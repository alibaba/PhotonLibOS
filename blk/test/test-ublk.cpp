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

// Built only on Linux (the ublk transport is LINUX-gated). Requires root and
// the ublk_drv module (the fixture tries modprobe); without them every case
// prints a skip notice and returns (see test::SkippableTest).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // O_DIRECT
#endif

#include "../blk.h"

#include "../../test/gtest.h"
#include "harness.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>
#include <photon/common/utility.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/fs.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace photon {
namespace blk {

static const char IMG_PATH[]       = "/tmp/photon-blk-ublk.img";
static const char IMG2_PATH[]      = "/tmp/photon-blk-ublk2.img";
static constexpr uint64_t IMG_SIZE = 64ull << 20;
static const char TEST_IDENTITY[]  = "photon-ublk-test";   // informational (ublk keys on dev_id)

static constexpr uint64_t IO_OFF = 1ull << 20;   // 1 MiB
static constexpr size_t   IO_LEN = 256ull << 10; // 256 KiB

// parse the dev_id out of "/dev/ublkb<N>"
static uint32_t node_dev_id(const char* node) {
    const char* p = strrchr(node, 'b');
    return p ? (uint32_t)atoi(p + 1) : UINT32_MAX;
}

// get_device_node() yields nullptr when there is no node, and a started ublk
// device always has one -- assert that here rather than letting a nullptr reach
// a std::string, which is UB and not a readable test failure
static std::string node_of(IBlkDevice* d) {
    const char* n = d->get_device_node();
    EXPECT_NE(nullptr, n);
    return n ? n : "";
}

// The lock directory every controller in this suite is built on. Stated here rather
// than left to a library default, because there is no default: a directory two
// applications share lets each one's orphan scan adopt the other's devices. Every
// tombstone path this file checks is built from it, so the suite cannot drift onto
// one directory for its controllers and probe another.
static const char SUITE_LOCKS[] = "/run/photon-blk";

// The geometry every config in this file starts from. At file scope rather than a
// fixture member because two fixtures need it: the gated one, and the ungated one
// covering what a constructed device already knows before anything reaches the
// kernel. One definition, so the two cannot drift onto different geometries.
static BlkDevInfo make_info() {
    BlkDevInfo i;
    i.identity = TEST_IDENTITY;
    i.size = IMG_SIZE;
    i.sector_size_shift = 9;
    i.features = FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
    return i;
}

// Everything a start() that was refused after it had already talked to the
// kernel could leave behind, as one sortable listing: the ublk nodes under /dev,
// and each of this suite's flock files marked HELD or FREE. The lock FILE is
// expected to survive -- devlock_release() unlocks and closes, it never unlinks,
// and the orphan scan keys on "file exists and is free" -- so what must not
// survive is a lock still held, which would wedge every later run.
static std::string residue() {
    return test::sh_off_vcpu(
        "{ ls -1 /dev 2>/dev/null | grep '^ublk';"
        "  for f in " + std::string(SUITE_LOCKS) +
        "/ublk-*.lock; do [ -e \"$f\" ] || continue;"
        "    flock -n \"$f\" -c true 2>/dev/null && echo \"FREE $f\" || echo \"HELD $f\"; done;"
        "} | sort");
}

// How many descriptors this process holds. What it is for: a claim or a control
// channel that a call takes and forgets to give back is invisible to every other
// oracle in this suite, because the tombstone it locked has usually been unlinked
// by then -- a flock on a deleted inode shows up in no listing, and residue() reads
// the directory, not the fd table. Counting the table does. Measured the same way on
// both sides so the descriptor opendir() itself takes cancels out.
static int fd_count() {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    while (readdir(d))
        n++;
    ::closedir(d);
    return n;
}

// The kernel's own count of requests outstanding on the node, reads plus writes.
// An oracle that reads nothing this suite wrote, which is what makes it able to
// say "that IO really is still out there" rather than "our bookkeeping says so".
static uint64_t node_inflight(const std::string& node) {
    std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;
    std::string path = "/sys/block/" + kname + "/inflight";
    char buf[64] = {};
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    unsigned long rd = 0, wr = 0;
    if (n <= 0 || sscanf(buf, "%lu %lu", &rd, &wr) != 2)
        return 0;
    return rd + wr;
}

// The kernel's nr_cpu_ids, which is what ublk_drv clamps ADD_DEV's nr_hw_queues to:
// setup_nr_cpu_ids() sets it to find_last_bit(cpu_possible_mask) + 1, and this file
// prints exactly that mask. So the POSSIBLE count is the oracle and the online one is
// not -- sysconf(_SC_NPROCESSORS_ONLN) and std::thread::hardware_concurrency() both
// report online, which a host carrying present-but-offline cpus makes smaller, so a
// case keyed on either would run on a host where the clamp does not bite and read its
// own request back as the kernel's answer. 0 if the file cannot be read.
static uint32_t possible_cpus() {
    int fd = ::open("/sys/devices/system/cpu/possible", O_RDONLY);
    if (fd < 0)
        return 0;
    char buf[256] = {};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return 0;
    // a cpu list ("0-7", "0-3,8-11"): the highest index is the last number in it,
    // whatever the list's shape
    uint32_t last = 0, v = 0;
    bool in_num = false;
    for (const char* p = buf; ; p++) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p - '0');
            in_num = true;
            continue;
        }
        if (in_num) { last = v; v = 0; in_num = false; }
        if (*p == '\0' || *p == '\n')
            break;
    }
    return last + 1;
}

// This thread's own CPU time, in milliseconds. CLOCK_THREAD_CPUTIME_ID rather than
// getrusage(RUSAGE_SELF): the oracle has to see exactly the vcpu that serves the queue
// and nothing else -- not the consumer child device_io spawns, not the off-vcpu helper
// thread, not photon's ancillary threads. That is what lets a case read "the pump was
// busy-polling" off it, provided the case first establishes the pump is on this thread.
static uint64_t thread_cpu_ms() {
    struct timespec ts = {};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) < 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / (1000 * 1000);
}

// Sleep `ms` on the vcpu and report the CPU this thread burned doing it. A coroutine
// busy-polling on this vcpu spends nearly all of the wall time; one blocked in the
// kernel's event wait spends none of it.
static uint64_t thread_cpu_ms_during(uint64_t ms) {
    uint64_t t0 = thread_cpu_ms();
    photon::thread_usleep(ms * 1000);
    return thread_cpu_ms() - t0;
}

// Forwards to the wrapped file and counts the three things an FUA dispatch can be told
// apart by. It is NOT the harness's test::CountingFile, which counts destructions for
// the ownership contract; this one counts syncs, and the two are named differently
// because both appear in this file.
//
// `writes` and `datasyncs` are what ublk's FUA path does now: pwritev, then fdatasync.
// `dsync_writes` is the prohibition half and must stay zero, because IFile::pwritev2 is
// not pure virtual and its base default drops `flags` -- a durability request sent that
// way reached only the backends that overrode it, and every other one discarded it
// while still reporting success.
class SyncCountingFile : public fs::IFile {
public:
    fs::IFile* const f;
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> datasyncs{0};
    std::atomic<uint64_t> dsync_writes{0};
    explicit SyncCountingFile(fs::IFile* file) : f(file) {}

    int close() override { return 0; }   // ownership stays with the fixture
    ssize_t read(void*, size_t) override { errno = ENOSYS; return -1; }
    ssize_t readv(const iovec*, int) override { errno = ENOSYS; return -1; }
    ssize_t write(const void*, size_t) override { errno = ENOSYS; return -1; }
    ssize_t writev(const iovec*, int) override { errno = ENOSYS; return -1; }
    fs::IFileSystem* filesystem() override { return nullptr; }
    ssize_t pread(void* buf, size_t count, off_t off) override { return f->pread(buf, count, off); }
    ssize_t preadv(const iovec* iov, int n, off_t off) override { return f->preadv(iov, n, off); }
    ssize_t pwrite(const void* buf, size_t count, off_t off) override { return f->pwrite(buf, count, off); }
    ssize_t pwritev(const iovec* iov, int n, off_t off) override {
        writes++;
        return f->pwritev(iov, n, off);
    }
    ssize_t pwritev2(const iovec* iov, int n, off_t off, int flags) override {
        if (flags & RWF_DSYNC)
            dsync_writes++;
        return f->pwritev2(iov, n, off, flags);
    }
    off_t lseek(off_t off, int whence) override { return f->lseek(off, whence); }
    int fsync() override { return f->fsync(); }
    int fdatasync() override {
        datasyncs++;
        return f->fdatasync();
    }
    int fchmod(mode_t m) override { return f->fchmod(m); }
    int fchown(uid_t u, gid_t g) override { return f->fchown(u, g); }
    int fstat(struct stat* st) override { return f->fstat(st); }
    int ftruncate(off_t len) override { return f->ftruncate(len); }
};

// A backend that hands back one byte other than the one it was given, at one
// offset. What it exists for: a consumer child's read-back comparison is the only
// thing standing between a device that returns wrong data and a suite that reports
// success, and every node these suites export returns right data -- so without a
// backend that does not, the comparison's own sensitivity is never exercised and
// CONS_VERIFY is an enumerator nothing asserts on.
//
// File-local, and one offset rather than all of them: the kernel reads a new device
// while it is scanning it, and corrupting those reads would change what the device
// looks like, which is not what is under test. UINT64_MAX is "corrupt nothing", so a
// case can bring the node up honest and turn the fault on once it is up.
class CorruptingFile : public test::RecordingFile {
public:
    explicit CorruptingFile(fs::IFile* f) : test::RecordingFile(f) {}
    uint64_t corrupt_at = UINT64_MAX;
    ssize_t preadv(const struct iovec* iov, int iovcnt, off_t offset) override {
        ssize_t r = test::RecordingFile::preadv(iov, iovcnt, offset);
        // Only the fragment that starts AT the offset, so a request the kernel
        // split on the way down is corrupted once and not once per fragment.
        if (r > 0 && iovcnt > 0 && (uint64_t)offset == corrupt_at)
            ((char*)iov[0].iov_base)[0] ^= 0xff;
        return r;
    }
};

// The construction-time half of this suite: what a caller can learn from a config
// and from a device that has been built but not started.
//
// Ungated, and that is the whole point of splitting it out. new_device() runs
// validate() -- pure by its own contract, no I/O and no kernel access -- and a
// constructor that only derives fields from the config, and start() rejects a null
// backend before it opens the control device. So none of this needs root or
// ublk_drv. Behind UblkTest's gate CI executed none of it: the runners there are
// unprivileged containers on a 5.10 node kernel, which predates ublk entirely, so
// the gate could never open no matter how the suite was written.
//
// The controller still gets SUITE_LOCKS rather than a writable stand-in. Nothing
// here opens it, and if a case ever starts to, a non-root run fails loudly instead
// of passing against a directory only some machines happen to permit.
class UblkConfigTest : public ::testing::Test {
public:
    UblkController* ctl = nullptr;

    void SetUp() override {
        ctl = new_ublk_controller(SUITE_LOCKS);
        ASSERT_NE(nullptr, ctl);
    }

    void TearDown() override {
        delete ctl;
        ctl = nullptr;
    }
};

class UblkTest : public test::SkippableTest {
public:
    test::TestImage img;
    fs::IFile* file = nullptr;
    // The default lock dir, and the only route this fixture has to a device: every
    // new_device() and every list_orphans() below goes through it, which is the
    // point of the controller -- a recovery loop cannot scan one dir and claim in
    // another.
    UblkController* ctl = nullptr;

    void SetUp() override {
        if (geteuid() != 0)
            return report_skip("ublk test requires root");
        if (::access("/dev/ublk-control", F_OK) != 0) {
            ::system("modprobe ublk_drv 2>/dev/null");
            photon::thread_usleep(100 * 1000);
            if (::access("/dev/ublk-control", F_OK) != 0)
                return report_skip("ublk_drv module not available");
        }
        ctl = new_ublk_controller(SUITE_LOCKS);
        ASSERT_NE(nullptr, ctl);
        sweep_orphans();
        ASSERT_EQ(0, img.create(IMG_PATH, IMG_SIZE));
        file = img.file;
    }

    void TearDown() override {
        img.release();
        sweep_orphans();
        delete ctl;
        ctl = nullptr;
    }

    // remove leftovers from crashed runs. Two forms: QUIESCED orphans
    // (list_orphans), and DEAD residues (crashed mid-create) which the
    // orphan list deliberately excludes. For every /dev/ublkcN whose flock
    // exists and is free, start() with a dummy backend (a DEAD residue is
    // DEL'd and recreated, a QUIESCED one recovered), then shutdown(). A
    // missing lock file usually means a device that is not ours -- this
    // walks a kernel-enumerated namespace -- but it is not proof of that:
    // our own residue from the auto-assigned-id create has no lock file
    // between ADD_DEV and the claim, and neither loop here reaches it. A
    // held lock means a live server.
    //
    // Owns its controller rather than using ctl: a skipped SetUp returns
    // before ctl exists, and TearDown still runs.
    void sweep_orphans() {
        auto c = new_ublk_controller(SUITE_LOCKS);   // the same scope as ctl
        if (!c)
            return;
        DEFER(delete c);
        std::vector<uint32_t> ids;
        // QUIESCED orphans: recover with their recorded config (start()
        // validates size against the registration)
        for (auto& rec : c->list_orphans()) {
            auto id = (uint32_t)atoi(rec.identity.c_str());
            ids.push_back(id);
            auto f = lfs_open_dummy();
            UblkController::Config cfg(rec);   // dev_id falls back to the decimal identity
            auto d = c->new_device(cfg);
            if (!d) { delete f; continue; }
            if (d->start(f, true) == 0)
                d->shutdown();
            else
                delete f;
            delete d;
        }
        if (DIR* d = ::opendir("/dev")) {
            struct dirent* e;
            while ((e = readdir(d))) {
                if (strncmp(e->d_name, "ublkc", 5) != 0 || !isdigit(e->d_name[5]))
                    continue;
                uint32_t id = (uint32_t)atoi(e->d_name + 5);
                std::string lp = std::string(SUITE_LOCKS) + "/ublk-" + std::to_string(id) + ".lock";
                int fd = ::open(lp.c_str(), O_RDONLY);
                if (fd < 0)
                    continue;
                bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
                if (free) ::flock(fd, LOCK_UN);
                ::close(fd);
                if (free && std::find(ids.begin(), ids.end(), id) == ids.end())
                    ids.push_back(id);
            }
            ::closedir(d);
        }
        for (uint32_t id : ids) {
            auto f = lfs_open_dummy();
            UblkController::Config cfg;
            cfg.info.identity = std::to_string(id);
            cfg.dev_id = id;
            cfg.info.size = 4 << 10;
            cfg.info.features = FEATURE_FLUSH;
            auto d = c->new_device(cfg);
            if (!d) { delete f; continue; }
            if (d->start(f, true) == 0)
                d->shutdown();
            else
                delete f;
            delete d;
        }
    }
    fs::IFile* lfs_open_dummy() {
        auto l = fs::new_localfs_adaptor();
        if (!l) return nullptr;
        auto f = l->open(IMG_PATH, O_RDWR | O_CREAT, 0644);
        delete l;
        return f;
    }

    // run blocking device IO off the photon vcpu, in a spawned consumer child;
    // harness.h's device_io is the authoritative statement of what it returns
    int device_io(const std::string& node, const std::vector<char>& wbuf,
                  bool verify_backend, uint64_t off = IO_OFF) {
        test::DeviceIoOpts o;
        o.backend = verify_backend ? file : nullptr;
        return test::device_io(node, wbuf.data(), wbuf.size(), off, o);
    }

    std::vector<char> pattern(uint8_t seed, size_t n = IO_LEN) { return test::pattern(seed, n); }
};

// The capability half of BlkDevInfo: what lets a caller branch on behaviour instead of
// inferring it from which factory built the object. The axes are properties of the
// transport, so they are already correct on a constructed device.
TEST_F(UblkConfigTest, capabilities_descriptor) {
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    const BlkDevInfo& i = dev->get_info();

    // The registration survives detach() in QUIESCED and a later start() re-attaches
    // through START_USER_RECOVERY, so the backlog is kernel-side. start() compares the
    // registered size, sector shift and attrs against this config and refuses a mismatch
    // with EINVAL, which is what makes adoption Full rather than partial.
    EXPECT_EQ(BlkBacklog::KernelSide, i.backlog);
    EXPECT_EQ(BlkAdoption::Full, i.adoption);
    // shutdown() gates on TRY_STOP_DEV, which answers EBUSY for as long as an initiator
    // holds the node and then gives up with EBUSY after stop_timeout_ms, leaving the
    // device alone -- shutdown_busy is the case that pins it. That DEL_DEV itself has no
    // liveness check is destroy_orphan()'s contract, a different operation, and citing
    // it here is what had this axis answering one level below the question blk.h asks.
    EXPECT_EQ(BlkShutdownRefusal::RefusesWhenAttached, i.shutdown_refusal);
    // UPDATE_SIZE goes through the control channel and a refusal there fails resize()
    EXPECT_EQ(BlkResizeEffect::NotifiedOrFailed, i.resize_effect);
    // The one transport where detach(false) really does skip the I/O wait: stop_serving
    // drains in_flight only when asked to flush, and queue teardown abandons rather than
    // waits. What is left is the QUIESCED poll, bounded by quiesce_timeout_ms because it
    // waits on the kernel and not on a backend. Every other transport answers false.
    EXPECT_EQ(true, i.detach_no_wait);
    EXPECT_EQ(FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES, i.offered);
    EXPECT_EQ(0ull, i.negotiated);   // no registration yet
}

// `negotiated` is the one descriptor member start() has to fill in, so it is the half
// of the descriptor that does need the kernel -- and the half the ungated case above
// can only pin at its initial value.
TEST_F(UblkTest, capabilities_negotiated_is_filled_by_start) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    // Spelled out rather than derived from `features & offered`, which is how the
    // implementation computes it and would therefore agree with itself if it were wrong.
    EXPECT_EQ(FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES,
              dev->get_info().negotiated);
}

TEST_F(UblkConfigTest, config_validation) {
    // the pure geometry checks are construction-time now: no object at all
    UblkController::Config bad(make_info());
    bad.info.size = 0;
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    bad = UblkController::Config(make_info());
    bad.info.sector_size_shift = 8;
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    bad = UblkController::Config(make_info());
    bad.info.size = IMG_SIZE + 1;   // not sector-aligned
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    // A null backend is start()'s to reject: it is not part of the config. The
    // rejection is the second statement of start(), ahead of the control device
    // being opened, so it needs no kernel -- and it leaves nothing behind, since
    // `backend` is only assigned once the argument is known good.
    UblkController::Config good(make_info());
    auto dev = ctl->new_device(good);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(nullptr));
    EXPECT_EQ(EINVAL, errno);
}

// The serving loop speaks only the plain ublk data plane: FETCH hands the kernel a
// buffer address and COMMIT carries the result. Two flags negotiate a different
// protocol it does not implement -- NEED_GET_DATA makes a WRITE's FETCH complete with
// a positive result tag_loop reads as fatal (it exits, stranding the request and
// burning the tag), and USER_COPY moves data by pread/pwrite on the cdev with a zero
// FETCH address the loop never sends. Both must be refused at construction, before
// ADD_DEV, so no object exists to strand anything. The values are from
// <linux/ublk_cmd.h>; this TU cannot see ublk.cpp's private copy of the UAPI.
TEST_F(UblkConfigTest, unimplemented_data_plane_flags_are_refused) {
    constexpr uint64_t NEED_GET_DATA = 1ULL << 2;
    constexpr uint64_t USER_COPY     = 1ULL << 7;
    for (uint64_t f : {NEED_GET_DATA, USER_COPY, NEED_GET_DATA | USER_COPY}) {
        UblkController::Config bad(make_info());
        bad.flags = f;
        errno = 0;
        EXPECT_EQ(nullptr, ctl->new_device(bad)) << "flags=" << f;
        EXPECT_EQ(EINVAL, errno) << "flags=" << f;
    }
    // control: a config that asks for no exotic data plane still constructs, so the
    // refusals above key on the flags and are not a blanket reject
    UblkController::Config good(make_info());
    good.flags = 0;
    auto dev = ctl->new_device(good);
    EXPECT_NE(nullptr, dev);
    delete dev;
}

TEST_F(UblkTest, a_second_start_on_the_same_object_is_ealready) {
    if (skip_reason) return;
    UblkController::Config good(make_info());
    auto dev = ctl->new_device(good);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    ASSERT_EQ(0, dev->start(file));
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EALREADY, errno);
    ASSERT_EQ(0, dev->shutdown());
}

// blk.h's start() contract, the half test::CountingFile exists to witness: an OWNED
// backend is deleted on shutdown, not only by the destructor. Both shutdowns must
// succeed for the release to be owed -- ublk's propagates a DEL_DEV failure, and a
// registration that survives is one this server can re-serve, so it keeps its backend.
TEST_F(UblkTest, shutdown_releases_a_backend_it_owns) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    std::atomic<int> destroyed{0};
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), /*ownership=*/true));
    EXPECT_EQ(0, destroyed.load());
    ASSERT_EQ(0, dev->shutdown());
    EXPECT_EQ(1, destroyed.load());

    // Usable again, which is the transition the leak hid behind: a second start()
    // registers a second device and serves a second backend, instead of overwriting
    // a pointer to a live one.
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), true));
    EXPECT_EQ(1, destroyed.load());
    ASSERT_EQ(0, dev->shutdown());
    EXPECT_EQ(2, destroyed.load());

    // An UNOWNED backend stays the caller's to delete.
    fs::IFile* mine = new test::CountingFile(file, &destroyed);
    ASSERT_EQ(0, dev->start(mine));
    ASSERT_EQ(0, dev->shutdown());
    EXPECT_EQ(2, destroyed.load());
    delete mine;
    EXPECT_EQ(3, destroyed.load());

    // detach() is the other way back into start(), and it is the leg that still
    // leaked after shutdown()->start() was fixed. detach keeps the registration on
    // purpose, and it cannot release the backend either: it doubles as the rollback
    // path of a failed start, where the caller owns it. So start() is the only place
    // that can release what an earlier session owned -- and both halves below are
    // load-bearing, because releasing in detach() instead would pass the second and
    // break the first.
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), true));
    EXPECT_EQ(3, destroyed.load());
    ASSERT_EQ(0, dev->detach(false));
    EXPECT_EQ(3, destroyed.load()) << "detach released a backend it is meant to keep";
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), true));
    EXPECT_EQ(4, destroyed.load()) << "start() overwrote the owned backend detach had kept";
    ASSERT_EQ(0, dev->shutdown());
    EXPECT_EQ(5, destroyed.load());
}

TEST_F(UblkTest, basic_io) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    cfg.queues = 2;
    cfg.queue_depth = 64;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());
    EXPECT_EQ(0, ::access(node.c_str(), F_OK));
    EXPECT_EQ(0, device_io(node, pattern(0x5a), /*verify_backend=*/true));
    // and a second range, crossing queues' tags
    EXPECT_EQ(0, device_io(node, pattern(0xa5), true, IMG_SIZE - IO_OFF - IO_LEN));
}

// Every other case here pins sector_size_shift = 9, where the ublk 512-byte wire ABI
// and the logical block size are the same number -- which is exactly why the shift bug
// this guards survived review. At 12 (4 KiB logical blocks) they differ 8x, and the
// ABI's nr_sectors / start_sector / dev_sectors are 512-byte counts that must NOT be
// scaled by the logical shift. Capacity and IO placement are the two observable halves,
// and mirror the reviewer's probe: a requested 64 MiB came back 8 MiB, and a 4 KiB
// write at 4 KiB landed as 32 KiB at 32 KiB.
TEST_F(UblkTest, sector_shift_12_places_io_and_reports_true_capacity) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    cfg.info.sector_size_shift = 12;   // 4 KiB logical block
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);

    // The kernel sets capacity from dev_sectors, a 512-byte count. Deriving it with
    // the logical shift instead reported 1/8 of IMG_SIZE.
    uint64_t cap = 0;
    test::run_off_vcpu([&] {
        int fd = ::open(node.c_str(), O_RDONLY);
        if (fd >= 0) { if (::ioctl(fd, BLKGETSIZE64, &cap) < 0) cap = 0; ::close(fd); }
    });
    EXPECT_EQ(IMG_SIZE, cap);

    // One logical block written at one logical block's offset must land at that byte
    // offset in the backend with that length. Scaling the ABI sectors by the logical
    // shift sent 4 KiB at 4 KiB to 32 KiB at 32 KiB instead, so the backend's copy at
    // 4 KiB stayed empty and the verify reads back a mismatch.
    EXPECT_EQ(0, device_io(node, pattern(0x5a, 4096), /*verify_backend=*/true, 4096));
}

// High-concurrency stress across MULTIPLE hardware queues: the kernel spreads
// the O_DIRECT threads over the queues, so this drives several per-queue
// cascading engines and their tag coroutines at once (basic_io only crosses
// tags inside one queue). Mixed block sizes exercise the iov/segment paths;
// the self-describing blocks attribute any misrouted or torn IO.
TEST_F(UblkTest, concurrent_stress) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
    cfg.queues = (uint32_t)std::min<long>(4, cpus > 0 ? cpus : 1);   // ublk: <= nr_cpus
    cfg.queue_depth = 128;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());

    EXPECT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "ublk"));
}

TEST_F(UblkTest, read_only) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    cfg.read_only = true;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);

    // the kernel-level RO flag must be set (UBLK_ATTR_READ_ONLY)
    {
        std::string ro_path = "/sys/block/" + node.substr(5) + "/ro";
        char b[8] = {};
        int fd = ::open(ro_path.c_str(), O_RDONLY);
        ASSERT_GE(fd, 0);
        ASSERT_GT(::read(fd, b, sizeof(b) - 1), 0);
        ::close(fd);
        EXPECT_EQ('1', b[0]);
    }

    EXPECT_EQ(0, test::expect_write_rejected(node, 0, 4096));
}

TEST_F(UblkTest, discard_write_zeroes) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);

    int result = -1;
    test::run_off_vcpu([&] {
        auto wbuf = pattern(0x3c);
        int fd = ::open(node.c_str(), O_RDWR);
        if (fd < 0) { result = errno ? errno : EIO; return; }
        DEFER(::close(fd));
        if (::pwrite(fd, wbuf.data(), wbuf.size(), IO_OFF) != (ssize_t)wbuf.size()) {
            result = errno ? errno : EIO; return;
        }
        ::fsync(fd);
        uint64_t range[2] = {IO_OFF, wbuf.size()};
        if (::ioctl(fd, BLKDISCARD, range) < 0) { result = errno ? errno : EIO; return; }
        std::vector<char> rbuf(wbuf.size(), 0x7e);
        if (::pread(fd, rbuf.data(), rbuf.size(), IO_OFF) != (ssize_t)rbuf.size()) {
            result = errno ? errno : EIO; return;
        }
        static const std::vector<char> zeros(wbuf.size(), 0);
        if (memcmp(rbuf.data(), zeros.data(), rbuf.size())) { result = EILSEQ; return; }
        // write again, then BLKZEROOUT
        if (::pwrite(fd, wbuf.data(), wbuf.size(), IO_OFF) != (ssize_t)wbuf.size()) {
            result = errno ? errno : EIO; return;
        }
        ::fsync(fd);
        if (::ioctl(fd, BLKZEROOUT, range) < 0) { result = errno ? errno : EIO; return; }
        if (::pread(fd, rbuf.data(), rbuf.size(), IO_OFF) != (ssize_t)rbuf.size()) {
            result = errno ? errno : EIO; return;
        }
        result = memcmp(rbuf.data(), zeros.data(), rbuf.size()) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, result);
    // the backend range must read back zero too (trim punched a hole /
    // zero_range wrote zeros)
    std::vector<char> bbuf(IO_LEN, 0x7e);
    struct iovec iov{bbuf.data(), bbuf.size()};
    ASSERT_EQ((ssize_t)IO_LEN, file->preadv(&iov, 1, IO_OFF));
    EXPECT_TRUE(iovector_view(&iov, 1).is_zero());
}

TEST_F(UblkTest, detach_reattach) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    std::string node = node_of(dev);
    uint32_t id = node_dev_id(node.c_str());
    ASSERT_NE(UINT32_MAX, id);
    ASSERT_EQ(0, device_io(node, pattern(0x11), true));

    // detach keeps the registration: the block node survives, quiesced
    ASSERT_EQ(0, dev->detach(true));
    EXPECT_EQ(0, ::access(node.c_str(), F_OK));

    // re-attach by dev_id (the recovery identity). It travels in the CONFIG, not
    // in the object -- an object's identity is fixed at construction -- so this
    // is a fresh device built from the id we discovered.
    UblkController::Config cfg2(make_info());
    cfg2.dev_id = id;
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    ASSERT_EQ(0, dev2->start(file));
    DEFER(dev2->shutdown());
    EXPECT_EQ(node, node_of(dev2));
    std::vector<char> rbuf(IO_LEN);
    int rc = -1;
    test::run_off_vcpu([&] {
        int fd = ::open(node.c_str(), O_RDONLY);
        if (fd < 0) { rc = errno ? errno : EIO; return; }
        DEFER(::close(fd));
        rc = ::pread(fd, rbuf.data(), rbuf.size(), IO_OFF) == (ssize_t)rbuf.size() ? 0 : EIO;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0, memcmp(pattern(0x11).data(), rbuf.data(), IO_LEN));
}

// BlkAdoption::Full is documented as checking the geometry AND the registered feature
// set. FEATURE_DISCARD and FEATURE_WRITE_ZEROES are not in basic.attrs -- they are an
// optional param type plus the two caps inside it -- so a drift in either direction used
// to pass the comparison while start() went on to publish cfg.info.negotiated from the
// REQUEST and the kernel queue kept the registration's limits. SET_PARAMS is refused once
// a device has been used, so an adoption cannot close that gap; blk.h's `negotiated`
// contract for ublk is that a refusal there fails start(), which is what this pins.
//
// Gated, and CI cannot run it: it needs root and ublk_drv.
TEST_F(UblkTest, adoption_refuses_a_feature_set_the_registration_does_not_have) {
    if (skip_reason) return;
    constexpr uint64_t F   = FEATURE_FLUSH;
    constexpr uint64_t D   = FEATURE_FLUSH | FEATURE_DISCARD;
    constexpr uint64_t DW  = FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
    constexpr uint64_t W   = FEATURE_FLUSH | FEATURE_WRITE_ZEROES;
    struct Row { uint64_t created, adopted; const char* what; };
    const Row rows[] = {
        {F,  DW, "ask for more than the registration has"},
        {DW, F,  "ask for less than the registration has"},
        {D,  W,  "swap one capability for another"},
    };
    for (const auto& row : rows) {
        UblkController::Config cfg(make_info());
        cfg.info.features = row.created;
        auto dev = ctl->new_device(cfg);
        ASSERT_NE(nullptr, dev) << row.what;
        DEFER(delete dev);
        ASSERT_EQ(0, dev->start(file)) << row.what;
        std::string node = node_of(dev);
        uint32_t id = node_dev_id(node.c_str());
        ASSERT_NE(UINT32_MAX, id) << row.what;
        ASSERT_EQ(0, dev->detach(true)) << row.what;   // quiesced + lock free = orphan

        // Non-vacuity, and on both sides of the comparison. The row's own premise first:
        // a row whose two sets were equal would make the refusal below prove nothing.
        ASSERT_NE(row.created, row.adopted) << row.what;
        // Then the registration's side, read back out of the kernel through the control
        // channel rather than off our own descriptor: the orphan record's features come
        // from the same GET_PARAMS reply the adoption validates against, so this shows
        // what the refusal is actually refusing.
        bool listed = false;
        for (auto& r : ctl->list_orphans()) {
            if (r.identity != std::to_string(id))
                continue;
            listed = true;
            EXPECT_EQ(row.created, r.features) << row.what;
        }
        ASSERT_TRUE(listed) << row.what;

        UblkController::Config cfg2(make_info());
        cfg2.info.features = row.adopted;
        cfg2.dev_id = id;
        auto dev2 = ctl->new_device(cfg2);
        ASSERT_NE(nullptr, dev2) << row.what;
        DEFER(delete dev2);
        errno = 0;
        EXPECT_EQ(-1, dev2->start(file)) << row.what;
        EXPECT_EQ(EINVAL, errno) << row.what;
        // the refused start() published nothing: a negotiated set is a claim about a
        // session, and there is no session
        EXPECT_EQ(0ull, dev2->get_info().negotiated) << row.what;

        // The refusal left the registration alone, so a config matching it still adopts.
        // This is also the leg that removes the device the row created, and the control
        // that the refusal above keys on the feature set and is not a blanket reject of
        // every adoption.
        UblkController::Config cfg3(make_info());
        cfg3.info.features = row.created;
        cfg3.dev_id = id;
        auto dev3 = ctl->new_device(cfg3);
        ASSERT_NE(nullptr, dev3) << row.what;
        DEFER(delete dev3);
        DEFER(dev3->shutdown());
        ASSERT_EQ(0, dev3->start(file)) << row.what;
        EXPECT_EQ(node, node_of(dev3)) << row.what;
    }
}

TEST_F(UblkTest, orphan_list) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    std::string node = node_of(dev);
    uint32_t id = node_dev_id(node.c_str());
    ASSERT_NE(UINT32_MAX, id);
    ASSERT_EQ(0, device_io(node, pattern(0x22), true));
    ASSERT_EQ(0, dev->detach(true));   // quiesced + lock free = orphan

    bool found = false;
    BlkDevInfo rec;
    for (auto& i : ctl->list_orphans()) {
        if (i.identity == std::to_string(id)) {
            found = true;
            rec = i;
        }
    }
    ASSERT_TRUE(found);
    EXPECT_EQ(IMG_SIZE, rec.size);
    EXPECT_EQ(9, (int)rec.sector_size_shift);
    EXPECT_EQ(FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES, rec.features);

    // recover via the orphan record alone (identity parses back to dev_id)
    UblkController::Config cfg2(rec);
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    DEFER(dev2->shutdown());
    ASSERT_EQ(0, dev2->start(file));
    EXPECT_EQ(node, node_of(dev2));
}

// Start a device and leave it quiesced with its flock free, which is what a
// daemon that died mid-life leaves behind, and hand back the dev_id. The device
// object stays the caller's: its destructor re-claims the flock, so it will
// RE-CREATE the tombstone file on the way out. Every assertion about a tombstone
// being gone therefore has to be made before that runs, and a stale lock file is
// what residue() already expects rather than residue -- devlock_release() never
// unlinks.
static std::string detach_into_orphan(UblkController* ctl, fs::IFile* file,
                                      const BlkDevInfo& info, IBlkDevice** dev_out) {
    *dev_out = nullptr;
    UblkController::Config cfg(info);
    auto dev = ctl->new_device(cfg);
    if (!dev)
        return "";
    if (dev->start(file) < 0) {
        delete dev;
        return "";
    }
    const char* n = dev->get_device_node();
    std::string node = n ? n : "";
    uint32_t id = node.empty() ? UINT32_MAX : node_dev_id(node.c_str());
    if (id == UINT32_MAX || dev->detach(true) < 0) {
        dev->shutdown();
        delete dev;
        return "";
    }
    *dev_out = dev;
    return std::to_string(id);
}

TEST_F(UblkTest, destroy_orphan_removes_a_dead_registration) {
    if (skip_reason) return;
    IBlkDevice* dev = nullptr;
    std::string id = detach_into_orphan(ctl, file, make_info(), &dev);
    ASSERT_FALSE(id.empty()) << "could not fabricate an orphan";
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    std::string cn = "/dev/ublkc" + id;
    std::string bn = "/dev/ublkb" + id;
    std::string lp = std::string(SUITE_LOCKS) + "/ublk-" + id + ".lock";

    // BEFORE, read independently of our own state
    ASSERT_EQ(0, ::access(cn.c_str(), F_OK));
    ASSERT_EQ(0, ::access(lp.c_str(), F_OK));
    BlkDevInfo rec;
    bool found = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == id) { rec = i; found = true; }
    ASSERT_TRUE(found) << "the quiesced device was not reported as an orphan";
    EXPECT_EQ(IMG_SIZE, rec.size);

    // Descriptor count taken here, once the scan above has finished with its own
    // control channel, and read again the moment destroy_orphan() returns -- so the
    // pair brackets exactly the descriptors that call takes: the claim it opens, and
    // the control channel it inits and finis. Neither may outlive it.
    int fds_before = fd_count();
    ASSERT_LT(0, fds_before);

    errno = 0;
    int rc = ctl->destroy_orphan(rec);
    int e = errno;
    int destroyed = (rc == 0) ? 1 : 0;
    EXPECT_EQ(0, rc) << "errno " << e;
    EXPECT_EQ(1, destroyed) << "nothing was destroyed";
    EXPECT_EQ(fds_before, fd_count())
        << "destroy_orphan kept a descriptor: the claim it took, or the control channel it opened";

    // AFTER. Three separate observations, because the scan's gate is the kernel
    // registration: once /dev/ublkcN is gone readdir never yields it again,
    // whatever the tombstone does. So "the list no longer reports it" cannot
    // witness a leaked tombstone, and the tombstone is asserted on its own --
    // before the device's destructor re-creates it (see detach_into_orphan).
    EXPECT_NE(0, ::access(cn.c_str(), F_OK)) << "the kernel registration survived";
    EXPECT_NE(0, ::access(bn.c_str(), F_OK)) << "the block node survived";
    EXPECT_NE(0, ::access(lp.c_str(), F_OK)) << "the tombstone survived";
    for (auto& i : ctl->list_orphans())
        EXPECT_NE(id, i.identity);
}

TEST_F(UblkTest, destroy_orphan_refuses_a_live_device) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);
    uint32_t id = node_dev_id(node.c_str());
    ASSERT_NE(UINT32_MAX, id);
    std::string sid = std::to_string(id);
    std::string cn = "/dev/ublkc" + sid;

    // live, so a server holds the flock and the scan does not report it
    for (auto& i : ctl->list_orphans())
        EXPECT_NE(sid, i.identity);

    // A BlkDevInfo from an EARLIER scan: the record can predate this server
    // taking the dev_id over, which is the window the claim closes. DEL_DEV is
    // unconditional -- no EBUSY, no state check -- so unlike tcmu there is no
    // second gate here to mask a missing flock check: without the claim this call
    // would really destroy a live device.
    BlkDevInfo stale;
    stale.identity = sid;
    stale.size = IMG_SIZE;
    stale.sector_size_shift = 9;
    errno = 0;
    int rc = ctl->destroy_orphan(stale);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EBUSY, e) << "a live device must be refused, not destroyed";

    // nothing was torn down, and the proof is that it still works: one real I/O
    // through the node, verified against the backend image
    EXPECT_EQ(0, ::access(cn.c_str(), F_OK)) << "a live device's registration was destroyed";
    EXPECT_EQ(0, device_io(node, pattern(0x44), true));
    for (auto& i : ctl->list_orphans())
        EXPECT_NE(sid, i.identity);
}

TEST_F(UblkTest, destroy_orphan_validates_the_identity) {
    if (skip_reason) return;
    IBlkDevice* dev = nullptr;
    std::string id = detach_into_orphan(ctl, file, make_info(), &dev);
    ASSERT_FALSE(id.empty()) << "could not fabricate an orphan";
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    std::string cn = "/dev/ublkc" + id;
    ASSERT_EQ(0, ::access(cn.c_str(), F_OK));

    // The victim: what each spelling below would name if it were parsed
    // permissively. Measured rather than assumed -- the numeric parse this call
    // uses is strtoul-based, and strtoul skips leading white space and accepts an
    // optional sign, so " 7" and "+7" both parse to 7. atoi(), which list_orphans()
    // can afford because it reads a kernel-enumerated dirent it has already
    // checked with isdigit(), turns "garbage" into 0 -- a valid dev_id.
    //
    // Owned strings, not c_str() of temporaries: two of these are built from the
    // dev_id this case fabricated, so they have to outlive the initializer.
    const std::vector<std::pair<std::string, const char*>> bad = {
        {"+" + id, "a leading sign parses to the device it spells"},
        {"  " + id, "leading white space parses to the device it spells"},
        {"garbage", "atoi() would make this dev 0, somebody else's device"},
        {"4294967295", "Config::dev_id's auto-assign sentinel, not a device"},
        {"4294967296", "one past what fits in the 32-bit dev_id"},
        {"", "nothing to name"},
    };
    int refused = 0;
    for (auto& b : bad) {
        BlkDevInfo d;
        d.identity = b.first;
        d.size = IMG_SIZE;
        d.sector_size_shift = 9;
        errno = 0;
        int rc = ctl->destroy_orphan(d);
        int e = errno;
        EXPECT_EQ(-1, rc) << "identity [" << b.first << "]: " << b.second;
        EXPECT_EQ(EINVAL, e) << "identity [" << b.first << "]: " << b.second;
        if (rc == -1 && e == EINVAL)
            refused++;
    }
    EXPECT_EQ((int)bad.size(), refused) << "not every unusable identity was refused";
    // and the device they all would have named is untouched
    EXPECT_EQ(0, ::access(cn.c_str(), F_OK)) << "a rejected identity destroyed the device it parsed to";
    bool still = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == id) still = true;
    EXPECT_TRUE(still) << "the orphan stopped being reported after a refused destroy";

    // The correct spelling of the same dev_id still works, so the rejections above
    // are about the spellings and not about the device having become undestroyable.
    BlkDevInfo good;
    good.identity = id;
    good.size = IMG_SIZE;
    good.sector_size_shift = 9;
    errno = 0;
    int rc = ctl->destroy_orphan(good);
    int e = errno;
    EXPECT_EQ(0, rc) << "errno " << e;
    EXPECT_NE(0, ::access(cn.c_str(), F_OK));
}

TEST_F(UblkTest, destroy_orphan_refuses_a_directory_tombstone) {
    if (skip_reason) return;
    IBlkDevice* dev = nullptr;
    std::string id = detach_into_orphan(ctl, file, make_info(), &dev);
    ASSERT_FALSE(id.empty()) << "could not fabricate an orphan";
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    std::string cn = "/dev/ublkc" + id;
    std::string lp = std::string(SUITE_LOCKS) + "/ublk-" + id + ".lock";

    // A DIRECTORY where the tombstone should be. devlock_free() opens O_RDONLY and
    // flocks, and both succeed on a directory fd, so the scan still reads this
    // entry as free and reports it; it is devlock_acquire()'s O_CREAT|O_RDWR that
    // refuses one. That asymmetry is what makes the entry listed but unadoptable,
    // and both halves are measured below rather than assumed.
    ASSERT_EQ(0, ::unlink(lp.c_str()));
    ASSERT_EQ(0, ::mkdir(lp.c_str(), 0755));
    DEFER(::rmdir(lp.c_str()));

    BlkDevInfo rec;
    bool found = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == id) { rec = i; found = true; }
    ASSERT_TRUE(found) << "a directory tombstone should still read as free to the scan";

    // adoption cannot proceed, at the very open devlock_acquire() performs
    errno = 0;
    int fd = ::open(lp.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    int oe = errno;
    EXPECT_EQ(-1, fd) << "the tombstone path was claimable after all";
    EXPECT_EQ(EISDIR, oe);
    if (fd >= 0) ::close(fd);

    // Here destroy_orphan() refuses too, and removes NOTHING -- which is not the
    // same as tcmu, and the difference is forced rather than chosen: on ublk the
    // claim IS the liveness gate (DEL_DEV is unconditional), so a tombstone that
    // cannot be opened means liveness cannot be established, and the only safe
    // answer is to touch nothing. The registration stays, so the scan keeps
    // reporting it and the operator keeps seeing the same EISDIR until they clear
    // the directory themselves.
    errno = 0;
    int rc = ctl->destroy_orphan(rec);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EISDIR, e) << "a directory tombstone is operator state: report it, do not delete it";
    EXPECT_EQ(0, ::access(cn.c_str(), F_OK)) << "the registration was removed without the liveness gate";
    EXPECT_EQ(0, ::access(lp.c_str(), F_OK)) << "the directory was deleted on the operator's behalf";
    bool still = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == id) still = true;
    EXPECT_TRUE(still) << "a refused destroy should leave the orphan reportable";

    // Once the operator clears it, the same call succeeds and takes both halves.
    ASSERT_EQ(0, ::rmdir(lp.c_str()));
    errno = 0;
    rc = ctl->destroy_orphan(rec);
    e = errno;
    EXPECT_EQ(0, rc) << "errno " << e;
    EXPECT_NE(0, ::access(cn.c_str(), F_OK));
    EXPECT_NE(0, ::access(lp.c_str(), F_OK));
}

// The two answers devlock_free() gives when the kernel has NO registration for the
// dev_id, and why they are not the same answer. A tombstone nobody holds is our own
// litter and goes with the device it outlived. One a LIVE SERVER holds is not this
// call's to take even though there is nothing in the kernel for it to stand for:
// that server is between claiming the name and ADD_DEV, and unlinking the file would
// leave the device it then creates with no tombstone -- which list_orphans() skips,
// so it would stop being reported by every later scan.
//
// The dev_id is one no ublk device can have been given, so this is about the
// tombstone alone and cannot collide with a real registration.
TEST_F(UblkTest, destroy_orphan_leaves_a_claimed_tombstone_alone) {
    if (skip_reason) return;
    const std::string sid = "4000000000";
    std::string cn = "/dev/ublkc" + sid;
    std::string lp = std::string(SUITE_LOCKS) + "/ublk-" + sid + ".lock";
    ASSERT_NE(0, ::access(cn.c_str(), F_OK)) << "this dev_id unexpectedly exists";

    BlkDevInfo rec;
    rec.identity = sid;
    rec.size = IMG_SIZE;
    rec.sector_size_shift = 9;

    // Claim it from this process: that is what makes it a live server's rather than
    // litter. A process that dies drops its flock, so "held" always means live.
    int fd = ::open(lp.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    DEFER({ if (fd >= 0) ::close(fd); ::unlink(lp.c_str()); });
    ASSERT_EQ(0, ::flock(fd, LOCK_EX | LOCK_NB));

    errno = 0;
    EXPECT_EQ(-1, ctl->destroy_orphan(rec));
    EXPECT_EQ(EBUSY, errno) << "a tombstone a live server holds is not this call's to remove";
    EXPECT_EQ(0, ::access(lp.c_str(), F_OK)) << "the claimed tombstone was deleted";

    // Let go, and the same call takes the identical file away as litter and reports
    // success -- so the refusal above was about the claim and not about the dev_id.
    ::flock(fd, LOCK_UN);
    ::close(fd);
    fd = -1;
    errno = 0;
    int rc = ctl->destroy_orphan(rec);
    int e = errno;
    EXPECT_EQ(0, rc) << "errno " << e;
    EXPECT_NE(0, ::access(lp.c_str(), F_OK)) << "a tombstone outliving its device survived";
}

TEST_F(UblkTest, resize_dev) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);

    constexpr uint64_t NEW_SIZE = 96ull << 20;
    ASSERT_EQ(0, file->ftruncate(NEW_SIZE));
    ASSERT_EQ(0, dev->resize(NEW_SIZE));

    uint64_t sz = 0;
    test::run_off_vcpu([&] {
        int fd = ::open(node.c_str(), O_RDONLY);
        if (fd < 0) return;
        DEFER(::close(fd));
        if (::ioctl(fd, BLKGETSIZE64, &sz) < 0)
            sz = 0;
    });
    EXPECT_EQ(NEW_SIZE, sz);

    // IO past the old size now works
    EXPECT_EQ(0, device_io(node, pattern(0x66), true, IMG_SIZE + IO_OFF));

    // shrink is rejected
    errno = 0;
    EXPECT_EQ(-1, dev->resize(IMG_SIZE));
    EXPECT_EQ(EINVAL, errno);
}

TEST_F(UblkTest, resize_recover) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    std::string node = node_of(dev);
    uint32_t id = node_dev_id(node.c_str());
    ASSERT_NE(UINT32_MAX, id);

    constexpr uint64_t NEW_SIZE = 96ull << 20;
    ASSERT_EQ(0, file->ftruncate(NEW_SIZE));
    ASSERT_EQ(0, dev->resize(NEW_SIZE));
    ASSERT_EQ(0, dev->detach(true));

    // UPDATE_SIZE writes the new capacity back into the kernel params, so
    // the orphan record carries it and the recovery validates against it
    bool found = false;
    BlkDevInfo rec;
    for (auto& i : ctl->list_orphans())
        if (i.identity == std::to_string(id)) { found = true; rec = i; }
    ASSERT_TRUE(found);
    EXPECT_EQ(NEW_SIZE, rec.size);

    UblkController::Config cfg2(rec);
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    DEFER(dev2->shutdown());
    ASSERT_EQ(0, dev2->start(file));
    EXPECT_EQ(node, node_of(dev2));
    EXPECT_EQ(0, device_io(node, pattern(0x67), true, IMG_SIZE + IO_OFF));
}

// The resize and orphan-scan paths convert between bytes and the ABI's 512-byte
// sectors too: UPDATE_SIZE takes a 512-byte count, dev_sectors is one, and
// list_orphans() scales the registered dev_sectors back to bytes. At a 4 KiB logical
// block all three must use the fixed 9. If the capacity conversion (above) and the
// scan conversion disagree -- one 512-based, one logical-based -- the orphan reports
// 8x the true size, which is why the sites move together.
TEST_F(UblkTest, sector_shift_12_resize_and_orphan_report_true_byte_size) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    cfg.info.sector_size_shift = 12;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    std::string node = node_of(dev);
    uint32_t id = node_dev_id(node.c_str());
    ASSERT_NE(UINT32_MAX, id);

    constexpr uint64_t NEW_SIZE = 96ull << 20;   // a multiple of the 4 KiB block
    ASSERT_EQ(0, file->ftruncate(NEW_SIZE));
    ASSERT_EQ(0, dev->resize(NEW_SIZE));

    uint64_t cap = 0;
    test::run_off_vcpu([&] {
        int fd = ::open(node.c_str(), O_RDONLY);
        if (fd >= 0) { if (::ioctl(fd, BLKGETSIZE64, &cap) < 0) cap = 0; ::close(fd); }
    });
    EXPECT_EQ(NEW_SIZE, cap);
    // IO past the old 64 MiB end reaches the grown region at the right byte offset
    EXPECT_EQ(0, device_io(node, pattern(0x66, 4096), true, IMG_SIZE + 4096));

    ASSERT_EQ(0, dev->detach(true));   // quiesced + lock free = orphan
    DEFER(dev->shutdown());
    bool found = false;
    BlkDevInfo rec;
    for (auto& i : ctl->list_orphans())
        if (i.identity == std::to_string(id)) { found = true; rec = i; }
    ASSERT_TRUE(found);
    EXPECT_EQ(NEW_SIZE, rec.size);
    EXPECT_EQ(12, (int)rec.sector_size_shift);
}

TEST_F(UblkTest, shutdown_busy) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    std::string node = node_of(dev);

    int fd = -1;
    test::run_off_vcpu([&] { fd = ::open(node.c_str(), O_RDONLY); });
    ASSERT_GE(fd, 0);

    errno = 0;
    EXPECT_EQ(-1, dev->shutdown());   // held open -> EBUSY, nothing torn down
    EXPECT_EQ(EBUSY, errno);
    EXPECT_EQ(0, ::access(node.c_str(), F_OK));

    ::close(fd);
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(node.c_str(), F_OK));
}

TEST_F(UblkTest, detach_then_shutdown) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    std::string node = node_of(dev);
    ASSERT_EQ(0, dev->detach(true));
    EXPECT_EQ(0, ::access(node.c_str(), F_OK));   // quiesced, node survives

    // shutdown from the detached state re-claims the free flock, then
    // destroys the registration
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(node.c_str(), F_OK));
    EXPECT_EQ(0, dev->shutdown());   // idempotent
}

TEST_F(UblkTest, adopted_device_left_alone) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev1 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev1);
    DEFER(delete dev1);
    ASSERT_EQ(0, dev1->start(file));
    std::string node = node_of(dev1);
    uint32_t id = node_dev_id(node.c_str());
    ASSERT_NE(UINT32_MAX, id);
    ASSERT_EQ(0, dev1->detach(true));   // flock released: up for adoption

    // a second server adopts the registration and serves it
    UblkController::Config cfg2(make_info());
    cfg2.dev_id = id;
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    ASSERT_EQ(0, dev2->start(file));
    DEFER(dev2->shutdown());
    ASSERT_EQ(0, device_io(node, pattern(0x33), true));

    // dev1 must stand down: its shutdown() finds the flock held by dev2 and
    // leaves the live device alone (DEL_DEV is unconditional -- without the
    // re-claim it would yank the disk out from under dev2)
    EXPECT_EQ(0, dev1->shutdown());
    EXPECT_EQ(0, ::access(node.c_str(), F_OK));
    EXPECT_EQ(0, device_io(node, pattern(0x34), true));

    // a failed start on a THIRD object must not roll back into DEL_DEV either
    UblkController::Config cfg3(make_info());
    cfg3.dev_id = id;
    auto dev3 = ctl->new_device(cfg3);
    ASSERT_NE(nullptr, dev3);
    DEFER(delete dev3);
    errno = 0;
    EXPECT_EQ(-1, dev3->start(file));
    EXPECT_EQ(EBUSY, errno);
    EXPECT_EQ(0, ::access(node.c_str(), F_OK));
    EXPECT_EQ(0, device_io(node, pattern(0x35), true));
}

TEST_F(UblkTest, restart_window_io) {
    if (skip_reason) return;
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    std::string node = node_of(dev);
    uint32_t id = node_dev_id(node.c_str());
    ASSERT_NE(UINT32_MAX, id);

    test::BackgroundWriter w;
    ASSERT_EQ(0, w.start(node, {IO_OFF, IMG_SIZE - IO_OFF, 64 << 10}));
    DEFER(w.stop());   // safety net; the explicit stop below is what normally runs

    // let IO run, then drop the server mid-flight and re-attach: with
    // USER_RECOVERY_REISSUE the initiator must see no error
    photon::thread_usleep(100 * 1000);
    ASSERT_EQ(0, dev->detach(false));
    UblkController::Config cfg2(make_info());
    cfg2.dev_id = id;
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    ASSERT_EQ(0, dev2->start(file));   // a fresh object adopts it by dev_id
    DEFER(dev2->shutdown());
    photon::thread_usleep(100 * 1000);
    EXPECT_EQ(0, w.errors());
    // stop the writer BEFORE the DEFERs fire: it holds the device open, and
    // shutdown() would (correctly) EBUSY against it
    w.stop();
}

// The isolation's only oracle: a consumer wedged on the node must not make the
// device unrecoverable. (a) the caller gets its deadline back instead of the
// wedge, (b) this process goes on asserting, and (c) -- the load-bearing one --
// the daemon takes the registration back and destroys it while the consumer is
// still stuck on it, and the consumer then drains on its own.
//
// (c) is what a consumer inside this process measurably cost once: it shares one
// fd table with the daemon, and that process was unkillable for two days with a
// device that was never recovered.
//
// A red here costs one ublk dev_id and may leave a process that cannot be killed,
// so do not re-run it in a loop and measure the D-state census after any failure.
TEST_F(UblkTest, consumer_hang_is_contained) {
    if (skip_reason) return;
    test::RecordingFile rec(file);
    UblkController::Config cfg(make_info());
    // The consumer drains only once the re-attached daemon has completed its IO,
    // and shutdown() bounds its wait for the node's last opener with this knob.
    // The default is a 2 s budget in total, tuned for a holder that will never
    // leave; here one is expected to, and 2 s is not much against a re-attach.
    cfg.stop_timeout_ms = 20000;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    DEFER(dev->shutdown());
    // Declared AFTER the shutdown DEFER so it runs BEFORE it: a re-attached
    // daemon that finds the gate shut parks on it again and never completes the
    // IO, which is the hang this case is supposed to be immune to.
    DEFER(rec.release_gate(1));
    rec.gated = true;
    ASSERT_EQ(0, dev->start(&rec));

    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());
    uint32_t id = node_dev_id(node.c_str());
    ASSERT_NE(UINT32_MAX, id);
    std::string cdev = "/dev/ublkc" + std::to_string(id);

    // Drop the serving side from underneath the consumer's IO -- on the vcpu,
    // since that is where the serving coroutines are, and only once the kernel
    // says that IO is outstanding.
    std::atomic<uint64_t> parked{0};
    std::atomic<int> drc{-1};
    std::atomic<uint64_t> detach_us{0};
    auto helper = photon::thread_create11([&] {
        // Let the consumer get there first. Dropping the serving side before it
        // has the node open would leave shutdown()'s TRY_STOP with no opener to
        // wait for, and the registration would go out from under a consumer that
        // was never given the chance to wedge on it.
        photon::thread_usleep(300 * 1000);
        for (int i = 0; i < 2000 && !parked.load(); i++) {
            uint64_t n = node_inflight(node);
            if (n)
                parked.store(n);
            else
                photon::thread_usleep(1000);
        }
        uint64_t t0 = photon::now;
        int rc = dev->detach(false);
        // Published before `drc`: that is the flag the body below waits on, so
        // seeing it means the duration is visible too.
        detach_us.store(photon::now - t0);
        drc.store(rc);
    });
    photon::thread_enable_join(helper);

    // 3 s: far above a served IO, which takes milliseconds, and far below what a
    // suite can afford to lose to one consumer.
    static constexpr uint64_t BUDGET_US = 3ull * 1000 * 1000;
    auto wbuf = pattern(0x5a);
    test::DeviceIoOpts o;
    o.timeout_us = BUDGET_US;
    test::ConsumerIoResult rep;
    o.report = &rep;
    int rc = test::device_io(node, wbuf.data(), wbuf.size(), IO_OFF, o);

    // The drop may still be running when the deadline expires. It is bounded --
    // detach's own quiesce wait is -- so wait it out instead of racing it.
    for (int i = 0; i < 10000 && drc.load() < 0; i++)
        photon::thread_usleep(1000);
    LOG_INFO("detach(false) returned ` after ` us, with the consumer still wedged",
             drc.load(), detach_us.load());

    // (a) the caller is back at its deadline and the consumer is not: still
    // alive, and deliberately not signalled -- an uninterruptible sleeper cannot
    // be killed, and a queued signal would only sit next to the wedge.
    EXPECT_EQ(ETIMEDOUT, rc);
    EXPECT_TRUE(rep.hung);
    EXPECT_FALSE(rep.reaped);
    EXPECT_EQ(test::CONS_TIMEOUT, rep.stage);
    EXPECT_GE(rep.elapsed_us, BUDGET_US);
    EXPECT_GT(rep.pid, 0);
    EXPECT_EQ(0, ::kill(rep.pid, 0));
    EXPECT_GE(parked.load(), 1u);   // the kernel counted the IO before the drop
    EXPECT_EQ(0, drc.load());

    // (b) this process is still here and still asserting, and the registration
    // survived the drop. The kernel's outstanding-request count is deliberately
    // NOT sampled here: a drop that worked takes the queue out of dispatch, so a
    // non-zero count at this point would mean the drop did NOT take. `parked`
    // above is that same oracle read at the one moment it can witness anything --
    // before the drop -- and (a)'s hung/CONS_TIMEOUT/kill() are what say the
    // consumer was still stuck right through it.
    EXPECT_EQ(0, ::access(node.c_str(), F_OK));

    // The observation that is not ours: who holds the control device, and who
    // holds the node. Logged rather than asserted, because requiring `fuser` is
    // not something this suite may do -- but a consumer that held the control
    // device would show up here and in (c) below.
    std::string hc = test::sh_off_vcpu("fuser -v " + cdev + " 2>&1 | tr '\\n' ' '");
    std::string hn = test::sh_off_vcpu("fuser -v " + node + " 2>&1 | tr '\\n' ' '");
    LOG_INFO("consumer child ` still wedged; holders of `: ` | holders of `: `",
             (int64_t)rep.pid, cdev, hc, node, hn);

    // (c) Open the gate so the re-attached daemon can complete the IO it
    // inherits, then destroy the device underneath the still-wedged consumer.
    rec.release_gate(1);
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(node.c_str(), F_OK));
    // ... which is what lets the consumer finish: reaped, drained, and reporting
    // the IO it was doing when the serving side vanished as successful.
    EXPECT_TRUE(test::consumer_reap(rep, 30ull * 1000 * 1000));
    EXPECT_FALSE(rep.hung);
    EXPECT_TRUE(rep.reaped);
    EXPECT_EQ(0, rep.status);
    EXPECT_EQ(test::CONS_DONE, rep.stage);
    EXPECT_EQ(0u, rep.fds);
    photon::thread_join((photon::join_handle*)helper);
    test::consumer_release(rep);   // a no-op once reaped; the net for when not
}

// The writer's twin of the case above, and the reason a bounded stop() has to
// return something. A writer is the one consumer that never ends on its own, so
// the caller's deadline is the only thing that can bound it -- and a deadline
// expiring has to be a failure a case can assert on, not a silence that looks
// exactly like a clean stop.
//
// It also has to hand the wedged child over, pid AND result channel together: a
// pid on its own cannot be reaped, because consumer_reap() refuses a child whose
// channel has been released, and a writer this process gave up on would then be
// uncollectable by construction. That is what the out-parameter below is for.
//
// A red here costs one ublk dev_id and may leave a process that cannot be killed,
// so do not re-run it in a loop and measure the D-state census after any failure.
TEST_F(UblkTest, writer_hang_is_contained) {
    if (skip_reason) return;
    test::RecordingFile rec(file);
    UblkController::Config cfg(make_info());
    // As above: shutdown() bounds its wait for the node's last opener with this
    // knob, and here one is expected to leave -- as soon as the gate opens.
    cfg.stop_timeout_ms = 20000;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    test::ConsumerIoResult abandoned;
    DEFER(test::consumer_release(abandoned));   // a no-op once it has been reaped
    DEFER(delete dev);
    DEFER(dev->shutdown());
    // Declared AFTER the shutdown DEFER so it runs BEFORE it, as above: a daemon
    // that finds the gate shut parks on it again and never completes the IO.
    DEFER(rec.release_gate(4096));
    ASSERT_EQ(0, dev->start(&rec));

    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());

    // Declared last so it is destroyed FIRST: on any early return its own stop()
    // gives the writer its bounded deadline before the DEFERs above start taking
    // the device apart. On the path this case actually takes it is already stopped.
    test::BackgroundWriter w;
    ASSERT_EQ(0, w.start(node, {IO_OFF, IMG_SIZE - IO_OFF, 64 << 10}));

    // UNGATED first, and one completed block before the gate shuts. Not tidiness:
    // stop() sets its flag before it waits, so a child that has not got going yet
    // reads that flag on its first pass through the loop and exits having written
    // nothing -- which is a clean stop, not a wedge, and this case would pass by
    // measuring nothing. A completed block is also what says the node is ready, so
    // the writer below is parked in the backend and not in an open that will not
    // resolve.
    ASSERT_TRUE(w.wait_iters(1));
    rec.gated = true;

    // Its next write parks in the backend and the kernel counts it. What "wedged"
    // has to mean here is a counter that has STOPPED, and that needs two windows
    // rather than one: the write already past the gate when it shut is not held by
    // it and completes normally, so a snapshot taken at that instant is one behind
    // the writer's last completed block. The first window is for that write to
    // finish, the second is the measurement -- 200 ms is two orders of magnitude
    // over what one of these writes takes when nothing is holding it.
    uint64_t parked = 0;
    for (int i = 0; i < 5000 && !parked; i++) {
        parked = node_inflight(node);
        if (!parked)
            photon::thread_usleep(1000);
    }
    EXPECT_GE(parked, 1u);
    photon::thread_usleep(200 * 1000);
    uint64_t frozen = w.iters();
    EXPECT_GT(frozen, 0u);
    photon::thread_usleep(200 * 1000);
    EXPECT_EQ(frozen, w.iters());

    // (a) The caller gets its deadline back instead of the wedge. 3 s: far above a
    // served IO, which takes milliseconds, and far below what a suite can afford to
    // lose to one writer.
    static constexpr uint64_t BUDGET_US = 3ull * 1000 * 1000;
    EXPECT_FALSE(w.stop(&abandoned, BUDGET_US));
    EXPECT_TRUE(abandoned.hung);
    EXPECT_FALSE(abandoned.reaped);
    EXPECT_EQ(test::CONS_TIMEOUT, abandoned.stage);
    EXPECT_EQ(ETIMEDOUT, abandoned.status);
    EXPECT_GE(abandoned.elapsed_us, BUDGET_US);
    // Still alive, and deliberately not signalled: an uninterruptible sleeper
    // cannot be killed, and a queued signal would only sit next to the wedge.
    EXPECT_GT(abandoned.pid, 0);
    EXPECT_EQ(0, ::kill(abandoned.pid, 0));
    // The counters stay readable right through the give-up: what the writer got to
    // before it wedged is still there to read, and giving up on it did not cost the
    // caller that. The census is still the "there was none" value -- a wedged writer
    // has produced no final report, and the zeros the channel was born with must not
    // read as a clean one.
    EXPECT_EQ(frozen, w.iters());
    EXPECT_EQ(test::CONS_FDS_UNMEASURED, abandoned.fds);

    // (b) this process is still here and still asserting, and the registration
    // survived the wedge.
    EXPECT_EQ(0, ::access(node.c_str(), F_OK));

    // (c) ... and the child is collectable, because the hand-over carried its
    // channel with it. Opening the gate lets the write it is parked in return, and
    // the loop that returns to it sees the stop flag the give-up already set -- so
    // it drains on its own, the daemon gets the node back, and only then is there a
    // status to read.
    rec.release_gate(4096);
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(node.c_str(), F_OK));
    EXPECT_TRUE(test::consumer_reap(abandoned, 30ull * 1000 * 1000));
    EXPECT_FALSE(abandoned.hung);
    EXPECT_TRUE(abandoned.reaped);
    EXPECT_EQ(0, abandoned.status);
    EXPECT_EQ(test::CONS_DONE, abandoned.stage);
    // The real census, which only a collected child has one to give.
    EXPECT_EQ(0u, abandoned.fds);
}

// CONS_VERIFY's only witness. The comparison a consumer child does between what it
// wrote and what came back is what makes every `EXPECT_EQ(0, device_io(...))` in
// every suite mean something, and no node those suites export has ever disagreed
// with itself -- so the comparison could be deleted, or inverted, or its stage and
// status misassigned, and all of them would stay green. This is the case that says
// otherwise: a backend that serves one wrong byte, and the verdict it produces.
//
// It needs a transport, which is why it is here and not in test-harness.cpp: with no
// daemon between the caller and the node there is nothing to make disagree. Measured
// rather than assumed -- a regular file is coherent by construction, and the three
// pseudo-devices that would answer a read with something other than what was written
// to them (/dev/zero, /dev/null, /dev/urandom) all refuse the fsync that
// consumer_io_body() does between its write and its read-back, so they stop at
// CONS_SYNC and never reach the comparison.
TEST_F(UblkTest, a_read_back_that_disagrees_is_reported_as_a_verify_failure) {
    if (skip_reason) return;
    CorruptingFile rec(file);
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    DEFER(dev->shutdown());
    ASSERT_EQ(0, dev->start(&rec));
    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());

    auto wbuf = pattern(0x6c);
    test::DeviceIoOpts o;
    // O_DIRECT, so the read-back really comes down to this backend instead of being
    // answered out of the page cache the write just filled -- a cached answer agrees
    // with what was written by construction and would prove nothing.
    o.direct = true;
    test::ConsumerIoResult rep;
    o.report = &rep;

    // Honest first, and through the very same call: this is the arm that says the
    // red below is the corruption and not the node, the offset, or the O_DIRECT.
    ASSERT_EQ(0, test::device_io(node, wbuf.data(), wbuf.size(), IO_OFF, o));
    ASSERT_EQ(test::CONS_DONE, rep.stage);

    rec.corrupt_at = IO_OFF;
    EXPECT_EQ(EILSEQ, test::device_io(node, wbuf.data(), wbuf.size(), IO_OFF, o));
    // CONS_VERIFY and not CONS_READ: the read came back whole, it is what came back
    // that was wrong. That distinction is the reason the enumerator exists --
    // CONS_READ names a syscall that failed -- and this is the assertion that pins
    // it to the right one.
    EXPECT_EQ(test::CONS_VERIFY, rep.stage);
    EXPECT_EQ(EILSEQ, rep.status);
    EXPECT_EQ(EILSEQ, rep.exit_code);   // the exit code mirrors the status
    EXPECT_EQ(0, rep.child_errno);      // no syscall failed; the comparison did
    EXPECT_TRUE(rep.reaped);
    EXPECT_FALSE(rep.hung);
    EXPECT_EQ(0u, rep.fds);

    // ... and the node was never the problem: the same IO, with the backend honest
    // again, is clean. Without this arm a red above would be indistinguishable from
    // a case that simply cannot do IO through this device.
    rec.corrupt_at = UINT64_MAX;
    EXPECT_EQ(0, test::device_io(node, wbuf.data(), wbuf.size(), IO_OFF, o));
    EXPECT_EQ(test::CONS_DONE, rep.stage);
    EXPECT_EQ(0, rep.status);
}

// was `dedicated_vcpu`. The assertion is the PLACEMENT, not merely that IO
// works: the pool-wide cursor must hand the two queues two different vcpus,
// and neither may be the caller's, or BlkConfig::pool is decoration.
//
// Both hardware queues have to receive IO for two vcpus to show up, and the
// kernel maps queues by CPU -- the same mechanism `concurrent_stress` above
// relies on. Hence the stress driver rather than two sequential single-thread
// IOs, which would both land on queue 0.
TEST_F(UblkTest, pool_placement_n_less_than_m) {
    if (skip_reason) return;
    if (std::thread::hardware_concurrency() < 2)
        return report_skip("needs >= 2 CPUs to spread IO over both queues");
    test::TestPool pool(4);
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    UblkController::Config cfg(make_info());
    cfg.queues = 2;
    cfg.pool = pool;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());
    EXPECT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "pool n<m", 8));

    EXPECT_EQ(2u, rec.vcpu_count());
    EXPECT_FALSE(rec.ran_on(caller));
}

// n > m: four queues into a two-vcpu pool. Every pool vcpu must be used -- that
// is what "the cursor wraps" means -- and IO must still be correct, which is the
// half that a placement-only assertion would miss.
TEST_F(UblkTest, pool_placement_n_greater_than_m) {
    if (skip_reason) return;
    if (std::thread::hardware_concurrency() < 4)
        return report_skip("needs >= 4 CPUs to spread IO over four queues");
    test::TestPool pool(2);
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    UblkController::Config cfg(make_info());
    cfg.queues = 4;
    cfg.pool = pool;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());
    EXPECT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "pool n>m", 8));

    EXPECT_EQ(2u, rec.vcpu_count());
    EXPECT_FALSE(rec.ran_on(caller));
}

// The pool == nullptr row of the config table: behavior must be exactly what it
// is today. "Exactly" is measurable -- one vcpu, and it is the caller's.
TEST_F(UblkTest, pool_null_serves_on_the_caller_vcpu) {
    if (skip_reason) return;
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    UblkController::Config cfg(make_info());
    cfg.queues = 2;
    cfg.pool = nullptr;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    EXPECT_EQ(0, device_io(node_of(dev), pattern(0x77), /*verify_backend=*/true));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(caller));
}

// n queues with no pool -- the quadrant the config table has that no case
// covered. The kernel must still see n hardware queues, and all n must be
// served from the caller's vcpu.
TEST_F(UblkTest, multiqueue_without_a_pool) {
    if (skip_reason) return;
    // ADD_DEV clamps the queue count to nr_cpu_ids, so four queues is a request this
    // host has to be able to satisfy for the count below to be the one asked for
    if (possible_cpus() < 4)
        return report_skip("needs >= 4 possible CPUs for ADD_DEV to grant four queues");
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    UblkController::Config cfg(make_info());
    cfg.queues = 4;
    cfg.pool = nullptr;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());
    std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;
    EXPECT_EQ(4, test::count_mq_dirs(kname));
    EXPECT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "mq, no pool", 8));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(caller));
}

// ADD_DEV NEGOTIATES the queue count: ublk_drv clamps nr_hw_queues to nr_cpu_ids and
// copies the clamped dev_info back, and the tag set it built has that many queues. So
// the reply, not the request, is what make_queues() has to serve -- serving the request
// mmaps descriptor rings for queue ids the device does not have, and ublk_ch_mmap bounds
// the mapping at nr_hw_queues and answers EINVAL, which failed start() on the first queue
// past the clamp. The clamp is on POSSIBLE cpus, which is why this asks for the transport's
// own maximum: on any host smaller than that the clamp is guaranteed to bite, and a case
// keyed on the online count could silently ask for a number the kernel grants unchanged.
//
// Gated, and CI cannot run it: it needs root and ublk_drv.
TEST_F(UblkTest, add_dev_serves_the_queue_count_the_kernel_negotiated) {
    if (skip_reason) return;
    // utils.h's MAX_QUEUES is the most this transport asks for; this file includes
    // blk.h and harness.h only, so the number is spelled out here.
    constexpr uint32_t ASKED = 64;
    uint32_t possible = possible_cpus();
    ASSERT_GT(possible, 0u) << "cannot read /sys/devices/system/cpu/possible";
    if (possible >= ASKED)
        return report_skip("this host has >= 64 possible CPUs, so ADD_DEV cannot clamp");

    UblkController::Config cfg(make_info());
    cfg.queues = ASKED;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    // the defect: this came back -1/EINVAL, from the descriptor mmap of the first queue
    // id past the clamp
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());

    // blk-mq's own directories, i.e. the kernel's answer rather than anything this
    // library recorded, and strictly fewer than what was asked for -- without that
    // second half the equality below could hold by the request never having been
    // clamped at all.
    std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;
    int mq = test::count_mq_dirs(kname);
    ASSERT_GT(mq, 0);
    EXPECT_EQ((int)possible, mq);
    EXPECT_LT((uint32_t)mq, ASKED);
    // and the device still serves on the geometry it actually got
    EXPECT_EQ(0, device_io(node, pattern(0x5a), /*verify_backend=*/true));
}

// An empty pool must degrade to the caller's vcpu, not divide by zero:
// WorkPool's cursor is `vcpu_index++ % size`, so size 0 there is a SIGFPE --
// migrate_to_pool's short-circuit is the only thing standing in the way.
TEST_F(UblkTest, empty_pool_falls_back_to_the_caller_vcpu) {
    if (skip_reason) return;
    photon::WorkPool empty(0);   // no vcpus, so no engines to match
    ASSERT_EQ(0, empty.get_vcpu_num());
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    UblkController::Config cfg(make_info());
    cfg.queues = 2;
    cfg.pool = &empty;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    EXPECT_EQ(0, device_io(node_of(dev), pattern(0x99), true));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(caller));
}

// check_pool_engines' integration half: the helper is unit-tested on its own, this
// proves the transport actually asks. A pool whose vcpus cannot host the serving
// coroutines is a configuration error, so start() refuses it rather than serve
// pathologically.
//
// The refusal is NOT up front here: start() has already added the kernel device,
// taken the flock and built the queues, so it arrives as a rollback whose del_dev
// discards its return value and is therefore best effort. The residue comparison
// is what turns "best effort" into something observable, and it is taken inside
// the case because the fixture's TearDown sweep runs afterwards and would clean up
// a leak before any outside check could see it.
TEST_F(UblkTest, pool_without_an_event_engine_is_refused) {
    if (skip_reason) return;
    const std::string residue_before = residue();
    photon::WorkPool bad(2);      // ev_engine defaults to 0: no engine at all
    test::RecordingFile rec(file);

    UblkController::Config cfg(make_info());
    cfg.queues = 2;
    cfg.pool = &bad;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(&rec));
    EXPECT_EQ(EINVAL, errno);
    EXPECT_EQ(0u, rec.vcpu_count());
    EXPECT_EQ(residue_before, residue());
}

// Two devices on one pool must not pile onto the same vcpu: the cursor is
// pool-wide, so B's first queue continues where A's last one stopped. A
// single-threaded IO per device is enough to show it -- what matters is that
// the two vcpus differ, not how many of each device's queues got traffic.
TEST_F(UblkTest, two_devices_share_one_pool) {
    if (skip_reason) return;
    test::TestPool pool(4);
    test::RecordingFile rec_a(file);
    // device B needs its OWN backend and its OWN probe: sharing `file` would
    // let each verify read the other's writes, and sharing one RecordingFile
    // would mix the two devices' placements into one set. Declared before
    // rec_b: reverse destruction drops rec_b first, img2 second.
    test::TestImage img2;
    ASSERT_EQ(0, img2.create(IMG2_PATH, IMG_SIZE));
    DEFER(img2.release());
    test::RecordingFile rec_b(img2.file);

    UblkController::Config cfg_a(make_info());
    cfg_a.queues = 2;
    cfg_a.pool = pool;
    auto dev_a = ctl->new_device(cfg_a);
    ASSERT_NE(nullptr, dev_a);
    DEFER(delete dev_a);
    ASSERT_EQ(0, dev_a->start(&rec_a));
    DEFER(dev_a->shutdown());
    EXPECT_EQ(0, device_io(node_of(dev_a), pattern(0x11), true));

    UblkController::Config cfg_b(make_info());
    cfg_b.info.identity = std::string(TEST_IDENTITY) + "-b";
    cfg_b.queues = 2;
    cfg_b.pool = pool;
    auto dev_b = ctl->new_device(cfg_b);
    ASSERT_NE(nullptr, dev_b);
    DEFER(delete dev_b);
    ASSERT_EQ(0, dev_b->start(&rec_b));
    DEFER(dev_b->shutdown());
    // the fixture's device_io helper verifies against the fixture's `file`, so
    // device B goes through test::device_io directly with its own backend
    auto wbuf = pattern(0x22);
    test::DeviceIoOpts o;
    o.backend = img2.file;
    EXPECT_EQ(0, test::device_io(node_of(dev_b), wbuf.data(), wbuf.size(), IO_OFF, o));

    ASSERT_GE(rec_a.vcpu_count(), 1u);
    ASSERT_GE(rec_b.vcpu_count(), 1u);
    for (auto* v : rec_a.vcpus())
        EXPECT_FALSE(rec_b.ran_on(v)) << "both devices' queues landed on one vcpu";
}

TEST_F(UblkTest, fua) {
    if (skip_reason) return;
    // FUA cannot be triggered from the raw node itself: O_DSYNC degenerates
    // to a write plus a separate FLUSH. It is filesystems that put REQ_FUA on
    // writes -- jbd2 writes every ext4 journal commit record with FUA, and
    // the flag only survives to the daemon because we advertised
    // UBLK_ATTR_FUA.
    //
    // What SyncCountingFile can and cannot see now that durability is pwritev plus
    // fdatasync rather than one pwritev2(RWF_DSYNC) call: it CANNOT attribute a sync
    // to the write that asked for it. A FLUSH op produces the same fdatasync, and the
    // kernel is free to put one straight after a write, so no ordering observable from
    // the backend separates the two. The ext4 sequence below is therefore a regression
    // guard -- advertising FUA must not break a real filesystem workload, and that
    // workload's writes and syncs must reach the backend -- and not an FUA
    // discriminator. The per-request discrimination lives in
    // NbdTest.an_fua_write_is_durable_before_its_reply, where the client sets the flag
    // itself rather than leaving it to a filesystem.
    //
    // dsync_writes is the half that IS decisive, and it decides in the negative: it has
    // to stay zero, which is what keeps the pwritev2 base default from swallowing a
    // durability request here again.
    SyncCountingFile cf(file);
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&cf));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);

    // control: a raw-node buffered write + fsync dispatches plain writes plus a FLUSH
    // op. Both counters have to move here, or the two comparisons after the ext4
    // sequence would be comparing a zero against a zero and reporting a pass.
    EXPECT_EQ(0, device_io(node, pattern(0xe1), /*verify_backend=*/false));
    uint64_t w0 = cf.writes.load(), d0 = cf.datasyncs.load();
    EXPECT_GT(w0, 0u);
    EXPECT_GT(d0, 0u) << "the fsync's FLUSH never reached the backend";
    EXPECT_EQ(0u, cf.dsync_writes.load());

    // the mkfs/mount/umount sequence must run off the vcpu: it does IO
    // through the device this daemon (on the vcpu) serves
    static const char MNT[] = "/tmp/photon-blk-ublk-mnt";
    int rc = -1;
    test::run_off_vcpu([&] {
        std::string mkfs = "mkfs.ext4 -q -F " + node + " >/dev/null 2>&1";
        if (::system(mkfs.c_str()) != 0) { rc = -2; return; }
        if (::mkdir(MNT, 0755) != 0 && errno != EEXIST) { rc = -2; return; }
        if (::mount(node.c_str(), MNT, "ext4", 0, nullptr) != 0) { rc = -2; return; }
        int f = ::open((std::string(MNT) + "/f").c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (f < 0) { ::umount2(MNT, MNT_DETACH); rc = errno; return; }
        auto wbuf = pattern(0xe1);
        if (::write(f, wbuf.data(), wbuf.size()) != (ssize_t)wbuf.size()) {
            int e = errno;
            ::close(f);
            ::umount2(MNT, MNT_DETACH);
            rc = e;
            return;
        }
        ::close(f);
        if (::umount(MNT) != 0) { ::umount2(MNT, MNT_DETACH); rc = errno; return; }
        rc = 0;
    });
    DEFER(::rmdir(MNT));
    if (rc == -2)
        return report_skip("mkfs.ext4 or mount not available here");
    ASSERT_EQ(0, rc);
    EXPECT_GT(cf.writes.load(), w0);
    EXPECT_GT(cf.datasyncs.load(), d0);
    EXPECT_EQ(0u, cf.dsync_writes.load());
}

// One controller = one scope. This one builds a SECOND controller on a custom dir
// and checks that its device and its orphan list agree on it, while the fixture's
// controller -- on SUITE_LOCKS -- sees neither.
TEST_F(UblkTest, custom_lock_dir) {
    if (skip_reason) return;
    static const char LOCKS[] = "/tmp/photon-blk-ublk-test-locks";
    ::system(("rm -rf " + std::string(LOCKS)).c_str());

    // No default directory to fall back on: a null or empty one is a refusal. What
    // this pins is that the two independent scopes below really are two, rather
    // than one shared default that both controllers happen to name.
    errno = 0;
    EXPECT_EQ(nullptr, new_ublk_controller(nullptr));
    EXPECT_EQ(EINVAL, errno);
    errno = 0;
    EXPECT_EQ(nullptr, new_ublk_controller(""));
    EXPECT_EQ(EINVAL, errno);

    // The bound is enforced at construction, not by silent truncation: a truncated
    // copy would be a DIFFERENT directory, the exact divergence a controller exists
    // to make impossible. 256 is utils.h's internal SCOPE_DIR_BUF, which the tests
    // do not include.
    std::string too_long = std::string("/tmp/") + std::string(256, 'x');
    errno = 0;
    EXPECT_EQ(nullptr, new_ublk_controller(too_long.c_str()));
    EXPECT_EQ(ENAMETOOLONG, errno);

    auto ctl2 = new_ublk_controller(LOCKS);
    ASSERT_NE(nullptr, ctl2);
    DEFER(delete ctl2);

    UblkController::Config cfg(make_info());
    auto dev = ctl2->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));   // acquire_lock mkdirs the dir itself
    std::string node = node_of(dev);
    uint32_t id = node_dev_id(node.c_str());
    ASSERT_NE(UINT32_MAX, id);
    // the lock landed exactly in the custom dir; a previous test may have
    // left a stale SUITE_LOCKS lock file behind for this (reused) id, which
    // would fool the SUITE_LOCKS probe below -- remove it
    std::string lp = std::string(LOCKS) + "/ublk-" + std::to_string(id) + ".lock";
    EXPECT_EQ(0, ::access(lp.c_str(), F_OK));
    ::unlink((std::string(SUITE_LOCKS) + "/ublk-" + std::to_string(id) + ".lock").c_str());

    ASSERT_EQ(0, dev->detach(true));   // quiesced + custom lock free = orphan

    // visible through the custom dir, invisible through SUITE_LOCKS
    bool found = false;
    for (auto& i : ctl2->list_orphans())
        if (i.identity == std::to_string(id)) found = true;
    EXPECT_TRUE(found);
    for (auto& i : ctl->list_orphans())
        EXPECT_NE(std::to_string(id), i.identity);

    // sweep_orphans probes only SUITE_LOCKS, so this test must remove
    // its own device: re-attach through the same controller and shut down
    UblkController::Config cfg2(make_info());
    cfg2.dev_id = id;
    auto dev2 = ctl2->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    ASSERT_EQ(0, dev2->start(file));
    ASSERT_EQ(0, dev2->shutdown());
    EXPECT_NE(0, ::access(node.c_str(), F_OK));
}

TEST_F(UblkTest, timeout_knobs) {
    if (skip_reason) return;
    // stop_timeout_ms bounds shutdown()'s TRY_STOP retry window against a
    // persistent holder
    UblkController::Config cfg(make_info());
    cfg.stop_timeout_ms = 100;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    std::string node = node_of(dev);

    int fd = -1;
    test::run_off_vcpu([&] { fd = ::open(node.c_str(), O_RDONLY); });
    ASSERT_GE(fd, 0);

    auto t0 = std::chrono::steady_clock::now();
    errno = 0;
    EXPECT_EQ(-1, dev->shutdown());
    EXPECT_EQ(EBUSY, errno);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    EXPECT_GE(ms, 50);      // it retried, not failed on the first EBUSY
    EXPECT_LT(ms, 1500);    // the 100ms knob, not the 2s default

    ::close(fd);
    ASSERT_EQ(0, dev->shutdown());

    // quiesce_timeout_ms = 0: detach() declines to wait out the kernel's
    // async quiesce, yet the device still quiesces on its own -- verify it
    // turns into a listed orphan shortly after (the test supplies the
    // patience detach declined to), then recover and remove it
    UblkController::Config cfg2(make_info());
    cfg2.quiesce_timeout_ms = 0;
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    ASSERT_EQ(0, dev2->start(file));
    uint32_t id = node_dev_id(node_of(dev2).c_str());
    ASSERT_NE(UINT32_MAX, id);
    ASSERT_EQ(0, dev2->detach(true));
    bool orphaned = false;
    for (int i = 0; i < 500 && !orphaned; i++) {
        for (auto& r : ctl->list_orphans())
            if (r.identity == std::to_string(id)) orphaned = true;
        if (!orphaned)
            photon::thread_usleep(10 * 1000);
    }
    EXPECT_TRUE(orphaned);
    UblkController::Config cfg3(make_info());
    cfg3.dev_id = id;
    auto dev3 = ctl->new_device(cfg3);
    ASSERT_NE(nullptr, dev3);
    DEFER(delete dev3);
    ASSERT_EQ(0, dev3->start(file));
    ASSERT_EQ(0, dev3->shutdown());
}

// BlkConfig::spin_us has three modes that select what the pump does when idle:
//   0          — block immediately (zero idle CPU)
//   UINT32_MAX — spin forever (lowest latency, burns a vCPU)
//   other      — spin for that many µs after last work, then block
// All three must produce correct I/O; a broken predicate (wrong comparison,
// dropped UINT32_MAX case) will hang or fail in at least one mode.
TEST_F(UblkTest, spin_us_modes) {
    if (skip_reason) return;
    const uint32_t modes[] = {0, UINT32_MAX, 50000};
    for (uint32_t sp : modes) {
        UblkController::Config cfg(make_info());
        cfg.spin_us = sp;
        auto dev = ctl->new_device(cfg);
        ASSERT_NE(nullptr, dev) << "spin_us=" << sp;
        DEFER(delete dev);
        ASSERT_EQ(0, dev->start(file)) << "spin_us=" << sp;
        DEFER(dev->shutdown());
        std::string node = node_of(dev);
        ASSERT_FALSE(node.empty()) << "spin_us=" << sp;
        EXPECT_EQ(0, device_io(node, pattern((uint8_t)(sp & 0xff)), true))
            << "I/O failed under spin_us=" << sp;
        dev->shutdown();
    }
}

// spin_us' finite mode is documented as busy-polling for that many microseconds AFTER
// THE LAST COMPLETION, then blocking. The queue pump cannot witness a completion itself:
// CascadingEventEngine::wait_for_events returns the number of fd-interest events it
// delivered, and a queue ring registers none -- every cqe it reaps belongs to a parked
// tag coroutine, which it resumes without counting anything. So the return value it used
// to key the window on was always 0 here, the window opened once at queue setup and never
// again, and a queue that had served I/O a second ago was already blocking. The tags now
// stamp the queue's window themselves; this measures the CONTRACT rather than the counter.
//
// The oracle is this thread's CPU time, and the case is built so that the pump is on this
// thread: an idle pump past its window blocks and costs it nothing, while one whose window
// a completion just rearmed busy-polls and costs it nearly the whole wall second. The
// placement that makes the clock valid is asserted below, not assumed.
//
// Gated, and CI cannot run it: it needs root and ublk_drv.
TEST_F(UblkTest, finite_spin_rearms_after_a_completion) {
    if (skip_reason) return;
    constexpr uint64_t SPIN_US = 3 * 1000 * 1000;
    test::RecordingFile rec(file);
    UblkController::Config cfg(make_info());
    cfg.pool = nullptr;   // the queue's pump and tags then live on THIS thread
    cfg.spin_us = (uint32_t)SPIN_US;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);
    ASSERT_FALSE(node.empty());

    // Phase 1 -- the control. Idle past the window that opens at queue setup, then
    // measure. A rearmed pump and one that never rearmed are BOTH blocking here, so what
    // this establishes is that the oracle can read "not spinning" at all: that spin_us is
    // not being honoured as UINT32_MAX, and that nothing else on this thread burns CPU.
    // The quiet loop is not decoration -- udev's probe of a fresh block device is I/O like
    // any other and stamps the window, so a fixed sleep could measure a spin the pump is
    // entitled to be in. Bounded, and a host that will not leave the node alone fails the
    // assertion below rather than looping.
    photon::thread_usleep(SPIN_US + 2 * 1000 * 1000);
    uint64_t idle_ms = 0;
    for (int i = 0; i < 20; i++) {
        idle_ms = thread_cpu_ms_during(500);
        if (idle_ms < 100)
            break;   // quiet: the pump is blocking, which is what this phase measures
    }

    // Phase 2 -- one completion through the device, then measure inside the window that
    // completion must have rearmed. The ASSERT is also the non-vacuity control on this
    // phase: no I/O would mean no completion and nothing to rearm with.
    ASSERT_EQ(0, device_io(node, pattern(0x5a), /*verify_backend=*/true));
    uint64_t spin_ms = thread_cpu_ms_during(1000);

    // The serving coroutines really ran on this thread, which is the whole licence for
    // reading a per-thread clock as a statement about the pump.
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(photon::get_vcpu()));

    EXPECT_LT(idle_ms, 200u) << "the pump never blocked, so the spin_ms below measured nothing";
    EXPECT_GT(spin_ms, 500u) << "the pump did not rearm its spin window after the completion";
}

}  // namespace blk
}  // namespace photon

int main(int argc, char** argv) {
    // A consumer child is this binary re-executed with a sentinel in argv[1]:
    // dispatch it before photon::init() and before gtest sees that argument.
    int cons = photon::blk::test::consumer_child_main(argc, argv);
    if (cons != photon::blk::test::CONS_NOT_A_CHILD)
        return cons;
    if (photon::init(photon::blk::test::TEST_EVENT_ENGINE,
                     photon::blk::test::TEST_IO_ENGINE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
