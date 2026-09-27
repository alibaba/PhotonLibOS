/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

// Implementations of the harness.h helpers. Test-local: every blk test target
// compiles this file (see blk/test/CMakeLists.txt); it is not part of the
// library and not installed.

#include "harness.h"

#include "../utils.h"                     // blk::run_off_vcpu

#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>   // report() logs a std::string
#include <photon/common/utility.h>          // DEFER
#include <photon/fs/localfs.h>              // TestImage
#include <photon/photon.h>                  // get_event_engine / get_io_engine
#include <photon/thread/thread.h>           // thread_usleep, now

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifndef O_DIRECT
#define O_DIRECT 0   // macOS has none; the node transports are Linux-only, and
                     // test-nbd.cpp compiles this file on every platform
#endif

namespace photon {
namespace blk {
namespace test {

// ---------------------------------------------------------------------------
// file-local helpers
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t STRESS_CANON_TID = 0xffffffffu;

// DISJOINT mode: content owned by one thread, so a read-back must match the
// reader's own (tid, seq) exactly.
uint32_t stress_fill(uint32_t tid, uint32_t seq) {
    return ((tid * 31 + seq * 7 + 1) & 0xff) | 1;   // never 0: zeros = a hole
}

// SHARED mode: content is a function of the slot ONLY, never of the writer, so
// concurrent writes to one slot are byte-identical and ANY interleaving of
// them is a valid outcome. Without that, reading back another writer's block
// (or a mix of two writers' blocks) is legitimate block-layer behavior and
// would be indistinguishable from corruption.
uint32_t stress_canon_fill(uint64_t off, uint32_t len) {
    uint64_t h = (off >> 12) * 0x9e3779b97f4a7c15ull + (uint64_t)len * 0x2545f4914f6cdd1dull;
    return (uint32_t)((h >> 32) & 0xfe) | 1;
}

uint64_t stress_now_us() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

// The two modes share their bookkeeping, so it lives here: the workers only
// report IOs, bytes and the first error.
struct StressCounters {
    std::atomic<uint64_t> ios{0}, bytes{0};
    std::atomic<int> failures{0};
    char errbuf[320] = {};
    // benign race: the first writer wins, the rest see a non-empty buffer
    void note(const std::string& msg, uint64_t a = 0, uint64_t b = 0) {
        if (errbuf[0]) return;
        snprintf(errbuf, sizeof(errbuf), "%s (a=%llu b=%llu)", msg.c_str(),
                 (unsigned long long)a, (unsigned long long)b);
    }
};

int stress_open(const StressCfg& c, StressCounters& cnt) {
    int fd = open_node(c.node, O_RDWR | (c.direct ? O_DIRECT : 0));
    if (fd < 0 && c.direct && (errno == EINVAL || errno == EOPNOTSUPP))
        fd = open_node(c.node, O_RDWR);   // this node rejects O_DIRECT
    if (fd < 0) {
        cnt.note(std::string("open failed: ") + strerror(errno), (uint64_t)errno);
        cnt.failures++;
    }
    return fd;
}

// DISJOINT: thread t owns [base + t*region, +region) and mixes block sizes, so
// every read-back must return its own last write -- any other owner, offset or
// fill is a misrouted or torn IO.
StressResult stress_disjoint(const StressCfg& c, uint64_t span) {
    StressResult res;
    StressCounters cnt;
    static const size_t SIZES[] = {4096, 16384, 65536, 262144};
    std::vector<size_t> sizes;
    for (auto s : SIZES)
        if (s <= c.max_block) sizes.push_back(s);
    if (sizes.empty()) sizes.push_back(4096);

    uint64_t region = (span / (uint64_t)c.threads) & ~(uint64_t)4095;
    if (region < sizes.back() || region < sizeof(StressBlockHdr) + 8) {
        res.failures = 1;
        res.first_error = "the per-thread region is too small for the block sizes";
        return res;
    }

    std::vector<std::thread> ths;
    for (int t = 0; t < c.threads; t++)
        ths.emplace_back([&, t] {
            StressRnd rnd((c.seed ? c.seed : 0x9e3779b9u) ^ ((uint64_t)t * 0x2545f491u));
            int fd = stress_open(c, cnt);
            if (fd < 0) return;
            size_t cap = sizes.back();
            void* wbuf = nullptr;
            void* rbuf = nullptr;
            if (posix_memalign(&wbuf, 4096, cap) || posix_memalign(&rbuf, 4096, cap)) {
                cnt.note(std::string("posix_memalign failed: ") + strerror(errno));
                cnt.failures++;
                ::close(fd);
                return;
            }
            uint64_t base = c.base_off + (uint64_t)t * region;
            for (int i = 0; i < c.iters; i++) {
                size_t bs = std::min<size_t>(sizes[rnd.below(sizes.size())], region);
                uint64_t off = base + rnd.below((region - bs) / 4096 + 1) * 4096;
                stress_format(wbuf, bs, off, (uint32_t)t, (uint32_t)i + 1);
                if (::pwrite(fd, wbuf, bs, (off_t)off) != (ssize_t)bs) {
                    cnt.note(std::string("pwrite failed: ") + strerror(errno), off, bs);
                    cnt.failures++;
                    break;
                }
                if (c.flush && (i % 8) == 7 && ::fsync(fd) < 0) {
                    cnt.note(std::string("fsync failed: ") + strerror(errno), off, bs);
                    cnt.failures++;
                    break;
                }
                memset(rbuf, 0, bs);
                if (::pread(fd, rbuf, bs, (off_t)off) != (ssize_t)bs) {
                    cnt.note(std::string("pread failed: ") + strerror(errno), off, bs);
                    cnt.failures++;
                    break;
                }
                uint32_t tid = 0, seq = 0;
                int v = stress_validate(rbuf, bs, off, &tid, &seq);
                if (v == 0 && (tid != (uint32_t)t || seq != (uint32_t)i + 1))
                    v = EILSEQ;   // another thread's block inside OUR region
                if (v != 0) {
                    cnt.note(std::string("read-back does not validate: ") + strerror(v) +
                             " (owner tid=" + std::to_string(tid) + " seq=" +
                             std::to_string(seq) + ")", off, bs);
                    cnt.failures++;
                    break;
                }
                cnt.ios++;
                cnt.bytes += bs;
            }
            free(wbuf);
            free(rbuf);
            ::close(fd);
        });
    for (auto& th : ths) th.join();

    res.ios = cnt.ios.load();
    res.bytes = cnt.bytes.load();
    res.failures = cnt.failures.load();
    res.first_error = cnt.errbuf;
    return res;
}


// SHARED: one grid of fixed-size canonical slots that every thread hammers.
// The grid is poisoned first, so a lost first write is caught; afterwards all
// writes to a slot are byte-identical, which is what makes a read-back
// verifiable under unrestricted overlap.
StressResult stress_shared(const StressCfg& c, uint64_t span) {
    StressResult res;
    StressCounters cnt;
    size_t slot = (size_t)(c.max_block & ~(size_t)4095);
    if (slot < sizeof(StressBlockHdr) + 8) {
        res.failures = 1;
        res.first_error = "the slot size is too small for a block header";
        return res;
    }
    // bound the grid: the poison pass writes all of it, and contention wants
    // more threads than slots, not more slots
    uint64_t nslots = std::min<uint64_t>(span / slot, 32);
    if (nslots < 2) {
        res.failures = 1;
        res.first_error = "the span holds fewer than two slots";
        return res;
    }

    void* wbuf = nullptr;
    void* rbuf = nullptr;
    if (posix_memalign(&wbuf, 4096, slot) || posix_memalign(&rbuf, 4096, slot)) {
        res.failures = 1;
        res.first_error = std::string("posix_memalign failed: ") + strerror(errno);
        free(wbuf);
        free(rbuf);
        return res;
    }
    stress_format_poison(wbuf, slot);
    int fd = stress_open(c, cnt);
    if (fd >= 0) {
        for (uint64_t k = 0; k < nslots; k++) {
            off_t off = (off_t)(c.base_off + k * slot);
            if (::pwrite(fd, wbuf, slot, off) != (ssize_t)slot) {
                cnt.note(std::string("poison pwrite failed: ") + strerror(errno),
                         (uint64_t)off, slot);
                cnt.failures++;
                break;
            }
        }
        if (::fsync(fd) < 0 && !cnt.failures) {
            cnt.note(std::string("poison fsync failed: ") + strerror(errno));
            cnt.failures++;
        }
        ::close(fd);
    }
    if (cnt.failures.load()) {
        res.failures = cnt.failures.load();
        res.first_error = cnt.errbuf;
        free(wbuf);
        free(rbuf);
        return res;
    }

    std::vector<std::thread> ths;
    for (int t = 0; t < c.threads; t++)
        ths.emplace_back([&, t] {
            StressRnd rnd((c.seed ? c.seed : 0x2545f491u) ^ ((uint64_t)t * 0x9e3779b9u));
            int myfd = stress_open(c, cnt);
            if (myfd < 0) return;
            void* wb = nullptr;
            void* rb = nullptr;
            if (posix_memalign(&wb, 4096, slot) || posix_memalign(&rb, 4096, slot)) {
                cnt.note(std::string("posix_memalign failed: ") + strerror(errno));
                cnt.failures++;
                ::close(myfd);
                return;
            }
            for (int i = 0; i < c.iters; i++) {
                uint64_t off = c.base_off + rnd.below(nslots) * slot;
                stress_format_canonical(wb, slot, off);
                if (::pwrite(myfd, wb, slot, (off_t)off) != (ssize_t)slot) {
                    cnt.note(std::string("pwrite failed: ") + strerror(errno), off, slot);
                    cnt.failures++;
                    break;
                }
                if (c.flush && (i % 8) == 7 && ::fsync(myfd) < 0) {
                    cnt.note(std::string("fsync failed: ") + strerror(errno), off, slot);
                    cnt.failures++;
                    break;
                }
                memset(rb, 0, slot);
                if (::pread(myfd, rb, slot, (off_t)off) != (ssize_t)slot) {
                    cnt.note(std::string("pread failed: ") + strerror(errno), off, slot);
                    cnt.failures++;
                    break;
                }
                // concurrent writers of this slot produce identical bytes, so
                // an exact match is required -- anything else is the device's
                if (memcmp(wb, rb, slot) != 0) {
                    cnt.note(stress_diagnose(rb, slot, off), off, slot);
                    cnt.failures++;
                    break;
                }
                cnt.ios++;
                cnt.bytes += slot;
            }
            free(wb);
            free(rb);
            ::close(myfd);
        });
    for (auto& th : ths) th.join();

    free(wbuf);
    free(rbuf);
    res.ios = cnt.ios.load();
    res.bytes = cnt.bytes.load();
    res.failures = cnt.failures.load();
    res.first_error = cnt.errbuf;
    return res;
}

}  // namespace

// ---------------------------------------------------------------------------
// off-vcpu primitives
// ---------------------------------------------------------------------------

void run_off_vcpu(TempDelegate<void> fn) {
    // qualified: unqualified lookup from photon::blk::test finds THIS function
    blk::run_off_vcpu([&] { fn(); return 0; });
}

int open_node(const std::string& node, int mode, int tries) {
    int fd = -1;
    for (int i = 0; i < tries; i++) {
        fd = ::open(node.c_str(), mode);
        if (fd >= 0 || errno != ENXIO)
            break;
        ::usleep(5000);
    }
    return fd;
}

std::string sh_off_vcpu(const std::string& cmd, int* rc) {
    std::string out;
    int code = -1;
    run_off_vcpu([&] {
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) return;
        char buf[4096];
        size_t n;
        while ((n = ::fread(buf, 1, sizeof(buf), p)) > 0)
            out.append(buf, n);
        code = ::pclose(p);
    });
    if (rc) *rc = code;
    return out;
}

// ---------------------------------------------------------------------------
// consumer-side process isolation
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t CONSUMER_MAGIC = 0x434f4e53u;   // "CONS"

// Everything the child needs, all of it prepared by the parent before the fork
// and all of it POD. The child may not allocate, construct, log, or touch
// photon: it forked out of a multi-threaded process, so any lock another thread
// held at the fork instant stays held forever in the child, and an allocator or
// a logger taking one deadlocks -- probabilistically, which is worse than
// reliably. Keeping this a flat POD struct is what makes the child body
// auditable in one read.
struct ConsumerArgs {
    const char* path;        // NUL-terminated, outlives the fork
    int flags;               // open() flags
    int open_tries;          // >= 1
    int write;               // 0 = read only
    uint64_t off;
    size_t len;
    const void* wbuf;        // len bytes
    void* rbuf;              // len bytes
    ConsumerReport* rep;     // the shared mapping
};

// The bound of the fd census. NOT /proc/self/fd: opendir()/readdir() allocate,
// and the child must not. fcntl(F_GETFD) per candidate is a bare syscall.
//
// Capped, because the census runs on every consumer IO and RLIMIT_NOFILE is
// commonly 1M. The cap bounds the SELF-CHECK only -- drop_inherited_fds() below
// covers every fd however high -- and a drop that does not work at all shows up
// at the low end, where the daemon's control-device and ring fds are.
uint32_t fd_table_bound() {
    constexpr uint32_t CAP = 1u << 16;
    struct rlimit rl;
    if (::getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY)
        return CAP;
    return rl.rlim_cur > CAP ? CAP : (uint32_t)rl.rlim_cur;
}

uint32_t count_fds_from(uint32_t lo) {
    uint32_t hi = fd_table_bound(), n = 0;
    for (uint32_t i = lo; i < hi; i++)
        if (::fcntl((int)i, F_GETFD) == 0)
            n++;
    return n;
}

// Drop the whole inherited fd table -- the load-bearing step of the isolation,
// and necessary but NOT sufficient: harness.h records what else a fork copies
// that this cannot reach. Without it the child holds every descriptor the daemon
// has, its control device among them, and the fork would have changed the
// structure without changing the property -- the hardest kind of false fix to
// notice. So the caller does not trust this: it censuses afterwards and refuses
// to run the IO if anything survived.
int drop_inherited_fds() {
#ifdef __linux__
    return ::close_range(3u, ~0u, 0u);
#else
    // no close_range(2); every transport that exports a node is Linux-only, so
    // this branch only has to be correct enough to keep the file compiling
    uint32_t hi = fd_table_bound();
    for (uint32_t i = 3; i < hi; i++)
        if (::close((int)i) < 0 && errno != EBADF)
            return -1;
    return 0;
#endif
}

// The child's IO: bare syscalls, no allocation, no logging. Records the verdict
// in the report page and returns the exit code, which mirrors `status`.
int consumer_io_body(const ConsumerArgs* a) {
    ConsumerStage stage = CONS_DONE;
    int e = 0, status = 0, fd = -1;
    for (int i = 0; i < a->open_tries; i++) {
        fd = ::open(a->path, a->flags);
        if (fd >= 0 || errno != ENXIO)
            break;
        struct timespec ts{0, 5 * 1000 * 1000};   // 5 ms: the not-ready race
        ::nanosleep(&ts, nullptr);
    }
    if (fd < 0) {
        stage = CONS_OPEN;
        e = errno;
    } else if (a->write) {
        if (::pwrite(fd, a->wbuf, a->len, (off_t)a->off) != (ssize_t)a->len) {
            stage = CONS_WRITE; e = errno;
        } else if (::fsync(fd) < 0) {
            stage = CONS_SYNC; e = errno;
        }
    }
    if (fd >= 0 && stage == CONS_DONE &&
        ::pread(fd, a->rbuf, a->len, (off_t)a->off) != (ssize_t)a->len) {
        stage = CONS_READ; e = errno;
    }
    // the comparison has to happen HERE: the caller's copy of rbuf is a
    // different page now, and what the device returned never reaches it
    if (stage == CONS_DONE && a->write && ::memcmp(a->wbuf, a->rbuf, a->len) != 0) {
        stage = CONS_VERIFY; status = EILSEQ;
    }
    if (fd >= 0)
        ::close(fd);
    if (stage != CONS_DONE && status == 0)
        status = e ? e : EIO;
    ConsumerReport* rep = a->rep;
    rep->status = status;
    rep->stage = (int32_t)stage;
    rep->child_errno = e;
    rep->magic = CONSUMER_MAGIC;
    return status < CONS_EXIT_DROP_FDS ? status : EIO;
}

// Turn the child's exit status and report page into the caller's verdict.
// Shared by consumer_io() and consumer_reap(), because an abandoned child's
// report is only readable once it does exit.
void consumer_decode(ConsumerIoResult& r, const ConsumerReport* rep, int st,
                     bool have_status) {
    if (!have_status) {
        r.status = EIO;
        r.stage = CONS_EXIT;
        LOG_ERROR("the consumer child ` of ` was collected before it reported",
                  (int64_t)r.pid, r.node);
        return;
    }
    if (WIFSIGNALED(st)) {
        r.status = EIO;
        r.stage = CONS_EXIT;
        r.exit_code = WTERMSIG(st);
        LOG_ERROR("the consumer child ` of ` died on signal `",
                  (int64_t)r.pid, r.node, WTERMSIG(st));
        return;
    }
    if (!WIFEXITED(st)) {
        r.status = EIO;
        r.stage = CONS_EXIT;
        LOG_ERROR("the consumer child ` of ` ended in an unexpected wait status `",
                  (int64_t)r.pid, r.node, st);
        return;
    }
    int code = WEXITSTATUS(st);
    r.exit_code = code;
    if (rep->magic == CONSUMER_MAGIC) {
        r.status = rep->status;
        r.stage = (ConsumerStage)rep->stage;
        r.child_errno = rep->child_errno;
        r.fds = rep->fds;
    } else {
        r.status = code;   // the exit code mirrors `status`
        r.stage = CONS_EXIT;
    }
    // the two structural codes outrank the report: both say the isolation
    // itself is in question, which is a louder finding than any IO error
    if (code == CONS_EXIT_DROP_FDS || code == CONS_EXIT_FDLEAK) {
        r.status = EIO;
        r.stage = code == CONS_EXIT_DROP_FDS ? CONS_DROP_FDS : CONS_FDLEAK;
    }
    if (r.status)
        LOG_ERROR("consumer IO on ` failed at ` (exit `), ", r.node,
                  consumer_stage_name(r.stage), code,
                  ERRNO(r.child_errno ? r.child_errno : r.status));
}

// The parent's wait, bounded. Polls instead of blocking, and on expiry gives up
// on the child rather than signalling it: a process in an uninterruptible sleep
// cannot be killed, so a signal would only queue up beside the wedge while the
// caller -- the thing the isolation exists to protect -- stays stuck.
// Returns 1 if the child exited (`*st` holds its status), 0 if the deadline
// passed with it still running, -1 if it is gone without a status to read.
int consumer_wait(pid_t pid, uint64_t timeout_us, int* st, uint64_t* elapsed_us,
                  uint64_t poll_ns) {
    uint64_t t0 = stress_now_us(), deadline = t0 + timeout_us;
    int ret = 0;
    *st = 0;
    for (;;) {
        pid_t w = ::waitpid(pid, st, WNOHANG);
        if (w == pid) {
            ret = 1;
            break;
        }
        // EINTR: keep the deadline and poll again. Anything else is ECHILD, i.e.
        // collected behind our back, so no status exists to read.
        if (w < 0 && errno != EINTR) {
            *st = 0;
            ret = -1;
            break;
        }
        if (stress_now_us() >= deadline)
            break;
        struct timespec ts{(time_t)(poll_ns / 1000000000ull), (long)(poll_ns % 1000000000ull)};
        ::nanosleep(&ts, nullptr);
    }
    *elapsed_us = stress_now_us() - t0;
    return ret;
}

}  // namespace

const char* consumer_stage_name(ConsumerStage s) {
    switch (s) {
    case CONS_DONE:     return "done";
    case CONS_OPEN:     return "open";
    case CONS_WRITE:    return "pwrite";
    case CONS_SYNC:     return "fsync";
    case CONS_READ:     return "pread";
    case CONS_VERIFY:   return "the read-back comparison";
    case CONS_DROP_FDS: return "dropping the inherited fd table";
    case CONS_FDLEAK:   return "the fd-table self-check";
    case CONS_EXIT:     return "the child's exit";
    case CONS_FORK:     return "fork";
    case CONS_MAPPING:  return "mapping the report page";
    case CONS_TIMEOUT:  return "the caller's deadline";
    }
    return "?";
}

ConsumerIoResult consumer_io(const std::string& node, const void* wbuf, void* rbuf,
                             size_t len, uint64_t off, const ConsumerIoOpts& o) {
    ConsumerIoResult r;
    r.node = node;
    uint64_t timeout = o.timeout_us ? o.timeout_us : CONSUMER_TIMEOUT_US;
    size_t page = sizeof(ConsumerReport);
    void* m = ::mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        r.stage = CONS_MAPPING;
        r.child_errno = errno;
        r.status = errno ? errno : EIO;
        LOG_ERROR_RETURN(r.status, r, "failed to map the consumer's report page, ", ERRNO());
    }
    ConsumerReport* rep = (ConsumerReport*)m;
    memset(rep, 0, page);

    ConsumerArgs a;
    a.path = node.c_str();
    a.flags = (o.read_only ? O_RDONLY : O_RDWR) | (o.direct ? O_DIRECT : 0);
    a.open_tries = o.open_tries > 0 ? o.open_tries : 1;
    a.write = o.read_only ? 0 : 1;
    a.off = off;
    a.len = len;
    a.wbuf = wbuf;
    a.rbuf = rbuf;
    a.rep = rep;

    // The child inherits our stdio buffers and _exit() does not flush them, so
    // unflushed output would be lost or duplicated depending on who wrote last.
    // Flush everything first; the child itself uses no stdio at all.
    ::fflush(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        int e = errno;
        r.stage = CONS_FORK;
        r.child_errno = e;
        r.status = e ? e : EIO;
        ::munmap(rep, page);
        LOG_ERROR_RETURN(e, r, "failed to fork the consumer child for `, ", node, ERRNO(e));
    }
    if (pid == 0) {
        // ---- the child. From here to _exit(): bare syscalls only. ----
        // Condition 1 of the isolation, and its first action without exception.
        if (drop_inherited_fds() != 0)
            _exit(CONS_EXIT_DROP_FDS);
        // ...verified, not assumed: the census is the only thing that turns
        // condition 1 from a comment into a checked property.
        uint32_t left = count_fds_from(3);
        if (left != 0) {
            rep->stage = CONS_FDLEAK;
            rep->fds = left;
            rep->status = EIO;
            rep->magic = CONSUMER_MAGIC;
            _exit(CONS_EXIT_FDLEAK);
        }
        _exit(consumer_io_body(&a));
    }

    r.pid = pid;
    int st = 0;
    uint64_t elapsed = 0;
    int w = consumer_wait(pid, timeout, &st, &elapsed, 2 * 1000 * 1000);
    r.elapsed_us = elapsed;
    if (w == 0) {
        // Abandoned, still running, deliberately not killed. Keep the report
        // page mapped: the child may complete later and write into it, and
        // consumer_reap() is what finishes the job (consumer_release() gives up
        // on it and just unmaps).
        r.hung = true;
        r.status = ETIMEDOUT;
        r.stage = CONS_TIMEOUT;
        r.shm = rep;
        LOG_ERROR_RETURN(ETIMEDOUT, r, "consumer IO on ` did not finish within ` us; abandoning pid ` (an uninterruptible sleeper cannot be killed)",
                         node, timeout, (int64_t)pid);
    }
    r.reaped = w > 0;
    r.shm = nullptr;
    consumer_decode(r, rep, st, w > 0);
    ::munmap(rep, page);
    return r;
}

bool consumer_reap(ConsumerIoResult& r, uint64_t timeout_us) {
    if (r.reaped)
        return true;
    if (r.pid <= 0)
        LOG_ERROR_RETURN(EINVAL, false, "there is no consumer child to wait for");
    ConsumerReport* rep = (ConsumerReport*)r.shm;
    if (!rep)
        LOG_ERROR_RETURN(EINVAL, false, "the consumer child ` of ` has no report page left",
                         (int64_t)r.pid, r.node);
    int st = 0, w = 0;
    uint64_t elapsed = 0;
    // off the vcpu: it is the same bounded WNOHANG/nanosleep poll
    run_off_vcpu([&] {
        w = consumer_wait(r.pid, timeout_us ? timeout_us : CONSUMER_TIMEOUT_US,
                          &st, &elapsed, 10 * 1000 * 1000);
    });
    r.elapsed_us += elapsed;
    if (w == 0)
        LOG_ERROR_RETURN(ETIMEDOUT, false, "the abandoned consumer child ` of ` is still running after ` us more",
                         (int64_t)r.pid, r.node, elapsed);
    r.reaped = w > 0;
    r.hung = false;
    consumer_decode(r, rep, st, w > 0);
    consumer_release(r);
    return true;
}

void consumer_release(ConsumerIoResult& r) {
    if (r.shm) {
        ::munmap(r.shm, sizeof(ConsumerReport));
        r.shm = nullptr;
    }
}

// ---------------------------------------------------------------------------
// single-shot device IO and the deterministic pattern
// ---------------------------------------------------------------------------

std::vector<char> pattern(uint8_t seed, size_t n) {
    std::vector<char> b(n);
    for (size_t i = 0; i < n; i++)
        b[i] = (char)(seed + i * 131);
    return b;
}

int device_io(const std::string& node, const void* wbuf, size_t len,
              uint64_t off, const DeviceIoOpts& o) {
    // Allocated here, not in the child: the child allocates nothing. It is the
    // child's copy that receives the read-back, and the child is also where the
    // comparison runs, because that copy is not visible from here.
    std::vector<char> rbuf(len);
    ConsumerIoResult r;
    run_off_vcpu([&] {
        ConsumerIoOpts c;
        c.read_only = o.read_only;
        c.direct = o.direct;
        c.timeout_us = o.timeout_us;
        r = consumer_io(node, wbuf, rbuf.data(), len, off, c);
    });
    if (o.report)
        *o.report = r;
    else if (r.hung)
        consumer_release(r);   // nobody can reap a child whose pid was not kept
    if (r.status == 0 && o.backend && !o.read_only) {
        std::vector<char> bbuf(len);
        iovec iov{bbuf.data(), len};
        if (o.backend->preadv(&iov, 1, (off_t)off) != (ssize_t)len)
            return EIO;
        if (memcmp(wbuf, bbuf.data(), len))
            return EILSEQ;
    }
    return r.status;
}

// ---------------------------------------------------------------------------
// block formats
// ---------------------------------------------------------------------------

void stress_format(void* buf, size_t len, uint64_t off, uint32_t tid, uint32_t seq) {
    StressBlockHdr h{off, (uint32_t)len, tid, seq, stress_fill(tid, seq)};
    memcpy(buf, &h, sizeof(h));
    memset((char*)buf + sizeof(h), (int)h.fill, len - sizeof(h));
}

int stress_validate(const void* buf, size_t len, uint64_t off,
                    uint32_t* tid, uint32_t* seq) {
    if (len < sizeof(StressBlockHdr) + 8)
        return EINVAL;
    StressBlockHdr h;
    memcpy(&h, buf, sizeof(h));
    if (h.off != off || h.len != len || h.fill != stress_fill(h.tid, h.seq))
        return EILSEQ;
    const uint8_t* p = (const uint8_t*)buf + sizeof(h);
    size_t n = len - sizeof(h);
    uint8_t word[8];
    memset(word, (int)h.fill, sizeof(word));
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        if (memcmp(p + i, word, 8) != 0)
            return EILSEQ;
    for (; i < n; i++)
        if (p[i] != (uint8_t)h.fill)
            return EILSEQ;
    if (tid) *tid = h.tid;
    if (seq) *seq = h.seq;
    return 0;
}

void stress_format_canonical(void* buf, size_t len, uint64_t off) {
    StressBlockHdr h{off, (uint32_t)len, STRESS_CANON_TID, 0,
                     stress_canon_fill(off, (uint32_t)len)};
    memcpy(buf, &h, sizeof(h));
    memset((char*)buf + sizeof(h), (int)h.fill, len - sizeof(h));
}

void stress_format_poison(void* buf, size_t len) {
    StressBlockHdr h{~0ull, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xee};
    memcpy(buf, &h, sizeof(h));
    memset((char*)buf + sizeof(h), 0xee, len - sizeof(h));
}

std::string stress_diagnose(const void* got, size_t len, uint64_t off) {
    StressBlockHdr h;
    memcpy(&h, got, sizeof(h));
    if (h.off == ~0ull && h.fill == 0xee)
        return "the slot still holds its poison block: the write was lost";
    if (h.off != off || h.len != len)
        return "a foreign block (off=" + std::to_string(h.off) + ", len=" +
               std::to_string(h.len) + "): misrouted IO";
    if (h.fill != stress_canon_fill(h.off, h.len))
        return "the header disagrees with its own fill: corrupted IO";
    return "the header is right but the payload is not: torn IO";
}

// ---------------------------------------------------------------------------
// the stress driver
// ---------------------------------------------------------------------------

void StressResult::report(const char* what, const StressCfg& c) const {
    LOG_INFO("` stress: ` threads x ` iters, ` IOs, ` MiB, ` us, ` failures, first=`",
             what, c.threads, c.iters, DEC(ios).comma(true),
             DEC(bytes >> 20).comma(true), DEC(elapsed_us).comma(true),
             failures, first_error);
}


// Runs on the CALLING OS THREAD (it spawns and joins its own workers); from a
// coroutine use stress_off_vcpu() below.
StressResult stress_run(const StressCfg& c) {
    StressResult res;
    uint64_t span = (c.span ? c.span : (c.size > c.base_off ? c.size - c.base_off : 0))
                    & ~(uint64_t)4095;
    if (!c.size || span < 4096 || c.base_off + span > c.size) {
        res.failures = 1;
        res.first_error = "the span does not fit the device";
        return res;
    }
    uint64_t t0 = stress_now_us();
    res = c.mode == StressMode::SHARED ? stress_shared(c, span) : stress_disjoint(c, span);
    res.elapsed_us = stress_now_us() - t0;
    return res;
}


// Coroutine-safe wrapper: the driver runs entirely on OS threads while this
// vcpu keeps serving the device under test.
StressResult stress_off_vcpu(const StressCfg& c) {
    StressResult res;
    run_off_vcpu([&] { res = stress_run(c); });
    return res;
}

int stress_node_both_modes(const std::string& node, uint64_t size, const char* label,
                           int shared_threads) {
    std::string l_disjoint = std::string(label) + " disjoint";
    std::string l_shared = std::string(label) + " shared";
    StressCfg s;
    s.node = node;
    s.size = size;
    s.flush = true;
    auto disjoint = stress_off_vcpu(s);
    disjoint.report(l_disjoint.c_str(), s);

    s.mode = StressMode::SHARED;
    s.threads = shared_threads;
    s.flush = false;
    auto shared = stress_off_vcpu(s);
    shared.report(l_shared.c_str(), s);

    int failures = disjoint.failures + shared.failures;
    if (disjoint.failures)
        LOG_ERROR("` stress: `", l_disjoint, disjoint.first_error);
    if (shared.failures)
        LOG_ERROR("` stress: `", l_shared, shared.first_error);
    // a phase that completed no IO at all passed nothing, whatever its counter
    if (!disjoint.ios || !shared.ios) {
        LOG_ERROR("` stress moved no IO (disjoint ` / shared `)", label,
                  disjoint.ios, shared.ios);
        failures++;
    }
    return failures;
}

// ---------------------------------------------------------------------------
// fixture scaffolding
// ---------------------------------------------------------------------------

int TestImage::create(const char* p, uint64_t size) {
    path = p;
    ::unlink(p);
    lfs = fs::new_localfs_adaptor();
    if (!lfs)
        LOG_ERROR_RETURN(ENOMEM, -1, "failed to create the localfs adaptor");
    file = lfs->open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (!file) {
        int e = errno;
        LOG_ERROR("failed to create the test image `, ", p, ERRNO());
        delete lfs;
        lfs = nullptr;
        errno = e;
        return -1;
    }
    if (file->ftruncate(size) < 0) {
        int e = errno;
        LOG_ERROR("failed to size the test image ` to `, ", p, size, ERRNO());
        release();
        errno = e;
        return -1;
    }
    return 0;
}

void TestImage::release() {
    if (file) { delete file; file = nullptr; }
    if (lfs) { delete lfs; lfs = nullptr; }
}

// the unlink happens here, not in release(): the suites sweep their transport's
// residue in TearDown (which may still open the image), and only then remove it
TestImage::~TestImage() {
    release();
    if (!path.empty())
        ::unlink(path.c_str());
}

int expect_write_rejected(const std::string& node, uint64_t off, size_t len) {
    int rc = -1;
    run_off_vcpu([&] {
        // open(O_RDWR) on a read-only block device SUCCEEDS (verified with a
        // read-only loop device): the RO enforcement is at write time
        int fd = open_node(node, O_RDWR);
        if (fd < 0) {
            rc = errno ? errno : EIO;
            return;
        }
        DEFER(::close(fd));
        std::vector<char> b(len, 0);
        if (::pread(fd, b.data(), len, (off_t)off) != (ssize_t)len) {   // reads work
            rc = errno ? errno : EIO;
            return;
        }
        rc = ::pwrite(fd, b.data(), len, (off_t)off) < 0 ? 0 : EILSEQ;   // writes must not
    });
    return rc;
}

// ---------------------------------------------------------------------------
// the restart-window writer
// ---------------------------------------------------------------------------

int BackgroundWriter::start(const std::string& node, const Opts& o) {
    if (running)
        LOG_ERROR_RETURN(EALREADY, -1, "the background writer is already running");
    if (o.block < 4096)
        LOG_ERROR_RETURN(EINVAL, -1, "the writer's block size must be at least 4096");
    stop_flag = false;
    done = false;
    bad = 0;
    iter_count = 0;
    th = std::thread([this, node, o] {
        DEFER(done = true);
        int fd = open_node(node, O_RDWR | (o.direct ? O_DIRECT : 0));
        if (fd < 0 && o.direct && (errno == EINVAL || errno == EOPNOTSUPP))
            fd = open_node(node, O_RDWR);   // this node rejects O_DIRECT
        if (fd < 0) {
            LOG_ERROR("background writer: open ` failed, ", node, ERRNO());
            bad++;
            return;
        }
        DEFER(::close(fd));
        void* wbuf = nullptr;
        void* rbuf = nullptr;
        if (posix_memalign(&wbuf, 4096, o.block) ||
            (o.verify && posix_memalign(&rbuf, 4096, o.block))) {
            LOG_ERROR("background writer: posix_memalign failed, ", ERRNO());
            bad++;
            free(wbuf);
            return;
        }
        DEFER({ free(wbuf); free(rbuf); });
        uint64_t off = o.off;
        while (!stop_flag.load(std::memory_order_relaxed)) {
            uint64_t it = iter_count.load(std::memory_order_relaxed);
            memset(wbuf, (int)((it & 0xff) | 1), o.block);
            if (::pwrite(fd, wbuf, o.block, (off_t)off) != (ssize_t)o.block) { bad++; break; }
            if (o.verify) {
                if (::pread(fd, rbuf, o.block, (off_t)off) != (ssize_t)o.block) { bad++; break; }
                if (memcmp(wbuf, rbuf, o.block)) { bad++; break; }
            }
            iter_count.fetch_add(1, std::memory_order_relaxed);
            if (o.advance) {
                off += o.block;
                if (o.wrap_at && off + o.block > o.wrap_at)
                    off = o.off;
            }
        }
    });
    running = true;
    return 0;
}

void BackgroundWriter::stop() {
    if (!running)
        return;
    stop_flag = true;
    // poll coroutine-side until the thread is out of its IO; only then is join
    // instant instead of stalling the vcpu that serves that very IO
    while (!done.load())
        photon::thread_usleep(1000);
    th.join();
    running = false;
}

bool BackgroundWriter::wait_iters(uint64_t n, uint64_t timeout_us) {
    uint64_t deadline = photon::now + timeout_us;
    while (iter_count.load() < n) {
        if (photon::now >= deadline)
            return false;
        photon::thread_usleep(1000);
    }
    return true;
}

// ---------------------------------------------------------------------------
// vcpu placement observation
// ---------------------------------------------------------------------------

// The vcpu set is written from several OS threads at once (one per pool vcpu),
// so it needs a real lock. photon::mutex, not std::mutex: the callers are
// coroutines, and a std::mutex would stall a whole vcpu for the duration.
// The critical section is a linear scan of a <= 64-element vector plus at most
// one push_back, so contention is not a concern either way.
void RecordingFile::record() {
    // Ahead of m_lock, deliberately: the gate's whole point is that every gated IO
    // parks AT THE SAME TIME, so a caller can observe a device with its in-flight
    // count pinned at a cap. Waiting under the mutex would let them through one at
    // a time and the pinned state would never exist.
    if (gated) gate.wait(1);
    auto* v = photon::get_vcpu();
    SCOPED_LOCK(m_lock);
    for (auto* x : m_vcpus)
        if (x == v) return;
    m_vcpus.push_back(v);
}

// The accessors copy under the lock instead of handing out a reference: while
// the caller iterates, a pool vcpu could still be inside record()'s push_back.
std::vector<photon::vcpu_base*> RecordingFile::vcpus() {
    SCOPED_LOCK(m_lock);
    return m_vcpus;
}

size_t RecordingFile::vcpu_count() {
    SCOPED_LOCK(m_lock);
    return m_vcpus.size();
}

bool RecordingFile::ran_on(photon::vcpu_base* v) {
    SCOPED_LOCK(m_lock);
    for (auto* x : m_vcpus)
        if (x == v) return true;
    return false;
}

void RecordingFile::reset() {
    SCOPED_LOCK(m_lock);
    m_vcpus.clear();
}

photon::fs::IFileSystem* RecordingFile::filesystem() { return m_file->filesystem(); }
ssize_t RecordingFile::pread (void* buf, size_t count, off_t offset) { record(); return m_file->pread(buf, count, offset); }
ssize_t RecordingFile::preadv(const struct iovec* iov, int iovcnt, off_t offset) { record(); return m_file->preadv(iov, iovcnt, offset); }
ssize_t RecordingFile::pwrite(const void* buf, size_t count, off_t offset) { record(); return m_file->pwrite(buf, count, offset); }
ssize_t RecordingFile::pwritev(const struct iovec* iov, int iovcnt, off_t offset) { record(); return m_file->pwritev(iov, iovcnt, offset); }
ssize_t RecordingFile::pwritev2(const struct iovec* iov, int iovcnt, off_t offset, int flags) { record(); return m_file->pwritev2(iov, iovcnt, offset, flags); }
ssize_t RecordingFile::read  (void* buf, size_t count) { record(); return m_file->read(buf, count); }
ssize_t RecordingFile::readv (const struct iovec* iov, int iovcnt) { record(); return m_file->readv(iov, iovcnt); }
ssize_t RecordingFile::write (const void* buf, size_t count) { record(); return m_file->write(buf, count); }
ssize_t RecordingFile::writev(const struct iovec* iov, int iovcnt) { record(); return m_file->writev(iov, iovcnt); }
off_t RecordingFile::lseek(off_t offset, int whence) { return m_file->lseek(offset, whence); }
int RecordingFile::fsync() { record(); return m_file->fsync(); }
int RecordingFile::fdatasync() { record(); return m_file->fdatasync(); }
int RecordingFile::fchmod(mode_t mode) { return m_file->fchmod(mode); }
int RecordingFile::fchown(uid_t owner, gid_t group) { return m_file->fchown(owner, group); }
int RecordingFile::fstat(struct stat* buf) { return m_file->fstat(buf); }
int RecordingFile::ftruncate(off_t length) { record(); return m_file->ftruncate(length); }
int RecordingFile::fallocate(int mode, off_t offset, off_t len) { record(); return m_file->fallocate(mode, offset, len); }
int RecordingFile::close() { return m_file->close(); }

// Must be constructed on a photon vcpu: get_event_engine()/get_io_engine() are
// per-vcpu, and a gtest body runs on the caller's.
TestPool::TestPool(size_t n)
    : pool(new photon::WorkPool(n, (int)photon::get_event_engine(),
                                (int)photon::get_io_engine())) {}

TestPool::~TestPool() {
    delete pool;
}

int count_mq_dirs(const std::string& name) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/block/%s/mq", name.c_str());
    DIR* d = opendir(path);
    if (!d)
        return -1;
    DEFER(closedir(d));
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)))
        if (e->d_type == DT_DIR && e->d_name[0] != '.')
            n++;
    return n;
}

}  // namespace test
}  // namespace blk
}  // namespace photon
