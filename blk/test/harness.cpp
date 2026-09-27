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
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>            // _NSGetExecutablePath: no /proc there
#endif

// POSIX names this but leaves the declaration to the implementation, and not
// every <unistd.h> supplies it.
extern char** environ;

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
    // The hop exists so a blocking callable does not stall the vcpu that has to
    // serve the IO it is waiting on. A caller with no photon thread has no vcpu
    // to stall -- and cannot take the hop either. blk::run_off_vcpu hands the
    // result back on a photon::semaphore, and semaphore::wait writes through
    // photon::CURRENT to record what this waiter wants before it tests whether the
    // count is already available. CURRENT is a null __thread pointer in a thread
    // photon never initialized, so the hand-back dereferences null rather than
    // waiting, and a count that was already signalled does not save it: the write
    // comes first. Reading CURRENT in the guard below is safe: it takes the
    // pointer's value and dereferences nothing.
    if (!photon::CURRENT) {
        fn();
        return;
    }
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

// argv[1] of a consumer child: the sentinel every suite's main() dispatches on,
// ahead of photon::init() and of InitGoogleTest(). Not file-local, because a case
// that spawns a consumer child of its own has to spell it -- and spelling it
// wrong is not a failure the child reports. A child that does not see the
// sentinel is this suite running again inside a child, and every case in it that
// spawns one of its own does the same.
const char CONS_CHILD_ARG[] = "--photon-blk-consumer";

namespace {

constexpr uint32_t CONSUMER_MAGIC = 0x434f4e53u;   // "CONS"

// The fd the result channel arrives on. Both sides of the spawn agree on it here;
// the number also travels in argv, so the child does not have to.
constexpr int CONS_CHANNEL_FD = 3;

// The exit code a spawn leaves behind when the exec itself failed: the child
// never became our program, so it never mapped the channel and there is no
// report to read. 127 cannot be confused with a verdict, because a verdict
// always comes with the magic written.
constexpr int CONS_EXIT_EXEC_FAILED = 127;

// A stress phase is not one IO: it is `threads` workers each doing `iters`
// write+read-back pairs against a device this very process is serving. Measured
// across every phase the suites run, the slowest is 0.6 s, so this is a budget
// three orders of magnitude above the work -- not the single-IO one, and not a
// tight one either, because a budget that expires mid-phase abandons a child
// still holding the node. That is the outcome the isolation exists to make
// survivable rather than to make cheap, so it is worth waiting to avoid.
constexpr uint64_t CONSUMER_STRESS_TIMEOUT_US = 10ull * 60 * 1000 * 1000;

// The channel's layout, derived from (mode, len) by the ONE function below, which
// is what both sides call -- the parent with the mode and length it is handing
// over, the child with the mode and length from its argv. Everything is
// page-aligned: an O_DIRECT consumer gets buffers it can hand to the kernel as
// they are.
size_t page_align(size_t n) {
    return (n + 4095) & ~(size_t)4095;
}
// Both report PODs share this one region, in every mode. Reserving the stress one
// costs a single-IO child nothing: the two together are still well under the page
// this region was already rounded up to.
size_t channel_report_bytes() {
    return page_align(sizeof(ConsumerReport) + sizeof(ConsumerStressReport));
}
// The stress POD's offset, aligned for its widest member: ConsumerReport is 20
// bytes, so without this a uint64_t field of it would sit on a 4-byte boundary.
size_t channel_stress_off() {
    constexpr size_t A = alignof(ConsumerStressReport);
    return (sizeof(ConsumerReport) + A - 1) & ~(A - 1);
}
// A stress child allocates its own IO buffers -- each of its workers
// posix_memaligns a pair -- so the channel carries no payload for it.
size_t channel_io_bytes(int mode, size_t len) {
    return mode == CONS_MODE_STRESS ? 0 : page_align(len);
}

struct ChannelLayout {
    size_t report_bytes;   // the region holding both report PODs
    size_t stress_off;     // ConsumerStressReport, inside that region
    size_t wbuf_off;       // the caller's write payload
    size_t rbuf_off;       // the child's read-back
    size_t io_bytes;       // each of those two; 0 in stress mode
    size_t total;          // what ftruncate() and both mmap() calls take
};
ChannelLayout channel_layout(int mode, size_t len) {
    ChannelLayout l;
    l.report_bytes = channel_report_bytes();
    l.stress_off = channel_stress_off();
    l.io_bytes = channel_io_bytes(mode, len);
    l.wbuf_off = l.report_bytes;
    l.rbuf_off = l.wbuf_off + l.io_bytes;
    l.total = l.rbuf_off + l.io_bytes;
    return l;
}

// One fd-backed shared object, because exec leaves the child nothing of this
// address space: what it can see is what it is handed as a descriptor. Unnamed
// on Linux; the POSIX fallback's name is unlinked the instant it is opened, so
// neither leaves a path behind to race over, to clean up after an abort, or to
// fill a filesystem.
int channel_create(size_t size) {
#ifdef __linux__
    int fd = ::memfd_create("photon-blk-consumer", MFD_CLOEXEC);
    if (fd < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to create the consumer's result channel");
#else
    int fd = -1;
    for (int i = 0; i < 16 && fd < 0; i++) {
        char name[64];
        snprintf(name, sizeof(name), "/photon-blk-consumer-%d-%d", (int)::getpid(), i);
        fd = ::shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            ::shm_unlink(name);
    }
    if (fd < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to create the consumer's result channel");
#endif
    if (::ftruncate(fd, (off_t)size) != 0) {
        int e = errno;
        ::close(fd);
        LOG_ERROR_RETURN(e, -1, "failed to size the consumer's result channel to ` bytes", size);
    }
    return fd;
}

// The binary to spawn is this one: the consumer body lives in the harness that
// every suite already links, so re-executing the suite's own path is what makes
// the child's argv sentinel reachable from a main() that has done nothing yet.
bool self_exe(char* buf, size_t sz) {
#ifdef __linux__
    ssize_t n = ::readlink("/proc/self/exe", buf, sz - 1);
    if (n <= 0)
        LOG_ERRNO_RETURN(0, false, "failed to resolve this binary's own path");
    buf[n] = '\0';
    return true;
#elif defined(__APPLE__)
    uint32_t n = (uint32_t)sz;
    if (::_NSGetExecutablePath(buf, &n) != 0)
        LOG_ERROR_RETURN(ENAMETOOLONG, false, "this binary's own path needs ` bytes", n);
    return true;
#else
    (void)buf; (void)sz;
    LOG_ERROR_RETURN(ENOSYS, false, "no way to resolve this binary's own path here");
#endif
}

// A stress phase's decoded argv. POD, and a member of ConsumerArgs rather than a
// StressCfg, because ConsumerArgs is memset before the decode and StressCfg holds
// a std::string.
struct ConsumerStressArgs {
    int mode;                // StressMode
    int threads;             // worker OS threads the child spawns
    int iters;               // write+read-back pairs per worker
    uint64_t base_off;
    uint64_t span;           // the parent already checked it fits the device
    size_t max_block;
    int direct;              // O_DIRECT
    int flush;               // fsync every 8th iteration
    uint32_t seed;           // 0 = derive from the thread id
};

// Everything the child needs, all of it POD and all of it decoded from argv: the
// child shares no memory with the caller, so what does not travel in argv or in
// the channel does not exist for it.
struct ConsumerArgs {
    int mode;                // ConsumerMode: which body runs
    const char* path;        // NUL-terminated, in the child's own argv
    int channel_fd;          // where the result channel arrived
    // CONS_MODE_IO
    int flags;               // open() flags
    int open_tries;          // >= 1
    int write;               // 0 = read only
    uint64_t off;
    size_t len;
    // CONS_MODE_STRESS
    ConsumerStressArgs stress;
    // both, filled in from the mapping after the census
    const void* wbuf;        // len bytes, in the channel
    void* rbuf;              // len bytes, in the channel
    ConsumerReport* rep;     // offset 0 of the channel
    ConsumerStressReport* srep;   // channel_stress_off()
};

// The bound of the fd census. NOT /proc/self/fd: reading that directory needs an
// opendir(), whose own descriptor is inside the range being counted.
// fcntl(F_GETFD) per candidate is one syscall and opens nothing.
//
// Capped, because the census runs on every consumer IO and RLIMIT_NOFILE is
// commonly 1M. Two claims about what the cap bounds, kept apart because they are
// not the same one: on EVERY platform it bounds the self-check, since
// count_fds_from() below stops at it. On Linux it bounds nothing else, because
// drop_inherited_fds() there covers every fd however high; that function's
// non-Linux fallback loop shares this bound, so on those the cap bounds the drop
// as well. A drop that does not work at all shows up at the low end, where the
// descriptors this process opens early are.
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

// Drop the whole fd table the child was born with. The exec already closed
// every descriptor that set O_CLOEXEC, but this process does not set it on
// everything it opens, and a descriptor that reaches a consumer wedged on the
// node is a reference held for as long as that consumer lives. So the drop is
// unconditional -- and the census behind it is what keeps that from being a
// comment nobody checks.
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

bool parse_u64(const char* s, uint64_t* v) {
    char* end = nullptr;
    errno = 0;
    unsigned long long x = ::strtoull(s, &end, 10);
    if (errno || !end || end == s || *end)
        return false;
    *v = (uint64_t)x;
    return true;
}

bool parse_int(const char* s, int* v, int lo, int hi) {
    uint64_t x = 0;
    if (!parse_u64(s, &x) || x > (uint64_t)hi)   // hi <= INT_MAX at every call
        return false;
    *v = (int)x;
    return *v >= lo;
}

// argc per mode. A single shared value would let a stress child read single-IO
// fields out of the wrong slots and get plausible numbers back, which is exactly
// the silent mis-decode the argv path exists to make loud.
constexpr int CONS_ARGC_IO     = 10;
constexpr int CONS_ARGC_STRESS = 14;

// The child's argv: fixed order, decimal integers, node path LAST (see the table
// in harness.h). A mis-decode must not be able to read as "nothing to do", so it
// is its own exit code rather than a silent success.
bool decode_io_argv(int argc, char** argv, ConsumerArgs* a) {
    if (argc != CONS_ARGC_IO)
        return false;
    uint64_t off = 0, len = 0;
    if (!parse_int(argv[4], &a->flags, 0, INT_MAX) ||
        !parse_int(argv[5], &a->open_tries, 1, 1000000) ||
        !parse_int(argv[6], &a->write, 0, 1) ||
        !parse_u64(argv[7], &off) ||
        !parse_u64(argv[8], &len) ||
        !len || len > (1ull << 40) || !argv[9][0])
        return false;
    a->off = off;
    a->len = (size_t)len;
    a->path = argv[9];
    return true;
}

bool decode_stress_argv(int argc, char** argv, ConsumerArgs* a) {
    if (argc != CONS_ARGC_STRESS)
        return false;
    ConsumerStressArgs* s = &a->stress;
    uint64_t base_off = 0, span = 0, max_block = 0, seed = 0;
    // The span's alignment and nonzero-ness are not new restrictions: they are
    // what stress_run() derives and checks before it spawns, so any phase that
    // would have run satisfies them. base_off's alignment is StressCfg's own
    // documented contract, and an unaligned one cannot do O_DIRECT IO anyway.
    if (!parse_int(argv[4], &s->mode, (int)StressMode::DISJOINT, (int)StressMode::SHARED) ||
        !parse_int(argv[5], &s->threads, 1, 4096) ||
        !parse_int(argv[6], &s->iters, 1, 1000000) ||
        !parse_u64(argv[7], &base_off) ||
        !parse_u64(argv[8], &span) ||
        !parse_u64(argv[9], &max_block) ||
        !parse_int(argv[10], &s->direct, 0, 1) ||
        !parse_int(argv[11], &s->flush, 0, 1) ||
        !parse_u64(argv[12], &seed) ||
        (base_off & 4095) || !span || (span & 4095) || !max_block ||
        seed > 0xffffffffull || !argv[13][0])
        return false;
    s->base_off = base_off;
    s->span = span;
    s->max_block = (size_t)max_block;
    s->seed = (uint32_t)seed;
    a->path = argv[13];
    return true;
}

bool decode_child_argv(int argc, char** argv, ConsumerArgs* a) {
    // The mode tag selects the rest of the table, so it is read ahead of either
    // half -- and an unknown tag is a mis-decode like any other, not a default.
    if (argc < 4 ||
        !parse_int(argv[2], &a->mode, CONS_MODE_IO, CONS_MODE_STRESS) ||
        !parse_int(argv[3], &a->channel_fd, 3, 65535))
        return false;
    return a->mode == CONS_MODE_STRESS ? decode_stress_argv(argc, argv, a)
                                       : decode_io_argv(argc, argv, a);
}

// The child's IO. Records the verdict in the channel and returns the exit code,
// which mirrors `status`.
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
    // the comparison has to happen HERE: the read-back lands in the channel,
    // and a child that is abandoned never gets its bytes copied out to the caller
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

// The child's stress phase -- the whole loop, not one IO of it. Each of the
// phase's workers opens the node for itself, so the phase is the unit that can
// end up wedged on it, and the unit whose descriptors and mappings must not be
// the daemon's.
//
// The exit code is 0 whenever the phase ran at all, however many of its IOs
// failed: what a phase has to say is how many it moved and what the first failure
// was, and that travels in ConsumerStressReport. A nonzero status is reserved for
// "this child never got as far as its phase", which is what tells the parent not
// to read the counters it is about to be handed.
int consumer_stress_body(const ConsumerArgs* a) {
    StressCfg c;
    c.node = a->path;
    c.threads = a->stress.threads;
    c.iters = a->stress.iters;
    c.base_off = a->stress.base_off;
    c.max_block = a->stress.max_block;
    c.mode = (StressMode)a->stress.mode;
    c.direct = a->stress.direct != 0;
    c.flush = a->stress.flush != 0;
    c.seed = a->stress.seed;
    // `size` and `span` stay at their defaults deliberately: the parent checked
    // the span against the device before it spawned -- a configuration check
    // needs no descriptor -- and the phases take the checked span as a parameter
    // and read neither field.
    StressResult res = c.mode == StressMode::SHARED ? stress_shared(c, a->stress.span)
                                                    : stress_disjoint(c, a->stress.span);
    ConsumerStressReport* s = a->srep;
    s->ios = res.ios;
    s->bytes = res.bytes;
    s->failures = res.failures;
    size_t n = res.first_error.size();
    if (n > sizeof(s->errbuf) - 1)
        n = sizeof(s->errbuf) - 1;
    memcpy(s->errbuf, res.first_error.c_str(), n);
    s->errbuf[n] = '\0';
    ConsumerReport* rep = a->rep;
    rep->status = 0;
    rep->stage = CONS_DONE;
    rep->child_errno = 0;
    rep->magic = CONSUMER_MAGIC;   // last: it is what says the rest is final
    return 0;
}

// The structural exit codes, which say the isolation itself is in question and
// so outrank anything an IO could report. CONS_EXIT for a plain status.
ConsumerStage exit_code_stage(int code) {
    switch (code) {
    case CONS_EXIT_DROP_FDS: return CONS_DROP_FDS;
    case CONS_EXIT_FDLEAK:   return CONS_FDLEAK;
    case CONS_EXIT_ARGV:     return CONS_ARGV;
    case CONS_EXIT_CHANNEL:  return CONS_CHANNEL;
    default:                 return CONS_EXIT;
    }
}

// Turn the child's exit status and report into the caller's verdict. Shared by
// consumer_io() and consumer_reap(), because an abandoned child's report is only
// readable once it does exit.
//
// A child that died without writing a word -- the exec failed, its argv did not
// decode, a signal took it -- has to stay distinguishable from one that reported
// failure. The channel cannot say that (an unwritten page looks the same as a
// page nobody ever mapped), so the wait status decides: the magic is only ever
// written last, next to a final status.
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
    } else if (code == CONS_EXIT_EXEC_FAILED) {
        r.status = EIO;
        r.stage = CONS_EXEC;
    } else {
        r.status = code;   // the exit code mirrors `status`
        r.stage = CONS_EXIT;
    }
    if (exit_code_stage(code) != CONS_EXIT) {
        r.status = EIO;
        r.stage = exit_code_stage(code);
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

// The spawn, the bounded wait and the decode that every consumer child goes
// through: consumer_io()'s, the stress driver's, and the malformed-argv
// witnesses'. ONE copy, deliberately -- they differ in the argv they hand over
// and in what they read back out of the channel, and in nothing else. A witness
// that spawned its own child would measure libc's exit codes instead of this
// path's mapping of them, which is the thing worth witnessing.
struct ConsumerSpawn {
    std::string node;                  // r.node, and the log lines
    std::vector<const char*> argv;     // argv[1..], the sentinel first; argv[0]
                                       // and the terminator are added here
    ChannelLayout layout;              // the channel's, from channel_layout()
    const void* wbuf = nullptr;        // copied into the channel before the spawn
    void* rbuf = nullptr;              // the read-back is copied out into it
    size_t len = 0;                    // both buffers'; 0 copies neither
    ConsumerStressReport* stress = nullptr;   // filled from the channel if reaped
    uint64_t timeout_us = 0;           // 0 = CONSUMER_TIMEOUT_US
};

ConsumerIoResult consumer_spawn(ConsumerSpawn& s) {
    ConsumerIoResult r;
    r.node = s.node;
    uint64_t timeout = s.timeout_us ? s.timeout_us : CONSUMER_TIMEOUT_US;
    size_t size = s.layout.total;

    int cfd = channel_create(size);
    if (cfd < 0) {
        r.stage = CONS_MAPPING;
        r.child_errno = errno;
        r.status = errno ? errno : EIO;
        LOG_ERROR_RETURN(r.status, r, "failed to create the result channel for consumer IO on `, ",
                         s.node, ERRNO());
    }
    // The parent's own mapping is what keeps the channel alive from here on, so
    // the descriptor can go as soon as the spawn has handed it over. Until then
    // the early returns below still have to close it.
    bool spawned = false;
    DEFER(if (!spawned) ::close(cfd));
    void* base = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
    if (base == MAP_FAILED) {
        r.stage = CONS_MAPPING;
        r.child_errno = errno;
        r.status = errno ? errno : EIO;
        LOG_ERROR_RETURN(r.status, r, "failed to map the result channel for consumer IO on `, ",
                         s.node, ERRNO());
    }
    // ... unless an abandoned child may still be writing into it, in which case
    // the caller owns it until consumer_reap() or consumer_release()
    bool handed_over = false;
    DEFER(if (!handed_over) ::munmap(base, size));

    ConsumerReport* rep = (ConsumerReport*)base;
    memset(rep, 0, s.layout.report_bytes);   // magic 0 until the child's last store
    if (s.wbuf && s.len)
        memcpy((char*)base + s.layout.wbuf_off, s.wbuf, s.len);

    char exe[PATH_MAX];
    if (!self_exe(exe, sizeof(exe))) {
        r.stage = CONS_SPAWN;
        r.child_errno = errno;
        r.status = errno ? errno : EIO;
        LOG_ERROR_RETURN(r.status, r, "cannot spawn the consumer child for ` without this binary's own path, ",
                         s.node, ERRNO());
    }

    std::vector<char*> av;
    av.reserve(s.argv.size() + 2);
    av.push_back(exe);
    for (const char* x : s.argv)
        av.push_back((char*)x);   // posix_spawn's signature; it does not write
    av.push_back(nullptr);

    // Every posix_spawn* call from here down -- the setup ones and posix_spawn
    // itself -- reports failure as its own return value; POSIX does not ask any
    // of them to set errno, so `e` is always taken from the call.
    int e = 0;
    posix_spawn_file_actions_t fa;
    if ((e = ::posix_spawn_file_actions_init(&fa)) != 0) {
        r.stage = CONS_SPAWN; r.child_errno = e; r.status = e;
        LOG_ERROR_RETURN(e, r, "failed to prepare the spawn of the consumer child for `, ",
                         s.node, ERRNO(e));
    }
    DEFER(::posix_spawn_file_actions_destroy(&fa));
    // adddup2(x, x) is a no-op by POSIX, so with the channel's own close-on-exec
    // still set the exec would close it and the child would have nothing to
    // report through. Clearing it here -- only in the collision case -- keeps
    // that from depending on how a libc spells the no-op.
    if (cfd == CONS_CHANNEL_FD)
        ::fcntl(cfd, F_SETFD, 0);
    if ((e = ::posix_spawn_file_actions_adddup2(&fa, cfd, CONS_CHANNEL_FD)) != 0) {
        r.stage = CONS_SPAWN; r.child_errno = e; r.status = e;
        LOG_ERROR_RETURN(e, r, "failed to hand the result channel to the consumer child for `, ",
                         s.node, ERRNO(e));
    }
    // Not the suite's stdout/stderr. A consumer child that outlives the suite --
    // which is what an abandoned one does -- would otherwise hold the write end
    // of whatever pipe the suite is being captured through, and the capture would
    // never see EOF. It also cannot corrupt the gtest output, which it produces
    // none of: its verdict travels in the channel.
    const char* devnull = "/dev/null";
    if ((e = ::posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, devnull, O_WRONLY, 0)) != 0 ||
        (e = ::posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, devnull, O_WRONLY, 0)) != 0) {
        r.stage = CONS_SPAWN; r.child_errno = e; r.status = e;
        LOG_ERROR_RETURN(e, r, "failed to detach the consumer child of ` from this process's stdio, ",
                         s.node, ERRNO(e));
    }
    posix_spawnattr_t attr;
    if ((e = ::posix_spawnattr_init(&attr)) != 0) {
        r.stage = CONS_SPAWN; r.child_errno = e; r.status = e;
        LOG_ERROR_RETURN(e, r, "failed to prepare the spawn of the consumer child for `, ",
                         s.node, ERRNO(e));
    }
    DEFER(::posix_spawnattr_destroy(&attr));
    // Not the caller's signal mask: a blocked signal the consumer inherits is
    // one it can never handle, and nothing about a consumer IO wants one.
    sigset_t no_signals;
    sigemptyset(&no_signals);
    if ((e = ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK)) != 0 ||
        (e = ::posix_spawnattr_setsigmask(&attr, &no_signals)) != 0) {
        r.stage = CONS_SPAWN; r.child_errno = e; r.status = e;
        LOG_ERROR_RETURN(e, r, "failed to prepare the spawn of the consumer child for `, ",
                         s.node, ERRNO(e));
    }

    pid_t pid = -1;
    if ((e = ::posix_spawn(&pid, exe, &fa, &attr, av.data(), environ)) != 0) {
        r.stage = CONS_SPAWN;
        r.child_errno = e;
        r.status = e;
        LOG_ERROR_RETURN(e, r, "failed to spawn the consumer child for `, ", s.node, ERRNO(e));
    }
    spawned = true;
    ::close(cfd);   // promptly: the collision case above cleared its CLOEXEC

    r.pid = pid;
    int st = 0;
    uint64_t elapsed = 0;
    int w = consumer_wait(pid, timeout, &st, &elapsed, 2 * 1000 * 1000);
    r.elapsed_us = elapsed;
    if (w == 0) {
        // Abandoned, still running, deliberately not killed.
        handed_over = true;
        r.hung = true;
        r.status = ETIMEDOUT;
        r.stage = CONS_TIMEOUT;
        r.shm = base;
        r.shm_size = size;
        LOG_ERROR_RETURN(ETIMEDOUT, r, "consumer IO on ` did not finish within ` us; abandoning pid ` (an uninterruptible sleeper cannot be killed)",
                         s.node, timeout, (int64_t)pid);
    }
    r.reaped = w > 0;
    consumer_decode(r, rep, st, w > 0);
    if (r.reaped) {
        if (s.rbuf && s.len)
            memcpy(s.rbuf, (char*)base + s.layout.rbuf_off, s.len);
        if (s.stress)
            *s.stress = *(const ConsumerStressReport*)((char*)base + s.layout.stress_off);
    }
    return r;
}

// The parent's half of a stress phase: hand the config over in argv, wait for the
// child with a deadline of its own, and read the counters back out of the
// channel. Everything that touches the node happens in the child.
StressResult stress_in_child(const StressCfg& c, uint64_t span) {
    StressResult res;
    // argv, in decode_stress_argv's order, path last
    char a_mode[4], a_fd[12], a_sm[4], a_th[12], a_it[12], a_base[24], a_span[24];
    char a_blk[24], a_direct[4], a_flush[4], a_seed[16];
    snprintf(a_mode, sizeof(a_mode), "%d", (int)CONS_MODE_STRESS);
    snprintf(a_fd, sizeof(a_fd), "%d", CONS_CHANNEL_FD);
    snprintf(a_sm, sizeof(a_sm), "%d", (int)c.mode);
    snprintf(a_th, sizeof(a_th), "%d", c.threads);
    snprintf(a_it, sizeof(a_it), "%d", c.iters);
    snprintf(a_base, sizeof(a_base), "%llu", (unsigned long long)c.base_off);
    snprintf(a_span, sizeof(a_span), "%llu", (unsigned long long)span);
    snprintf(a_blk, sizeof(a_blk), "%llu", (unsigned long long)c.max_block);
    snprintf(a_direct, sizeof(a_direct), "%d", c.direct ? 1 : 0);
    snprintf(a_flush, sizeof(a_flush), "%d", c.flush ? 1 : 0);
    snprintf(a_seed, sizeof(a_seed), "%u", c.seed);

    ConsumerStressReport srep;
    memset(&srep, 0, sizeof(srep));
    ConsumerSpawn s;
    s.node = c.node;
    s.argv = {CONS_CHILD_ARG, a_mode, a_fd, a_sm, a_th, a_it, a_base, a_span, a_blk,
              a_direct, a_flush, a_seed, c.node.c_str()};
    s.layout = channel_layout(CONS_MODE_STRESS, 0);
    s.stress = &srep;
    s.timeout_us = CONSUMER_STRESS_TIMEOUT_US;
    ConsumerIoResult r = consumer_spawn(s);
    // Nothing keeps this child's pid, so a channel handed over for a later reap
    // has nowhere to go -- release it, as device_io() does in the same situation.
    if (r.shm)
        consumer_release(r);
    if (r.status != 0 || r.stage != CONS_DONE) {
        // The phase never ran, or its counters are not readable. Name the culprit
        // instead of reporting the zeros a channel is born with, which would read
        // as "a clean phase that moved no IO".
        res.failures = 1;
        res.first_error = std::string("the consumer child never reported a stress phase: ") +
                          consumer_stage_name(r.stage);
        LOG_ERROR_RETURN(EIO, res, "` stress: the consumer child ` never reported the phase, stage `, status `, exit `",
                         c.node, (int64_t)r.pid, consumer_stage_name(r.stage), r.status, r.exit_code);
    }
    res.ios = srep.ios;
    res.bytes = srep.bytes;
    res.failures = srep.failures;
    res.first_error.assign(srep.errbuf, strnlen(srep.errbuf, sizeof(srep.errbuf)));
    return res;
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
    case CONS_ARGV:     return "decoding its argv";
    case CONS_CHANNEL:  return "mapping the result channel";
    case CONS_EXEC:     return "the exec of the consumer child";
    case CONS_EXIT:     return "the child's exit";
    case CONS_SPAWN:    return "posix_spawn";
    case CONS_MAPPING:  return "creating the result channel";
    case CONS_TIMEOUT:  return "the caller's deadline";
    }
    return "?";
}

int consumer_child_main(int argc, char** argv) {
    if (argc < 2 || ::strcmp(argv[1], CONS_CHILD_ARG) != 0)
        return CONS_NOT_A_CHILD;

    ConsumerArgs a;
    memset(&a, 0, sizeof(a));
    if (!decode_child_argv(argc, argv, &a))
        _exit(CONS_EXIT_ARGV);

    ChannelLayout l = channel_layout(a.mode, a.len);
    // (a) map the channel from the descriptor the spawn handed us ...
    void* base = ::mmap(nullptr, l.total, PROT_READ | PROT_WRITE, MAP_SHARED, a.channel_fd, 0);
    if (base == MAP_FAILED)
        _exit(CONS_EXIT_CHANNEL);
    // (b) ... then drop the fd table, which closes that descriptor. The mapping
    // is what survives the drop, and that is why the verdict travels in a
    // mapping rather than in the descriptor it arrived on.
    if (drop_inherited_fds() != 0)
        _exit(CONS_EXIT_DROP_FDS);
    // (c) verified, not assumed: the census is the only thing that turns the
    // drop from a comment into a checked property. It runs after (b) and before
    // any IO, so what it counts is what the child was BORN with, not what the
    // IO opened.
    ConsumerReport* rep = (ConsumerReport*)base;
    uint32_t left = count_fds_from(3);
    if (left != 0) {
        rep->stage = CONS_FDLEAK;
        rep->fds = left;
        rep->status = EIO;
        rep->magic = CONSUMER_MAGIC;
        _exit(CONS_EXIT_FDLEAK);
    }
    a.rep = rep;
    a.srep = (ConsumerStressReport*)((char*)base + l.stress_off);
    // A stress child has no payload region, so leaving these pointing one past
    // its mapping would be a trap for whoever next adds a read to that body.
    if (l.io_bytes) {
        a.wbuf = (char*)base + l.wbuf_off;
        a.rbuf = (char*)base + l.rbuf_off;
    }
    // (d) the body, and (e) the exit
    _exit(a.mode == CONS_MODE_STRESS ? consumer_stress_body(&a) : consumer_io_body(&a));
}

ConsumerIoResult consumer_io(const std::string& node, const void* wbuf, void* rbuf,
                             size_t len, uint64_t off, const ConsumerIoOpts& o) {
    // argv, in decode_io_argv's order. Decimal integers and the path last, so
    // nothing here needs quoting and nothing can be ambiguous about where a
    // field ended.
    char a_mode[4], a_fd[12], a_flags[16], a_tries[16], a_write[4], a_off[24], a_len[24];
    snprintf(a_mode, sizeof(a_mode), "%d", (int)CONS_MODE_IO);
    snprintf(a_fd, sizeof(a_fd), "%d", CONS_CHANNEL_FD);
    snprintf(a_flags, sizeof(a_flags), "%d",
             (o.read_only ? O_RDONLY : O_RDWR) | (o.direct ? O_DIRECT : 0));
    snprintf(a_tries, sizeof(a_tries), "%d", o.open_tries > 0 ? o.open_tries : 1);
    snprintf(a_write, sizeof(a_write), "%d", o.read_only ? 0 : 1);
    snprintf(a_off, sizeof(a_off), "%llu", (unsigned long long)off);
    snprintf(a_len, sizeof(a_len), "%llu", (unsigned long long)len);

    ConsumerSpawn s;
    s.node = node;
    s.argv = {CONS_CHILD_ARG, a_mode, a_fd, a_flags, a_tries, a_write, a_off, a_len,
              node.c_str()};
    s.layout = channel_layout(CONS_MODE_IO, len);
    s.wbuf = wbuf;
    s.rbuf = rbuf;
    s.len = len;
    s.timeout_us = o.timeout_us;
    return consumer_spawn(s);
}

ConsumerIoResult consumer_spawn_argv(const std::vector<std::string>& argv, uint64_t timeout_us) {
    ConsumerIoResult refused;
    refused.node = argv.empty() ? std::string("?") : argv[0];
    // Refused rather than spawned: see CONS_CHILD_ARG for what a child that does
    // not see the sentinel is.
    if (argv.empty() || argv[0] != CONS_CHILD_ARG) {
        refused.status = EINVAL;
        refused.stage = CONS_ARGV;
        LOG_ERROR_RETURN(EINVAL, refused, "a consumer child's argv must start with the sentinel `, not `",
                         CONS_CHILD_ARG, refused.node);
    }

    ConsumerSpawn s;
    // The report page alone. A child whose argv does not decode never reaches an
    // IO region, and one that does would size it from the very argv this call is
    // malformed on purpose -- so there is no length here that could be right.
    s.layout = channel_layout(CONS_MODE_IO, 0);
    s.node = refused.node;
    s.timeout_us = timeout_us;
    for (const auto& a : argv)
        s.argv.push_back(a.c_str());
    return consumer_spawn(s);
}

bool consumer_reap(ConsumerIoResult& r, uint64_t timeout_us) {
    if (r.reaped)
        return true;
    if (r.pid <= 0)
        LOG_ERROR_RETURN(EINVAL, false, "there is no consumer child to wait for");
    ConsumerReport* rep = (ConsumerReport*)r.shm;
    if (!rep)
        LOG_ERROR_RETURN(EINVAL, false, "the consumer child ` of ` has no result channel left",
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
    // Only a collected child is known to have stopped running. -1 means it is gone
    // without a status to read, which says nothing about when it stopped, so
    // `hung` is left as it was and consumer_decode's own log carries the diagnosis.
    if (r.reaped)
        r.hung = false;
    consumer_decode(r, rep, st, w > 0);
    consumer_release(r);
    return r.reaped;
}

void consumer_release(ConsumerIoResult& r) {
    if (r.shm) {
        ::munmap(r.shm, r.shm_size);
        r.shm = nullptr;
        r.shm_size = 0;
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
    // The read-back lands in the result channel and is copied here once the
    // child is collected; the comparison against `wbuf` runs in the child, which
    // is the only side that sees both.
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


// Runs on the CALLING OS THREAD: it spawns the child that runs the phase and
// then polls for it with a deadline of its own. From a coroutine use
// stress_off_vcpu() below.
StressResult stress_run(const StressCfg& c) {
    StressResult res;
    uint64_t span = (c.span ? c.span : (c.size > c.base_off ? c.size - c.base_off : 0))
                    & ~(uint64_t)4095;
    // A configuration check rather than IO: it reads no descriptor, so it stays
    // here -- a bad config should not cost a spawn, and it should not come back
    // wearing the stage of a child that ran and then hit it.
    if (!c.size || span < 4096 || c.base_off + span > c.size) {
        res.failures = 1;
        res.first_error = "the span does not fit the device";
        return res;
    }
    uint64_t t0 = stress_now_us();
    res = stress_in_child(c, span);
    // Measured here, around the spawn, so it now includes the spawn and the exec
    // of the child that ran the phase as well as the phase itself.
    res.elapsed_us = stress_now_us() - t0;
    return res;
}


// Coroutine-safe wrapper: the spawn and the bounded wait both run on an OS thread
// while this vcpu keeps serving the device under test.
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
    if (gated) {
        // wait_interruptible, not wait: tearing a serving queue down interrupts
        // every coroutine still parked in the backend and expects it to leave at
        // that point, and semaphore::wait is documented as the uninterruptible
        // wrapper -- it swallows the interrupt and parks again, so a tag held here
        // never leaves and the teardown waiting for it never ends. An interrupt
        // only reaches this point while the device is going away, when where the IO
        // ran is no longer an observation worth keeping.
        if (gate.wait_interruptible(1) < 0)
            return;
    }
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
