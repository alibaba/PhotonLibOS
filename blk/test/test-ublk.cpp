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
// the ublk_drv module (the fixture tries modprobe); otherwise GTEST_SKIPs.

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
#include <photon/thread/workerpool.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
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

// forwards to the wrapped file, counting pwritev2 calls that ask for
// RWF_DSYNC -- the observable signature of the FUA dispatch path
class CountingFile : public fs::IFile {
public:
    fs::IFile* const f;
    std::atomic<uint64_t> dsync_writes{0};
    explicit CountingFile(fs::IFile* file) : f(file) {}

    int close() override { return 0; }   // ownership stays with the fixture
    ssize_t read(void*, size_t) override { errno = ENOSYS; return -1; }
    ssize_t readv(const iovec*, int) override { errno = ENOSYS; return -1; }
    ssize_t write(const void*, size_t) override { errno = ENOSYS; return -1; }
    ssize_t writev(const iovec*, int) override { errno = ENOSYS; return -1; }
    fs::IFileSystem* filesystem() override { return nullptr; }
    ssize_t pread(void* buf, size_t count, off_t off) override { return f->pread(buf, count, off); }
    ssize_t preadv(const iovec* iov, int n, off_t off) override { return f->preadv(iov, n, off); }
    ssize_t pwrite(const void* buf, size_t count, off_t off) override { return f->pwrite(buf, count, off); }
    ssize_t pwritev(const iovec* iov, int n, off_t off) override { return f->pwritev(iov, n, off); }
    ssize_t pwritev2(const iovec* iov, int n, off_t off, int flags) override {
        if (flags & RWF_DSYNC)
            dsync_writes++;
        return f->pwritev2(iov, n, off, flags);
    }
    off_t lseek(off_t off, int whence) override { return f->lseek(off, whence); }
    int fsync() override { return f->fsync(); }
    int fdatasync() override { return f->fdatasync(); }
    int fchmod(mode_t m) override { return f->fchmod(m); }
    int fchown(uid_t u, gid_t g) override { return f->fchown(u, g); }
    int fstat(struct stat* st) override { return f->fstat(st); }
    int ftruncate(off_t len) override { return f->ftruncate(len); }
};

class UblkTest : public ::testing::Test {
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
            GTEST_SKIP() << "ublk test requires root";
        if (::access("/dev/ublk-control", F_OK) != 0) {
            ::system("modprobe ublk_drv 2>/dev/null");
            photon::thread_usleep(100 * 1000);
            if (::access("/dev/ublk-control", F_OK) != 0)
                GTEST_SKIP() << "ublk_drv module not available";
        }
        ctl = new_ublk_controller(nullptr);
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
    // (list_orphans), and DEAD residues (crashed mid-create between
    // ADD_DEV and the lock) which the orphan list deliberately excludes.
    // For every /dev/ublkcN whose flock exists and is free, start() with a
    // dummy backend (a DEAD residue is DEL'd and recreated, a QUIESCED one
    // recovered), then shutdown(). A missing lock file means a foreign
    // device (never ours); a held lock means a live server.
    //
    // Owns its controller rather than using ctl: a GTEST_SKIP in SetUp returns
    // before ctl exists, and TearDown still runs.
    void sweep_orphans() {
        auto c = new_ublk_controller(nullptr);   // the default dir, same scope as ctl
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
                std::string lp = "/run/photon-blk/ublk-" + std::to_string(id) + ".lock";
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

    BlkDevInfo make_info() {
        BlkDevInfo i;
        i.identity = TEST_IDENTITY;
        i.size = IMG_SIZE;
        i.sector_size_shift = 9;
        i.features = FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
        return i;
    }

    // run blocking device IO off the photon vcpu; returns 0 on success, errno
    // on a syscall failure, or EILSEQ on a data mismatch
    int device_io(const std::string& node, const std::vector<char>& wbuf,
                  bool verify_backend, uint64_t off = IO_OFF) {
        test::DeviceIoOpts o;
        o.backend = verify_backend ? file : nullptr;
        return test::device_io(node, wbuf.data(), wbuf.size(), off, o);
    }

    std::vector<char> pattern(uint8_t seed, size_t n = IO_LEN) { return test::pattern(seed, n); }
};

TEST_F(UblkTest, config_validation) {
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

    // a null backend is start()'s to reject: it is not part of the config
    UblkController::Config good(make_info());
    auto dev = ctl->new_device(good);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(nullptr));
    EXPECT_EQ(EINVAL, errno);

    // a second start on the same object must be EALREADY
    ASSERT_EQ(0, dev->start(file));
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EALREADY, errno);
    ASSERT_EQ(0, dev->shutdown());
}

TEST_F(UblkTest, basic_io) {
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

// High-concurrency stress across MULTIPLE hardware queues: the kernel spreads
// the O_DIRECT threads over the queues, so this drives several per-queue
// cascading engines and their tag coroutines at once (basic_io only crosses
// tags inside one queue). Mixed block sizes exercise the iov/segment paths;
// the self-describing blocks attribute any misrouted or torn IO.
TEST_F(UblkTest, concurrent_stress) {
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

TEST_F(UblkTest, orphan_list) {
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

TEST_F(UblkTest, resize_dev) {
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

TEST_F(UblkTest, shutdown_busy) {
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

// was `dedicated_vcpu`. The assertion is the PLACEMENT, not merely that IO
// works: the pool-wide cursor must hand the two queues two different vcpus,
// and neither may be the caller's, or BlkConfig::pool is decoration.
//
// Both hardware queues have to receive IO for two vcpus to show up, and the
// kernel maps queues by CPU -- the same mechanism `concurrent_stress` above
// relies on. Hence the stress driver rather than two sequential single-thread
// IOs, which would both land on queue 0.
TEST_F(UblkTest, pool_placement_n_less_than_m) {
    if (std::thread::hardware_concurrency() < 2)
        GTEST_SKIP() << "needs >= 2 CPUs to spread IO over both queues";
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
    if (std::thread::hardware_concurrency() < 4)
        GTEST_SKIP() << "needs >= 4 CPUs to spread IO over four queues";
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

// An empty pool must degrade to the caller's vcpu, not divide by zero:
// WorkPool's cursor is `vcpu_index++ % size`, so size 0 there is a SIGFPE --
// migrate_to_pool's short-circuit is the only thing standing in the way.
TEST_F(UblkTest, empty_pool_falls_back_to_the_caller_vcpu) {
    photon::WorkPool empty(0, (int)photon::get_event_engine(),
                              (int)photon::get_io_engine());
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

// Two devices on one pool must not pile onto the same vcpu: the cursor is
// pool-wide, so B's first queue continues where A's last one stopped. A
// single-threaded IO per device is enough to show it -- what matters is that
// the two vcpus differ, not how many of each device's queues got traffic.
TEST_F(UblkTest, two_devices_share_one_pool) {
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
    // FUA cannot be triggered from the raw node itself: O_DSYNC degenerates
    // to a write plus a separate FLUSH. It is filesystems that put REQ_FUA on
    // writes -- jbd2 writes every ext4 journal commit record with FUA, and
    // the flag only survives to the daemon because we advertised
    // UBLK_ATTR_FUA. CountingFile observes the resulting RWF_DSYNC dispatch.
    CountingFile cf(file);
    UblkController::Config cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&cf));
    DEFER(dev->shutdown());
    std::string node = node_of(dev);

    // control: raw-node buffered write + fsync dispatches plain writes plus
    // a FLUSH op; neither carries FUA
    EXPECT_EQ(0, device_io(node, pattern(0xe1), /*verify_backend=*/false));
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
        GTEST_SKIP() << "mkfs.ext4 or mount not available in this environment";
    ASSERT_EQ(0, rc);
    EXPECT_GT(cf.dsync_writes.load(), 0u);
}

// One controller = one scope. This one builds a SECOND controller on a custom dir
// and checks that its device and its orphan list agree on it, while the fixture's
// default-dir controller sees neither.
TEST_F(UblkTest, custom_lock_dir) {
    static const char LOCKS[] = "/tmp/photon-blk-ublk-test-locks";
    ::system(("rm -rf " + std::string(LOCKS)).c_str());

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
    // left a stale default-dir lock file behind for this (reused) id, which
    // would fool the default-dir probe below -- remove it
    std::string lp = std::string(LOCKS) + "/ublk-" + std::to_string(id) + ".lock";
    EXPECT_EQ(0, ::access(lp.c_str(), F_OK));
    ::unlink(("/run/photon-blk/ublk-" + std::to_string(id) + ".lock").c_str());

    ASSERT_EQ(0, dev->detach(true));   // quiesced + custom lock free = orphan

    // visible through the custom dir, invisible through the default one
    bool found = false;
    for (auto& i : ctl2->list_orphans())
        if (i.identity == std::to_string(id)) found = true;
    EXPECT_TRUE(found);
    for (auto& i : ctl->list_orphans())
        EXPECT_NE(std::to_string(id), i.identity);

    // sweep_orphans probes only the default dir, so this test must remove
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

}  // namespace blk
}  // namespace photon

int main(int argc, char** argv) {
    // A consumer child is this binary re-executed with a sentinel in argv[1]:
    // dispatch it before photon::init() and before gtest sees that argument.
    int cons = photon::blk::test::consumer_child_main(argc, argv);
    if (cons != photon::blk::test::CONS_NOT_A_CHILD)
        return cons;
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
