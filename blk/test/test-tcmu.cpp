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

// Built only on Linux (the tcmu transport is LINUX-gated). Requires root and
// the target_core_user + tcm_loop modules with configfs mounted; otherwise the
// fixture GTEST_SKIPs.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // O_DIRECT
#endif

#include "../blk.h"
#include "../utils.h"

#include "../../test/gtest.h"
#include "harness.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>   // thread_create11 for the gate-release coroutine
#include <photon/thread/workerpool.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>
#include <scsi/sg.h>      // SG_IO passthrough: the unknown-opcode case

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace photon {
namespace blk {

static const char IMG_PATH[]      = "/tmp/photon-blk-tcmu.img";
static constexpr uint64_t IMG_SIZE = 64ull << 20;
static const char TEST_IDENTITY[] = "photon-tcmu-test";
// a fixed WWN keeps the tcm_loop path deterministic for cross-test cleanup
static const char TEST_WWN[]      = "naa.50000000000007e5";
static const char BS_PATH[]       = "/sys/kernel/config/target/core/user_0/photon-tcmu-test";
static const char LB_PATH[]       = "/sys/kernel/config/target/loopback/naa.50000000000007e5";
static const char INQUIRY_VENDOR[]= "PHOTON";  // must match tcmu.cpp's emul_inquiry

// tcmu genetlink ABI (uapi <linux/target_core_user.h>; the enum values are a
// stable ABI, defined locally because that header is C++-hostile -- tcmu.cpp
// hand-rolls the ring ABI for the same reason)
static const char     TCMU_GENL_FAMILY[]      = "TCM-USER";
static const char     TCMU_MCGRP_CONFIG[]     = "config";
static constexpr uint8_t  TCMU_CMD_ADDED_DEVICE = 1;
static constexpr uint16_t TCMU_ATTR_DEVICE      = 1;
static constexpr uint16_t TCMU_ATTR_MINOR       = 2;

static constexpr uint64_t IO_OFF = 1ull << 20;   // 1 MiB
static constexpr size_t   IO_LEN = 256ull << 10; // 256 KiB

// passive-daemon tests: the operator (this test) creates the backstore by raw
// configfs writes; a distinct identity/WWN/size keeps it apart from the
// active-path device. dev_config is simply the backend image path.
static const char PASSIVE_BS[]   = "photon-passive";
static const char PASSIVE_IMG[]  = "/tmp/photon-blk-passive.img";
static constexpr uint64_t PASSIVE_SIZE = 32ull << 20;
static const char PASSIVE_WWN[]  = "naa.50000000000007e6";
static const char PASSIVE_BS_PATH[] = "/sys/kernel/config/target/core/user_0/photon-passive";
static const char PASSIVE_LB_PATH[] = "/sys/kernel/config/target/loopback/naa.50000000000007e6";
static const char REFUSE_BS[]    = "photon-refuse";
static const char REFUSE_BS_PATH[] = "/sys/kernel/config/target/core/user_0/photon-refuse";

// write a configfs/sysfs attribute; returns 0 or -errno (thread-safe errno
// handoff for off_vcpu)
static int cfs_write(const std::string& path, const std::string& val) {
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return -errno;
    ssize_t n = ::write(fd, val.data(), val.size());
    int e = errno;
    ::close(fd);
    return n == (ssize_t)val.size() ? 0 : -(e ? e : EIO);
}

// probe the per-device flock: free => no live server (daemon or active path)
static bool lock_free(const char* identity) {
    std::string lp = std::string("/run/photon-blk/tcmu-") + identity + ".lock";
    int fd = ::open(lp.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (free) ::flock(fd, LOCK_UN);
    ::close(fd);
    return free;
}

// operator-side tcm_loop LUN wiring for a raw backstore (mirrors attach_lun /
// detach_lun in tcmu.cpp; the daemon never touches fabrics)
static int lun_attach(const char* wwn, const char* bs_path, const char* bs_name) {
    std::string lb = std::string("/sys/kernel/config/target/loopback/") + wwn;
    std::string tpgt = lb + "/tpgt_1";
    std::string lun0 = tpgt + "/lun/lun_0";
    if (::mkdir(lb.c_str(), 0755) && errno != EEXIST) return -errno;
    if (::mkdir(tpgt.c_str(), 0755) && errno != EEXIST) return -errno;
    int rc = cfs_write(tpgt + "/nexus", wwn);
    if (rc) return rc;
    if (::mkdir(lun0.c_str(), 0755) && errno != EEXIST) return -errno;
    if (::symlink(bs_path, (lun0 + "/" + bs_name).c_str())) return -errno;
    return 0;
}
static void lun_detach(const char* wwn, const char* bs_name) {
    std::string lb = std::string("/sys/kernel/config/target/loopback/") + wwn;
    std::string tpgt = lb + "/tpgt_1";
    std::string lun0 = tpgt + "/lun/lun_0";
    ::unlink((lun0 + "/" + bs_name).c_str());
    ::rmdir(lun0.c_str());
    ::rmdir(tpgt.c_str());
    ::rmdir(lb.c_str());
}

// read a sysfs attribute, trimmed; returns false if unreadable
static bool sysfs_read(const std::string& path, std::string& out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    char b[256];
    ssize_t r = ::read(fd, b, sizeof(b) - 1);
    ::close(fd);
    if (r <= 0) return false;
    b[r] = '\0';
    out.assign(b);
    size_t s = out.find_first_not_of(" \t\r\n");
    size_t e = out.find_last_not_of(" \t\r\n");
    out = (s == std::string::npos) ? "" : out.substr(s, e - s + 1);
    return true;
}

// find the /dev/sdX whose SCSI vendor is ours (and size matches), or ""
static std::string find_photon_sd(uint64_t expect_sectors) {
    DIR* d = opendir("/sys/block");
    if (!d) return "";
    DEFER(closedir(d));
    struct dirent* e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "sd", 2) != 0) continue;
        std::string vendor;
        if (!sysfs_read(std::string("/sys/block/") + e->d_name + "/device/vendor", vendor))
            continue;
        if (vendor != INQUIRY_VENDOR) continue;
        std::string sz;
        if (sysfs_read(std::string("/sys/block/") + e->d_name + "/size", sz) &&
            expect_sectors && strtoull(sz.c_str(), nullptr, 10) != expect_sectors)
            continue;
        return std::string("/dev/") + e->d_name;
    }
    return "";
}

// the SCSI scan that registers /dev/sdX runs asynchronously after the LUN
// attach; poll on the photon vcpu (yielding to the pump that answers INQUIRY)
static std::string wait_photon_sd(uint64_t expect_sectors, int tries = 5000) {
    for (int i = 0; i < tries; i++) {
        auto s = find_photon_sd(expect_sectors);
        if (!s.empty()) return s;
        photon::thread_usleep(1000);
    }
    return "";
}

// open a freshly-scanned /dev/sdX, retrying the brief window where the device
// node exists (state=running) but open() still returns ENXIO until the block
// layer settles. Must be called off the photon vcpu (from a worker std::thread);
// returns the fd, or -1 with errno preserved.
static int open_sd(const std::string& sd, int mode) {
    return test::open_node(sd, mode);
}

// best-effort removal of any configfs residue for our fixed identities/WWNs, so
// a mid-test ASSERT failure cannot contaminate the next test
static void force_cleanup() {
    auto drop = [](const char* lb_path, const char* bs_path) {
        std::string lun0 = std::string(lb_path) + "/tpgt_1/lun/lun_0";
        if (DIR* d = opendir(lun0.c_str())) {
            struct dirent* e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                ::unlink((lun0 + "/" + e->d_name).c_str());
            }
            closedir(d);
        }
        ::rmdir(lun0.c_str());
        ::rmdir((std::string(lb_path) + "/tpgt_1").c_str());
        ::rmdir(lb_path);
        ::rmdir(bs_path);   // rmdir disables + destroys; enable accepts only "1"
    };
    drop(LB_PATH, BS_PATH);
    drop(PASSIVE_LB_PATH, PASSIVE_BS_PATH);
    drop(PASSIVE_LB_PATH, REFUSE_BS_PATH);
    ::unlink(PASSIVE_IMG);
}

// The operator is another PROCESS in production (targetcli/rtslib/overlaybd).
// Here it is a thread: with the reply protocol its configfs write BLOCKS until
// the event loop on the photon vcpu answers it, so it cannot run on that vcpu.
// Writes that only need the pump (LUN attach, rescan) use run_off_vcpu instead,
// which is synchronous and leaves the vcpu free.
struct Operator {
    std::thread th;
    int rc = 0;
    template<typename Fn>
    void run(Fn fn) { th = std::thread([this, fn] { rc = fn(); }); }
    int join() {
        if (th.joinable())
            th.join();
        return rc;
    }
};

class TcmuTest : public ::testing::Test {
public:
    test::TestImage img;
    fs::IFile* file = nullptr;
    std::atomic<int> resolved{0};   // successful map_passive calls
    TcmuHBA* sys = nullptr;         // created in SetUp. netlink_reply is
                                    // module-GLOBAL, so the tests that need it
                                    // replace this one rather than hold a second

    // The mapping step the event loop performs for an ADDED event: dev_config is
    // the backend image path; only PASSIVE_IMG maps, anything else is refused
    // with ENOENT (which the caller then reports via TcmuHBA::deny).
    fs::IFile* map_passive(const char* dev_config) {
        if (strcmp(dev_config, PASSIVE_IMG) != 0) {
            errno = ENOENT;
            return nullptr;
        }
        auto f = img.lfs->open(PASSIVE_IMG, O_RDWR | O_CREAT, 0644);
        if (!f)
            return nullptr;
        if (f->ftruncate(PASSIVE_SIZE) != 0) {
            int e = errno ? errno : EIO;
            delete f;
            errno = e;
            return nullptr;
        }
        resolved++;
        return f;
    }

    // A config built from the event alone: the operator's backstore is not a
    // photon one, and the operator attaches its own LUN. identity = ev.bs_name is
    // load-bearing, not convenience: the device derives the backstore it registers
    // and serves from it, and the HBA matches the ADDED's dev_id by that same name
    // -- a mismatch would leave the operator's enable waiting forever.
    TcmuHBA::Config passive_cfg(const TcmuHBA::Event& ev) {
        BlkDevInfo info;
        info.identity = ev.bs_name;
        info.size = ev.size;
        info.sector_size_shift = 9;   // tcmu is always a 512-byte sector
        info.features = FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
        TcmuHBA::Config cfg(info);
        cfg.adopt_external = true;
        cfg.loopback_lun = false;
        return cfg;
    }

    // Serve one ADDED event the way a caller would: map dev_config to a backend
    // and start a device for it. The HBA hands that event's dev_id to the device
    // built for the backstore name, and start() answers ADDED_DEVICE_DONE with it,
    // which unblocks the operator's enable. A mapping failure is reported with
    // deny(), which fails that enable with our errno. The backend stays the
    // caller's (ownership=false), so *bk_out must be deleted AFTER the device.
    IBlkDevice* serve_added(const TcmuHBA::Event& ev, fs::IFile** bk_out) {
        *bk_out = nullptr;
        errno = 0;
        auto bk = map_passive(ev.dev_config);
        if (!bk) {
            sys->deny(ev, errno ? errno : ENOENT);
            return nullptr;
        }
        auto dev = sys->new_device(passive_cfg(ev));
        if (!dev) {
            delete bk;
            return nullptr;
        }
        if (dev->start(bk) < 0) {   // start() answered with the errno
            delete dev;
            delete bk;
            return nullptr;
        }
        *bk_out = bk;
        return dev;
    }

    void SetUp() override {
        if (geteuid() != 0)
            GTEST_SKIP() << "tcmu test requires root";
        if (::access("/sys/kernel/config/target/core", F_OK) != 0)
            GTEST_SKIP() << "configfs target not mounted (target_core_mod?)";
        if (::access("/sys/module/target_core_user", F_OK) != 0)
            GTEST_SKIP() << "target_core_user module not loaded";
        if (::access("/sys/module/tcm_loop", F_OK) != 0)
            GTEST_SKIP() << "tcm_loop module not loaded";
        force_cleanup();  // start from a clean slate
        // after the cleanup, so the startup scan finds nothing to synthesize
        sys = new_tcmu_hba("user_0");
        ASSERT_NE(nullptr, sys);
        ASSERT_EQ(0, img.create(IMG_PATH, IMG_SIZE));
        file = img.file;
    }

    void TearDown() override {
        // delete the HBA FIRST: it restores the module-global reply flag,
        // so the cleanup writes below cannot block on a listener that is gone
        delete sys;
        sys = nullptr;
        img.release();
        force_cleanup();
    }

    BlkDevInfo make_info() {
        BlkDevInfo i;
        i.identity = TEST_IDENTITY;
        i.size = IMG_SIZE;
        i.sector_size_shift = 9;  // tcmu is always 512-byte sector
        i.features = FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
        return i;
    }

    TcmuHBA::Config make_cfg(bool loopback = true) {
        TcmuHBA::Config cfg(make_info());
        cfg.loopback_lun = loopback;
        cfg.loopback_wwn = TEST_WWN;
        cfg.timeout = 30;
        return cfg;
    }

    // run blocking device IO off the photon vcpu; returns 0 on success, errno on
    // a syscall failure, or EILSEQ on a data mismatch. verify_backend reads the
    // range back from backend_file (default: the fixture's backend `file`).
    int device_io(const std::string& sd, const std::vector<char>& wbuf, bool verify_backend,
                  uint64_t off = IO_OFF, fs::IFile* backend_file = nullptr) {
        test::DeviceIoOpts o;
        o.backend = verify_backend ? (backend_file ? backend_file : file) : nullptr;
        return test::device_io(sd, wbuf.data(), wbuf.size(), off, o);
    }

    std::vector<char> pattern(uint8_t seed, size_t n = IO_LEN) { return test::pattern(seed, n); }
};

TEST_F(TcmuTest, config_validation) {
    auto cfg = make_cfg(/*loopback=*/false);

    // the pure config checks are construction-time now: no object at all
    TcmuHBA::Config bad = cfg;
    bad.info.size = 0;
    errno = 0;
    EXPECT_EQ(nullptr, sys->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    bad = cfg;
    bad.info.identity = "";
    errno = 0;
    EXPECT_EQ(nullptr, sys->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    bad = cfg;
    bad.info.size = IMG_SIZE + 500;  // not a multiple of the 512-byte sector
    errno = 0;
    EXPECT_EQ(nullptr, sys->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    // a null backend is start()'s to reject: it is not part of the config
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(nullptr));
    EXPECT_EQ(EINVAL, errno);

    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EALREADY, errno);
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(BS_PATH, F_OK));  // registration gone
}

TEST_F(TcmuTest, loopback_io) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty()) << "no /dev/sdX appeared for the tcm_loop LUN";
    LOG_INFO("tcmu loopback device: `", sd.c_str());
    // two independent derivations must agree: the scan above matches the SCSI
    // vendor and size, get_device_node() walks the fabric's read-only `address`
    // attrib to the exact HCTL. It polls for the block/ link, which the kernel
    // defers to async work -- that work issues READ CAPACITY, so it can only
    // complete while the pump is answering.
    const char* np = dev->get_device_node();
    ASSERT_NE(nullptr, np) << "the tcm_loop LUN's node was not resolved";
    EXPECT_EQ(sd, std::string(np));

    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 7 + 3);
    EXPECT_EQ(0, device_io(sd, wbuf, /*verify_backend=*/true));
}

TEST_F(TcmuTest, backstore_only) {
    auto cfg = make_cfg(/*loopback=*/false);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    // the registration exists but no LUN was attached, so no /dev/sdX is ours
    EXPECT_EQ(0, ::access(BS_PATH, F_OK));
    EXPECT_TRUE(wait_photon_sd(IMG_SIZE / 512, /*tries=*/300).empty());
    EXPECT_EQ(nullptr, dev->get_device_node());
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(BS_PATH, F_OK));
}

TEST_F(TcmuTest, detach_reattach) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());
    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 5 + 11);
    EXPECT_EQ(0, device_io(sd, wbuf, true));

    // orderly detach keeps the registration + LUN, stops serving, frees the lock
    ASSERT_EQ(0, dev->detach(/*wait_pending=*/true));
    EXPECT_EQ(0, ::access(BS_PATH, F_OK));
    EXPECT_EQ(0, ::access((std::string(LB_PATH) + "/tpgt_1").c_str(), F_OK));

    // re-start validates the existing registration and resumes serving
    ASSERT_EQ(0, dev->start(file));
    sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());
    // the LUN survived the detach, so attach_lun took its "already linked" path:
    // the node must be re-resolved there too
    const char* np = dev->get_device_node();
    ASSERT_NE(nullptr, np) << "the tcm_loop LUN's node was not resolved";
    EXPECT_EQ(sd, std::string(np));

    std::vector<char> wbuf2(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf2[i] = (char)(i * 3 + 1);
    EXPECT_EQ(0, device_io(sd, wbuf2, true));

    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(BS_PATH, F_OK));
}

TEST_F(TcmuTest, detach_then_shutdown) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    // orderly detach keeps the registration + LUN, stops serving, frees the lock
    ASSERT_EQ(0, dev->detach(/*wait_pending=*/true));
    EXPECT_EQ(0, ::access(BS_PATH, F_OK));
    EXPECT_EQ(0, ::access((std::string(LB_PATH) + "/tpgt_1").c_str(), F_OK));

    // call shutdown() from the DETACHED state (no intervening start()). Observe:
    // does it actually destroy the registration, and how long does it take?
    auto t0 = std::chrono::steady_clock::now();
    int rc = dev->shutdown();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    bool bs_exists  = (::access(BS_PATH, F_OK) == 0);
    bool lun_exists = (::access((std::string(LB_PATH) + "/tpgt_1").c_str(), F_OK) == 0);
    LOG_INFO("shutdown() after detach: rc=`, took ` ms, backstore_exists=`, lun_exists=`",
             rc, ms, bs_exists, lun_exists);

    // contract (blk.h:98 "detach() + destroy"): shutdown() from the detached
    // state re-serves transiently and tears the registration + LUN down, fast --
    // a live pump answers the LUN-removal commands, so no cmd_time_out stall
    EXPECT_EQ(0, rc);
    EXPECT_FALSE(bs_exists)  << "registration leaked";
    EXPECT_FALSE(lun_exists) << "LUN leaked";
    EXPECT_LT(ms, 10000) << "shutdown stalled (no live pump for LUN teardown)";
}

TEST_F(TcmuTest, read_only) {
    auto cfg = make_cfg(/*loopback=*/true);
    cfg.read_only = true;
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    int rc = -1;
    test::run_off_vcpu([&] {
        std::vector<char> buf(4096, 0x22);
        int fd = open_sd(sd, O_RDWR);
        if (fd < 0) fd = open_sd(sd, O_RDONLY);
        if (fd < 0) { rc = errno ? errno : EIO; return; }
        DEFER(::close(fd));
        // a write must be refused (either the block layer's ro gate or our
        // SCSI DATA_PROTECT sense)
        ssize_t w = ::pwrite(fd, buf.data(), buf.size(), IO_OFF);
        bool write_refused = (w < 0);
        // a read must still succeed
        ssize_t r = ::pread(fd, buf.data(), buf.size(), IO_OFF);
        rc = (write_refused && r == (ssize_t)buf.size()) ? 0 : EACCES;
    });
    EXPECT_EQ(0, rc);
}

// An opcode emulate() does not handle must come back as CHECK CONDITION carrying
// OUR sense: tcmu.cpp fills the sense buffer itself instead of setting the ring's
// UNKNOWN_OP flag, so the reply has to travel the normal completion path and
// arrive intact. A vendor-specific opcode is used because nothing in the block
// layer, the SCSI midlayer or the target has an opinion about it -- the CDB
// reaches the ring untouched, so a failure here is about our handler and not
// about something upstream deciding the command was invalid first.
TEST_F(TcmuTest, unknown_opcode) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    // what the range holds now, so "the backend was untouched" is checkable
    std::vector<char> before(4096, 0x5a);
    struct iovec iov_b{before.data(), before.size()};
    ASSERT_EQ((ssize_t)before.size(), file->preadv(&iov_b, 1, IO_OFF));

    int rc = -1;
    uint8_t status = 0xff;
    uint8_t sense[64] = {};
    unsigned sb_len = 0;
    test::run_off_vcpu([&] {
        int fd = open_sd(sd, O_RDWR);
        if (fd < 0) { rc = errno ? errno : EIO; return; }
        DEFER(::close(fd));
        uint8_t cdb[12] = {0xc0};   // vendor specific; group 5 => 12-byte CDB
        sg_io_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.interface_id = 'S';
        h.cmdp = cdb;
        h.cmd_len = sizeof(cdb);
        h.dxfer_direction = SG_DXFER_NONE;
        h.sbp = sense;
        h.mx_sb_len = sizeof(sense);
        h.timeout = 30000;
        if (::ioctl(fd, SG_IO, &h) < 0) { rc = errno ? errno : EIO; return; }
        status = h.status;
        sb_len = h.sb_len_wr;
        rc = 0;
    });
    ASSERT_EQ(0, rc) << "SG_IO itself failed, errno=" << rc;

    EXPECT_EQ(0x02, (int)status) << "expected CHECK CONDITION, got SCSI status " << (int)status;
    // set_sense writes 18 bytes: 0x70, key at [2], additional length 10 at [7],
    // ASC at [12], ASCQ at [13]
    ASSERT_GE(sb_len, 14u) << "no usable sense came back, sb_len_wr=" << sb_len;
    EXPECT_TRUE(sense[0] == 0x70 || sense[0] == 0x71)
        << "not fixed-format sense, response code " << (int)sense[0];
    EXPECT_EQ(0x05, (int)(sense[2] & 0x0f)) << "sense key is not ILLEGAL_REQUEST";
    EXPECT_EQ(10, (int)sense[7]) << "additional sense length is not 10";
    EXPECT_EQ(0x20, (int)sense[12]) << "ASC is not INVALID COMMAND OPERATION CODE";
    EXPECT_EQ(0x00, (int)sense[13]) << "ASCQ should be 0";

    std::vector<char> after(4096, 0x11);
    struct iovec iov_a{after.data(), after.size()};
    ASSERT_EQ((ssize_t)after.size(), file->preadv(&iov_a, 1, IO_OFF));
    EXPECT_EQ(0, memcmp(before.data(), after.data(), before.size()))
        << "an opcode we do not implement modified the backend";
}

TEST_F(TcmuTest, orphan_list) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    // while serving, the flock is held: not an orphan
    for (auto& o : sys->list_orphans())
        EXPECT_NE(TEST_IDENTITY, o.identity);

    // detach keeps the registration but frees the flock: now an orphan
    ASSERT_EQ(0, dev->detach(/*wait_pending=*/false));
    bool found = false;
    for (auto& o : sys->list_orphans()) {
        if (o.identity == TEST_IDENTITY) {
            found = true;
            EXPECT_EQ(IMG_SIZE, o.size);
            EXPECT_EQ(9, (int)o.sector_size_shift);
        }
    }
    EXPECT_TRUE(found) << "detached registration not reported as an orphan";

    // recover: re-start harvests the orphan, then a clean shutdown removes it
    ASSERT_EQ(0, dev->start(file));
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(BS_PATH, F_OK));
}

TEST_F(TcmuTest, discard_write_zeroes) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 13 + 7);

    int rc = -1;
    test::run_off_vcpu([&] {
        int fd = open_sd(sd, O_RDWR);
        if (fd < 0) { rc = errno ? errno : EIO; return; }
        DEFER(::close(fd));
        // lay down a known pattern, then discard it
        if (::pwrite(fd, wbuf.data(), IO_LEN, IO_OFF) != (ssize_t)IO_LEN) { rc = errno ? errno : EIO; return; }
        if (::fsync(fd) < 0) { rc = errno ? errno : EIO; return; }
        uint64_t range[2] = {IO_OFF, IO_LEN};
        if (::ioctl(fd, BLKDISCARD, range) < 0) { rc = errno ? errno : EIO; return; }
        // the discarded range must read back as zero (the trim punched a hole)
        std::vector<char> rbuf(IO_LEN, 0x5a);
        if (::pread(fd, rbuf.data(), IO_LEN, IO_OFF) != (ssize_t)IO_LEN) { rc = errno ? errno : EIO; return; }
        for (size_t i = 0; i < IO_LEN; i++)
            if (rbuf[i] != 0) { rc = EILSEQ; return; }
        rc = 0;
    });

    // we advertise discard (RC16 LBPME + VPD 0xB0), so a failure here is real
    int e = rc;
    ASSERT_EQ(0, e) << "BLKDISCARD/readback failed (errno=" << e << ")";

    // the backend file's range must be zero too (the trim punched a hole)
    std::vector<char> bbuf(IO_LEN, 0x5a);
    struct iovec iov{bbuf.data(), IO_LEN};
    ASSERT_EQ((ssize_t)IO_LEN, file->preadv(&iov, 1, IO_OFF));
    for (size_t i = 0; i < IO_LEN; i++)
        ASSERT_EQ(0, bbuf[i]) << "backend range not discarded at byte " << i;
}

TEST_F(TcmuTest, write_zeroes) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 7 + 3);

    int rc = -1;
    test::run_off_vcpu([&] {
        int fd = open_sd(sd, O_RDWR);
        if (fd < 0) { rc = errno ? errno : EIO; return; }
        DEFER(::close(fd));
        // lay down a known pattern, then zero it out
        if (::pwrite(fd, wbuf.data(), IO_LEN, IO_OFF) != (ssize_t)IO_LEN) { rc = errno ? errno : EIO; return; }
        if (::fsync(fd) < 0) { rc = errno ? errno : EIO; return; }
        uint64_t range[2] = {IO_OFF, IO_LEN};
        if (::ioctl(fd, BLKZEROOUT, range) < 0) { rc = errno ? errno : EIO; return; }
        std::vector<char> rbuf(IO_LEN, 0x5a);
        if (::pread(fd, rbuf.data(), IO_LEN, IO_OFF) != (ssize_t)IO_LEN) { rc = errno ? errno : EIO; return; }
        for (size_t i = 0; i < IO_LEN; i++)
            if (rbuf[i] != 0) { rc = EILSEQ; return; }
        rc = 0;
    });

    // observed: the Linux SCSI disk driver routes BLKZEROOUT to WRITE SAME (not
    // UNMAP) against this device, landing in emul_write_same's zero_fill; a
    // failure here is real
    int e = rc;
    ASSERT_EQ(0, e) << "BLKZEROOUT/readback failed (errno=" << e << ")";

    std::vector<char> bbuf(IO_LEN, 0x5a);
    struct iovec iov{bbuf.data(), IO_LEN};
    ASSERT_EQ((ssize_t)IO_LEN, file->preadv(&iov, 1, IO_OFF));
    for (size_t i = 0; i < IO_LEN; i++)
        ASSERT_EQ(0, bbuf[i]) << "backend range not zeroed at byte " << i;
}

TEST_F(TcmuTest, timeout_knobs) {
    auto cfg = make_cfg(/*loopback=*/false);
    cfg.timeout = 45;
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    // BlkConfig::timeout must land in both attribs: cmd_time_out covers
    // in-flight commands, qfull_time_out is the restart-window queueing knob
    std::string v;
    ASSERT_TRUE(sysfs_read(std::string(BS_PATH) + "/attrib/cmd_time_out", v));
    EXPECT_EQ("45", v);
    ASSERT_TRUE(sysfs_read(std::string(BS_PATH) + "/attrib/qfull_time_out", v));
    EXPECT_EQ("45", v);
}

// resize(): grow-only. The backend is enlarged first (blk.h contract), then
// dev->resize() writes dev_size + pends a capacity-changed UNIT ATTENTION;
// the initiator learns the new capacity via a revalidate (triggered here with
// the sysfs rescan knob, which synchronously re-reads READ CAPACITY -- hence
// off the photon vcpu so the pump can answer).
TEST_F(TcmuTest, resize_grow) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());
    std::string sdname = sd.substr(5);   // "/dev/sdX" -> "sdX"

    // shrink and misalignment are rejected before touching anything
    errno = 0;
    EXPECT_EQ(-1, dev->resize(IMG_SIZE / 2));
    EXPECT_EQ(EINVAL, errno);
    errno = 0;
    EXPECT_EQ(-1, dev->resize(IMG_SIZE + 123));
    EXPECT_EQ(EINVAL, errno);

    // enlarge the backend first, then grow the device
    constexpr uint64_t NEW_SIZE = IMG_SIZE * 2;
    ASSERT_EQ(0, file->ftruncate(NEW_SIZE));
    ASSERT_EQ(0, dev->resize(NEW_SIZE));
    std::string v;
    ASSERT_TRUE(sysfs_read(std::string(BS_PATH) + "/attrib/dev_size", v));
    EXPECT_EQ(std::to_string(NEW_SIZE), v);

    // make the initiator re-read the capacity (absorbs the one-shot UA)
    int rc = -1;
    test::run_off_vcpu([&, sdname] {
        int fd = ::open(("/sys/block/" + sdname + "/device/rescan").c_str(), O_WRONLY);
        if (fd < 0) { rc = errno; return; }
        ssize_t w = ::write(fd, "1", 1);
        ::close(fd);
        rc = (w == 1) ? 0 : EIO;
    });
    ASSERT_EQ(0, rc);

    // the kernel-visible size must grow
    std::string sz;
    bool grown = false;
    for (int i = 0; i < 2000 && !grown; i++) {
        if (sysfs_read("/sys/block/" + sdname + "/size", sz))
            grown = (strtoull(sz.c_str(), nullptr, 10) == NEW_SIZE / 512);
        if (!grown)
            photon::thread_usleep(1000);
    }
    ASSERT_TRUE(grown) << "sd size did not grow after resize+rescan";

    // real IO beyond the OLD capacity must work and land in the backend
    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 11 + 5);
    EXPECT_EQ(0, device_io(sd, wbuf, true, IMG_SIZE + IO_OFF));   // past the old 64 MiB
}

// pool serving: the pump + dispatch pool run on a pool vcpu while the test's
// own vcpu only drives the API. The assertion is the PLACEMENT: tcmu gets ONE
// command ring per device (documented on BlkConfig::queues), so it takes exactly one vcpu from the
// pool no matter how big the pool is -- and the re-started pump is a fresh
// coroutine that takes the cursor's NEXT vcpu, so the set grows to two and
// still excludes the caller's. Also exercises the cross-vcpu surface: IO,
// detach/re-start (each start migrates a fresh pump into the pool), and a
// resize() issued from this vcpu (the capacity atomics are written here, read
// there).
TEST_F(TcmuTest, pool_placement_pump_off_the_caller_vcpu) {
    // TestPool QUERIES the caller's engines instead of spelling one out:
    // check_pool_engines derives its requirement from the caller's own vcpu, so
    // a pool built from that query satisfies it by construction; writing
    // INIT_EVENT_EPOLL here would encode today's recommended_order (epoll ahead
    // of iouring) as if it were a contract.
    test::TestPool pool(2);
    // Declared before cfg/dev so it outlives the device (BlkConfig CONTRACT 1)
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();
    auto cfg = make_cfg(/*loopback=*/true);
    cfg.pool = pool;
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty()) << "no /dev/sdX appeared (pool serving)";

    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 9 + 1);
    EXPECT_EQ(0, device_io(sd, wbuf, /*verify_backend=*/true));

    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_FALSE(rec.ran_on(caller));

    // detach tears down the serving vcpu; re-start spawns a fresh one (the LUN
    // persists across both, so sd stays valid)
    ASSERT_EQ(0, dev->detach(/*wait_pending=*/true));
    ASSERT_EQ(0, dev->start(&rec));
    sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    // resize issued from THIS vcpu; the serving vcpu picks it up via atomics
    constexpr uint64_t NEW_SIZE = IMG_SIZE + (32ull << 20);
    ASSERT_EQ(0, file->ftruncate(NEW_SIZE));
    ASSERT_EQ(0, dev->resize(NEW_SIZE));
    std::string v;
    ASSERT_TRUE(sysfs_read(std::string(BS_PATH) + "/attrib/dev_size", v));
    EXPECT_EQ(std::to_string(NEW_SIZE), v);

    std::vector<char> wbuf2(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf2[i] = (char)(i * 4 + 2);
    EXPECT_EQ(0, device_io(sd, wbuf2, /*verify_backend=*/true));

    // the re-started pump took the cursor's NEXT vcpu, so the set grew to two
    // and still excludes the caller's. Measured here, after the second IO,
    // rather than right after the re-start: placement is recorded by BACKEND
    // IO, and the re-scan of a LUN that persisted issues none.
    EXPECT_EQ(2u, rec.vcpu_count());
    EXPECT_FALSE(rec.ran_on(caller));

    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(BS_PATH, F_OK));
}

// Stop under load: detach(wait_pending=true) flushes a pool-serving device
// while writers are still hammering the node. This is the only way the flush's
// drain_ring meets live handle_cmd completions: both write the mailbox tail,
// and the teardown polls the non-atomic in_flight their DEFERs decrement --
// single-vcpu invariants that hold only when the teardown runs on the vcpu
// that served the ring. So the placement probe is the oracle: the backend IOs
// the flush itself dispatches record the vcpu serve_stop ran on, and that must
// be the pump's, never the caller's. The restart must then harvest the writes
// that parked in the kernel while the ring was down, with the writers seeing
// zero errors.
//
// The gate is what makes the oracle deterministic instead of a race we hope to
// win (the vhost-user detach_waits_for_the_avail_backlog idiom): once every
// backend IO parks inside the probe, the first queue_depth writers' writes
// occupy ALL dispatch slots, so the surplus writers' writes stay UNCONSUMED in
// the ring for as long as the gate is shut -- a pinned, stable state, not a
// few-microsecond window. Ungated, the ring is empty whenever the pump
// happened to drain it last, the flush dispatches nothing, and a teardown on
// the WRONG vcpu goes unobserved. The release 50 ms into the detach only sets
// WHEN the flush can proceed, never WHICH vcpu it runs on.
TEST_F(TcmuTest, pool_serving_stop_under_load) {
    // ONE pool vcpu, so the placement set is {pool} or {pool, caller}, and
    // only the teardown path can add the caller. Engines QUERIED, not spelled
    // out, and declared before cfg/dev so the pool outlives the device --
    // see pool_placement_pump_off_the_caller_vcpu (CONTRACT 1)
    photon::WorkPool pool(1, (int)photon::get_event_engine(),
                             (int)photon::get_io_engine());
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();
    auto cfg = make_cfg(/*loopback=*/true);
    cfg.pool = &pool;
    cfg.queue_depth = 2;   // fewer slots than writers, so the surplus is ring backlog
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty()) << "no /dev/sdX appeared (pool serving)";

    // WRITERS concurrent writers, each on its own region, all UNGATED first:
    // the gate must NOT close before the device has settled, because the
    // kernel's own scan and the writers' opens issue backend IO through this
    // very probe, and parking those strands the scan (measured: the opens then
    // fail ENXIO for their whole retry budget, and the stranded scan IO dies
    // at cmd_time_out). One completed write per writer is the sync point that
    // says the settling IO is all behind us.
    constexpr int WRITERS = 5;
    test::BackgroundWriter bw[WRITERS];
    // Safety net, release FIRST: stop() polls coroutine-side for the writer
    // thread to leave its IO, and a writer parked in a gated backend IO only
    // returns once the gate opens -- stopping before releasing would stall.
    DEFER({
        rec.release_gate(4096);
        for (auto& b : bw)
            b.stop();
    });
    for (int i = 0; i < WRITERS; i++) {
        uint64_t base = IO_OFF + (uint64_t)i * (8ull << 20);
        ASSERT_EQ(0, bw[i].start(sd, {base, base + (8ull << 20), 64 << 10,
                                      /*direct=*/true, /*advance=*/true,
                                      /*verify=*/false}));
    }
    for (int i = 0; i < WRITERS; i++)
        ASSERT_TRUE(bw[i].wait_iters(1)) << "writer " << i << " never got going";

    // Close the gate. Every writer is serial, so within milliseconds each one
    // sits in its next write: queue_depth of them dispatched and parked inside
    // record() (holding their slots), the surplus unconsumed in the ring --
    // and nothing can drain any more, which is what makes the state below a
    // pin rather than a window. 200 ms is two orders of margin for that; the
    // snapshot afterwards is the frozen iteration count.
    rec.gated = true;
    photon::thread_usleep(200 * 1000);
    uint64_t pinned[WRITERS];
    for (int i = 0; i < WRITERS; i++)
        pinned[i] = bw[i].iters();

    // The gate must open while the detach is inside its flush, and it cannot
    // open before: this coroutine is created READY on the caller's vcpu, and
    // run_serve_stop does not yield until it waits for the teardown. By then
    // the flush is already parked -- by code order, not by timing: serve_stop
    // joins the pump before it calls drain_ring, and drain_ring needs a
    // dispatch slot, both of which the gate-parked IOs are holding. So the
    // coroutines the flush creates inherit whichever vcpu run_serve_stop put
    // it on, which is the quantity under test. The 50 ms is margin, not a
    // barrier: nothing about this case's colour depends on it.
    auto rth = photon::thread_create11([&] {
        photon::thread_usleep(50 * 1000);
        rec.release_gate(4096);   // comfortably over the parked + backlog IOs
    });
    photon::thread_enable_join(rth);

    // The teardown under test, with the writers still holding writes in the
    // ring. Anything queued after the flush's head snapshot parks in the
    // kernel, so the restart must come BEFORE the writers stop: stopping
    // first would just sit on the parked write's cmd_time_out and report it
    // as a writer error.
    int drc = dev->detach(/*wait_pending=*/true);
    photon::thread_join((photon::join_handle*)rth);
    ASSERT_EQ(0, drc);

    // Positive work count: every pinned write crossed the stop -- queue_depth
    // of them released from the gate, the surplus dispatched by the flush's
    // drain_ring itself. A teardown that dispatched nothing could not advance
    // every writer, and an idle ring would leave this at 0.
    for (int i = 0; i < WRITERS; i++)
        ASSERT_TRUE(bw[i].wait_iters(pinned[i] + 1)) << "writer " << i << " never crossed the stop";
    uint64_t flushed = 0;
    for (int i = 0; i < WRITERS; i++)
        flushed += bw[i].iters() - pinned[i];
    ASSERT_GE(flushed, (uint64_t)WRITERS);

    // THE ORACLE: the flush-dispatched IOs recorded the vcpu serve_stop ran
    // on. Deleting run_serve_stop's thread_migrate puts them on THIS vcpu,
    // where the drain double-writes the mailbox tail against the pool's
    // handle_cmd coroutines and RMWs the non-atomic in_flight across threads.
    EXPECT_FALSE(rec.ran_on(caller));
    EXPECT_EQ(1u, rec.vcpu_count());

    ASSERT_EQ(0, dev->start(&rec));
    sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());
    for (int i = 0; i < WRITERS; i++) {
        uint64_t resumed = bw[i].iters();
        ASSERT_TRUE(bw[i].wait_iters(resumed + 3));   // the parked writes came back
    }

    for (auto& b : bw)
        b.stop();
    for (int i = 0; i < WRITERS; i++)
        EXPECT_EQ(0, bw[i].errors());

    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 7 + 3);
    EXPECT_EQ(0, device_io(sd, wbuf, /*verify_backend=*/true));

    EXPECT_FALSE(rec.ran_on(caller));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_EQ(0, dev->shutdown());
}

// The pool == nullptr row of the config table for tcmu, and the regression
// baseline for the whole conversion: with no pool the pump stays where it
// always was.
TEST_F(TcmuTest, pool_null_serves_on_the_caller_vcpu) {
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();
    auto cfg = make_cfg(/*loopback=*/true);
    cfg.pool = nullptr;
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());
    EXPECT_EQ(0, device_io(sd, pattern(0x5a), /*verify_backend=*/true));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(caller));
}

// tcmu ignores cfg.queues because the kernel hands it one command ring per
// device (documented on BlkConfig::queues). Setting four must not produce four serving vcpus.
TEST_F(TcmuTest, queues_are_ignored) {
    test::TestPool pool(4);
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();
    auto cfg = make_cfg(/*loopback=*/true);
    cfg.queues = 4;
    cfg.pool = pool;
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());
    EXPECT_EQ(0, device_io(sd, pattern(0xa5), true));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_FALSE(rec.ran_on(caller));
}

// High-concurrency stress on the LUN's /dev/sdX: many O_DIRECT threads (so
// every IO goes through the ring, not the page cache), mixed block sizes, and
// self-describing blocks (harness.h) that a reader can validate without
// knowing who wrote them last. DISJOINT proves no cross-thread misrouting
// (each block must carry the reader's own tid+seq); SHARED proves the pump
// survives maximal contention on one region -- a torn or lost write breaks a
// block's invariants even when the owner is legitimately someone else.
TEST_F(TcmuTest, concurrent_stress) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    EXPECT_EQ(0, test::stress_node_both_modes(sd, IMG_SIZE, "tcmu"));
}

// The seamless-restart contract: detach(false) simulates daemon death (ring
// and LUN kept, pending IO parks under cmd/qfull_time_out); a re-start()
// harvests the ring and the initiator must see no IO error, only latency.
TEST_F(TcmuTest, restart_window_io) {
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    // the writer hammers ONE offset and verifies every read-back, so the test
    // can count verified iterations across the outage. BackgroundWriter::stop()
    // never joins on the photon vcpu while IO may be in flight (see harness.h).
    test::BackgroundWriter w;
    ASSERT_EQ(0, w.start(sd, {IO_OFF, 0, IO_LEN, /*direct=*/true,
                              /*advance=*/false, /*verify=*/true}));
    DEFER(w.stop());

    // steady state: a few verified iterations before the outage
    ASSERT_TRUE(w.wait_iters(3));

    // daemon "dies" without draining; IO issued during the outage parks in the
    // kernel under qfull_time_out (30s via make_cfg) -- the window is 300ms
    ASSERT_EQ(0, dev->detach(/*wait_pending=*/false));
    photon::thread_usleep(300 * 1000);

    // restart harvests the parked ring; the initiator must observe no error
    ASSERT_EQ(0, dev->start(file));
    uint64_t resume_from = w.iters();
    ASSERT_TRUE(w.wait_iters(resume_from + 3));

    EXPECT_EQ(0, w.errors());
    EXPECT_GE(w.iters(), 6u);
}

TEST_F(TcmuTest, genetlink_added_device) {
    // subscribe to the TCM-USER "config" multicast group BEFORE start() so we
    // capture the ADDED_DEVICE the kernel fires when the backstore is enabled.
    // This validates the utils.h multicast path against the real kernel ABI.
    GenlSock gs;
    ASSERT_GE(gs.sk, 0);
    int fam = gs.resolve_family(TCMU_GENL_FAMILY);
    ASSERT_GT(fam, 0) << "no TCM-USER genetlink family (target_core_user?)";
    int grp = gs.resolve_mcast_group(TCMU_GENL_FAMILY, TCMU_MCGRP_CONFIG);
    ASSERT_GT(grp, 0);
    ASSERT_EQ(0, gs.tune_for_notifications());
    ASSERT_EQ(0, gs.subscribe((uint32_t)grp));

    auto cfg = make_cfg(/*loopback=*/false);   // no LUN needed; ADDED fires at enable
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    // TCMU_ATTR_DEVICE is the uio name "tcm-user/<hbanum>/<backstore>/<dev_config>"
    std::string want_dev = std::string("tcm-user/0/") + TEST_IDENTITY + "/photon/" + TEST_IDENTITY;
    int got_minor = -1;
    for (int i = 0; i < 2000 && got_minor < 0; i++) {   // the msg may already be buffered
        gs.recv_notifications([&](uint16_t ntype, uint8_t cmd, const char* attrs, size_t alen) {
            if (ntype != (uint16_t)fam || cmd != TCMU_CMD_ADDED_DEVICE)
                return;
            size_t dl = 0;
            const void* d = nla_find(attrs, alen, TCMU_ATTR_DEVICE, &dl);
            if (!d || want_dev != (const char*)d)
                return;
            size_t ml = 0;
            const void* m = nla_find(attrs, alen, TCMU_ATTR_MINOR, &ml);
            if (m && ml >= sizeof(uint32_t))
                memcpy(&got_minor, m, sizeof(uint32_t));
        });
        if (got_minor < 0)
            photon::thread_usleep(1000);
    }
    ASSERT_GE(got_minor, 0) << "no ADDED_DEVICE multicast for " << want_dev;

    // the minor must be the N of the /dev/uioN the kernel registered for us
    int uio_minor = -1;
    if (DIR* d = opendir("/sys/class/uio")) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "uio", 3) != 0)
                continue;
            std::string nm;
            if (sysfs_read(std::string("/sys/class/uio/") + e->d_name + "/name", nm) && nm == want_dev) {
                uio_minor = atoi(e->d_name + 3);
                break;
            }
        }
        closedir(d);
    }
    ASSERT_GE(uio_minor, 0) << "no /sys/class/uio entry for " << want_dev;
    EXPECT_EQ(uio_minor, got_minor);
}

// The active path with the reply protocol ON: every configfs write WE make fires
// an event the kernel then waits on, while this vcpu is blocked inside that very
// write -- so the HBA's listener must answer for its own devices (ADDED at
// enable, RECONFIG at our own resize, matched by self_size, REMOVED at our own
// destroy, with serving already cleared). Without that, this test hangs.
TEST_F(TcmuTest, active_path_under_reply_mode) {
    delete sys;   // SetUp built this one with netlink_reply off; replace it
    sys = new_tcmu_hba("user_0", /*netlink_reply=*/true);
    ASSERT_NE(nullptr, sys);
    auto cfg = make_cfg(/*loopback=*/false);   // no LUN: keep to the configfs path
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    ASSERT_EQ(0, file->ftruncate(IMG_SIZE * 2));
    ASSERT_EQ(0, dev->resize(IMG_SIZE * 2));
    std::string v;
    ASSERT_TRUE(sysfs_read(std::string(BS_PATH) + "/attrib/dev_size", v));
    EXPECT_EQ(std::to_string(IMG_SIZE * 2), v);

    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(BS_PATH, F_OK));
}

// Two TcmuHBA objects, the reply-mode one on a DIFFERENT HBA. Raising the flag is
// module-GLOBAL, so from then on the kernel arms a *_DONE wait on EVERY
// backstore's configfs writes -- and it waits even when the multicast found no
// listener at all (tcmu_netlink_event_send tolerates -ESRCH for ADDED and then
// waits anyway), so an unanswered enable hangs uninterruptibly. Our plain
// instance never answers (it did not engage the protocol), so what saves its
// start() is the per-backstore opt-out the create path writes
// (nl_reply_supported=-1, v4.15+). The two HBAs matter: on the SAME one the
// reply-mode instance would answer for our device anyway (flock held -> "staying
// out" -> status 0) and mask a missing opt-out; across HBAs it refuses foreign
// events with -ENOSYS instead, so this test fails without the opt-out. Below
// v4.15 there is no opt-out and the failure mode is a HANG, hence the skip.
TEST_F(TcmuTest, two_instances_one_reply_mode) {
    const char HBA0[] = "/sys/kernel/config/target/core/user_0";
    const char HBA1[] = "/sys/kernel/config/target/core/user_1";
    const char BS1[]  = "/sys/kernel/config/target/core/user_1/photon-twoinst";
    int hrc = ::mkdir(HBA0, 0755);
    ASSERT_TRUE(hrc == 0 || errno == EEXIST) << "cannot create the HBA dir: " << strerror(errno);

    // the opt-out is v4.15+; probe it on a throwaway (never enabled) backstore
    std::string probe = std::string(HBA0) + "/photon-probe-attrib";
    ASSERT_EQ(0, ::mkdir(probe.c_str(), 0755));
    DEFER(::rmdir(probe.c_str()));   // never enabled, so the rmdir always works
    if (::access((probe + "/attrib/nl_reply_supported").c_str(), F_OK) != 0)
        GTEST_SKIP() << "no per-backstore nl_reply_supported (needs kernel v4.15+); "
                        "there a plain instance beside a reply-mode one needs "
                        "defensive_reply=true instead";

    delete sys;   // SetUp built this one with netlink_reply off; replace it
    sys = new_tcmu_hba("user_0", /*netlink_reply=*/true);
    ASSERT_NE(nullptr, sys);

    DEFER(::rmdir(HBA1));            // registered first, so it runs last
    auto plain = new_tcmu_hba("user_1");
    ASSERT_NE(nullptr, plain);
    DEFER(delete plain);             // before the fixture's TearDown restores the flag

    auto cfg = make_cfg(/*loopback=*/false);   // no LUN: keep to the configfs path
    cfg.info.identity = "photon-twoinst";
    auto dev = plain->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));   // -ENOSYS from user_0 without the opt-out
    DEFER(dev->shutdown());

    std::string v;
    ASSERT_TRUE(sysfs_read(std::string(BS1) + "/attrib/nl_reply_supported", v));
    EXPECT_EQ("-1", v) << "the plain instance's backstore did not opt out of the reply wait";

    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(BS1, F_OK));
}

// ---------------------------------------------------------------------------
// P3: the passive side (TcmuHBA). The operator (this test) creates and
// configures backstores by raw configfs writes; the event loop on this vcpu
// serves them. With the reply protocol an operator write BLOCKS until this vcpu
// answers it, so those writes run on an Operator thread -- in production the
// operator is another process. Writes that only need the pump (LUN attach,
// rescan) go through run_off_vcpu, which leaves this vcpu free.
// ---------------------------------------------------------------------------

// netlink_reply=true makes every operator write synchronous: enable blocks until
// our ADDED_DEVICE_DONE, dev_size until RECONFIG_DEVICE_DONE (a negative reply
// vetoes the change), rmdir until REMOVED_DEVICE_DONE.
TEST_F(TcmuTest, passive_daemon) {
    delete sys;   // SetUp built this one with netlink_reply off; replace it
    sys = new_tcmu_hba("user_0", /*netlink_reply=*/true);
    ASSERT_NE(nullptr, sys);
    constexpr uint64_t TMO = 30ull * 1000 * 1000;

    Operator op;
    op.run([] {
        if (::mkdir(PASSIVE_BS_PATH, 0755) != 0) return -errno;
        if (int rc = cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_config", PASSIVE_IMG)) return rc;
        if (int rc = cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_size", std::to_string(PASSIVE_SIZE))) return rc;
        return cfs_write(std::string(PASSIVE_BS_PATH) + "/enable", "1");   // blocks until we serve
    });

    TcmuHBA::Event ev;
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    EXPECT_EQ(TcmuHBA::EventKind::ADDED, ev.kind);
    EXPECT_STREQ(PASSIVE_BS, ev.bs_name);
    EXPECT_STREQ(PASSIVE_IMG, ev.dev_config);
    EXPECT_EQ(PASSIVE_SIZE, ev.size);
    EXPECT_EQ(0, strncmp(ev.uio_node, "/dev/uio", 8));
    EXPECT_FALSE(ev.synthesized);
    EXPECT_NE(0u, ev.dev_id);

    fs::IFile* bk = nullptr;
    auto dev = serve_added(ev, &bk);
    ASSERT_NE(nullptr, dev);
    ASSERT_NE(nullptr, bk);
    DEFER(delete bk);
    DEFER(delete dev);
    EXPECT_EQ(0, op.join()) << "the operator's enable did not return once we served";
    EXPECT_EQ(1, resolved.load());
    EXPECT_FALSE(lock_free(PASSIVE_BS));

    // the LUN symlink blocks until the SCSI scan completes, which our pump
    // answers -- so it runs off this vcpu
    int lrc = -1;
    test::run_off_vcpu([&] { lrc = lun_attach(PASSIVE_WWN, PASSIVE_BS_PATH, PASSIVE_BS); });
    ASSERT_EQ(0, lrc);
    std::string sd = wait_photon_sd(PASSIVE_SIZE / 512);
    ASSERT_FALSE(sd.empty()) << "no /dev/sdX for the passive device";
    std::string sdname = sd.substr(5);

    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 6 + 1);
    auto chk = img.lfs->open(PASSIVE_IMG, O_RDWR);
    ASSERT_NE(nullptr, chk);
    DEFER(delete chk);
    ASSERT_EQ(0, device_io(sd, wbuf, true, IO_OFF, chk));

    // operator-driven grow: enlarge the backend first, then dev_size; that write
    // blocks until resize() applies the new size and answers RECONFIG_DEVICE_DONE
    constexpr uint64_t GROWN = 64ull << 20;
    ASSERT_EQ(0, chk->ftruncate(GROWN));
    Operator grow;
    grow.run([] { return cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_size", std::to_string(GROWN)); });
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::RECONFIG, ev.kind);
    EXPECT_STREQ("dev_size", ev.attr);
    EXPECT_EQ(GROWN, ev.size);
    EXPECT_EQ(0, dev->resize(ev.size));
    EXPECT_EQ(0, grow.join());
    std::string v;
    ASSERT_TRUE(sysfs_read(std::string(PASSIVE_BS_PATH) + "/attrib/dev_size", v));
    EXPECT_EQ(std::to_string(GROWN), v);

    // a shrink is vetoed: resize() answers with a negative status, so the write
    // fails with EINVAL and the kernel does not commit the value
    Operator shrink;
    shrink.run([] { return cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_size", std::to_string(PASSIVE_SIZE)); });
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::RECONFIG, ev.kind);
    EXPECT_EQ(-1, dev->resize(ev.size));
    EXPECT_EQ(-EINVAL, shrink.join());

    // a dev_config change cannot be applied (a live backend is not re-mapped), so
    // the event loop denies it and the operator's write fails with our errno
    Operator reconf;
    reconf.run([] { return cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_config", "/tmp/changed.img"); });
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::RECONFIG, ev.kind);
    EXPECT_STREQ("dev_config", ev.attr);
    EXPECT_EQ(0, sys->deny(ev, EINVAL));
    EXPECT_EQ(-EINVAL, reconf.join());
    ASSERT_TRUE(sysfs_read(std::string(PASSIVE_BS_PATH) + "/attrib/dev_config", v));
    EXPECT_EQ(std::string(PASSIVE_IMG), v);

    // the initiator re-reads the capacity (absorbs the one-shot UNIT ATTENTION)
    int rrc = -1;
    test::run_off_vcpu([&] { rrc = cfs_write("/sys/block/" + sdname + "/device/rescan", "1"); });
    ASSERT_EQ(0, rrc);
    std::string sz;
    bool grown = false;
    for (int i = 0; i < 2000 && !grown; i++) {
        if (sysfs_read("/sys/block/" + sdname + "/size", sz))
            grown = (strtoull(sz.c_str(), nullptr, 10) == GROWN / 512);
        if (!grown)
            photon::thread_usleep(1000);
    }
    ASSERT_TRUE(grown) << "sd size did not grow after reconfig+rescan";

    // IO past the OLD capacity works and lands in the backend image
    ASSERT_EQ(0, device_io(sd, wbuf, true, PASSIVE_SIZE + IO_OFF, chk));

    // our OWN resize with the reply protocol on: this write blocks inside the
    // kernel on the caller's vcpu waiting for RECONFIG_DEVICE_DONE, which only
    // the HBA's listener can send (it recognizes the event as ours by
    // self_size). Without that this call deadlocks. It also must NOT be queued
    // back at us -- a spurious event would fail the next wait_for_event below.
    constexpr uint64_t OURS = 96ull << 20;
    ASSERT_EQ(0, chk->ftruncate(OURS));
    ASSERT_EQ(0, dev->resize(OURS));
    ASSERT_TRUE(sysfs_read(std::string(PASSIVE_BS_PATH) + "/attrib/dev_size", v));
    EXPECT_EQ(std::to_string(OURS), v);

    // a device we refuse: deny() fails the operator's enable with our errno
    ASSERT_EQ(0, ::mkdir(REFUSE_BS_PATH, 0755));
    ASSERT_EQ(0, cfs_write(std::string(REFUSE_BS_PATH) + "/attrib/dev_config", "/nonexistent"));
    ASSERT_EQ(0, cfs_write(std::string(REFUSE_BS_PATH) + "/attrib/dev_size", std::to_string(PASSIVE_SIZE)));
    Operator refuse;
    refuse.run([] { return cfs_write(std::string(REFUSE_BS_PATH) + "/enable", "1"); });
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::ADDED, ev.kind);
    EXPECT_STREQ(REFUSE_BS, ev.bs_name);
    fs::IFile* rbk = nullptr;
    EXPECT_EQ(nullptr, serve_added(ev, &rbk));   // map_passive fails -> deny(ENOENT)
    EXPECT_EQ(nullptr, rbk);
    EXPECT_EQ(-ENOENT, refuse.join());
    ASSERT_EQ(0, ::rmdir(REFUSE_BS_PATH));

    // removal: the operator's rmdir blocks until we stop serving and answer
    // REMOVED_DEVICE_DONE, so the flock is free by the time it returns
    test::run_off_vcpu([&] { lun_detach(PASSIVE_WWN, PASSIVE_BS); });
    Operator rm;
    rm.run([] { return ::rmdir(PASSIVE_BS_PATH) == 0 ? 0 : -errno; });
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::REMOVED, ev.kind);
    EXPECT_STREQ(PASSIVE_BS, ev.bs_name);
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_EQ(0, rm.join());
    EXPECT_TRUE(lock_free(PASSIVE_BS));
}

// netlink_reply=false: no synchronous feedback. Every operator write returns at
// once and the event loop learns about the device asynchronously.
TEST_F(TcmuTest, passive_daemon_async) {
    constexpr uint64_t TMO = 30ull * 1000 * 1000;

    ASSERT_EQ(0, ::mkdir(PASSIVE_BS_PATH, 0755));
    ASSERT_EQ(0, cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_config", PASSIVE_IMG));
    ASSERT_EQ(0, cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_size", std::to_string(PASSIVE_SIZE)));
    ASSERT_EQ(0, cfs_write(std::string(PASSIVE_BS_PATH) + "/enable", "1"));   // returns at once

    TcmuHBA::Event ev;
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::ADDED, ev.kind);
    EXPECT_FALSE(ev.synthesized);
    fs::IFile* bk = nullptr;
    auto dev = serve_added(ev, &bk);
    ASSERT_NE(nullptr, dev);
    DEFER(delete bk);
    DEFER(delete dev);
    EXPECT_EQ(1, resolved.load()) << "the event loop did not serve the device";

    // the pump may still be starting; the scan's INQUIRY parks in the ring
    int lrc = -1;
    test::run_off_vcpu([&] { lrc = lun_attach(PASSIVE_WWN, PASSIVE_BS_PATH, PASSIVE_BS); });
    ASSERT_EQ(0, lrc);
    std::string sd = wait_photon_sd(PASSIVE_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 3 + 7);
    auto chk = img.lfs->open(PASSIVE_IMG, O_RDONLY);
    ASSERT_NE(nullptr, chk);
    DEFER(delete chk);
    ASSERT_EQ(0, device_io(sd, wbuf, true, IO_OFF, chk));

    // rmdir disables + destroys; without the reply protocol the REMOVED event is
    // fire-and-forget, so the write returns at once and we shut down afterwards
    test::run_off_vcpu([&] { lun_detach(PASSIVE_WWN, PASSIVE_BS); });
    ASSERT_EQ(0, ::rmdir(PASSIVE_BS_PATH));
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::REMOVED, ev.kind);
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_TRUE(lock_free(PASSIVE_BS));
}

// A backstore configured while no HBA runs is reported by the startup scan
// as a SYNTHESIZED event: dev_id 0, because that configure completed already and
// no reply is owed (or could be sent -- the dev_id only ever existed in the
// missed event). Deleting and recreating the HBA reports it again, and the
// same device object re-adopts the surviving registration.
TEST_F(TcmuTest, passive_daemon_scan) {
    // nothing may be listening while the operator configures the backstore, or
    // the event arrives live and the scan is not what reports it
    delete sys;
    sys = nullptr;
    ASSERT_EQ(0, ::mkdir(PASSIVE_BS_PATH, 0755));
    ASSERT_EQ(0, cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_config", PASSIVE_IMG));
    ASSERT_EQ(0, cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_size", std::to_string(PASSIVE_SIZE)));
    ASSERT_EQ(0, cfs_write(std::string(PASSIVE_BS_PATH) + "/enable", "1"));   // nobody listening
    EXPECT_EQ(0, resolved.load());
    constexpr uint64_t TMO = 30ull * 1000 * 1000;

    sys = new_tcmu_hba("user_0");
    ASSERT_NE(nullptr, sys);
    TcmuHBA::Event ev;
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::ADDED, ev.kind);
    EXPECT_TRUE(ev.synthesized) << "the startup scan did not report the device";
    EXPECT_EQ(0u, ev.dev_id);
    EXPECT_EQ(PASSIVE_SIZE, ev.size);
    EXPECT_STREQ(PASSIVE_BS, ev.bs_name);
    // an operator's backstore is not a photon orphan: list_orphans() reports only
    // registrations whose dev_config is "photon/<identity>"
    for (auto& o : sys->list_orphans())
        EXPECT_NE(std::string(PASSIVE_BS), o.identity);

    fs::IFile* bk = nullptr;
    auto dev = serve_added(ev, &bk);
    ASSERT_NE(nullptr, dev);
    DEFER(delete bk);
    DEFER(delete dev);
    EXPECT_EQ(1, resolved.load());
    EXPECT_FALSE(lock_free(PASSIVE_BS));

    int lrc = -1;
    test::run_off_vcpu([&] { lrc = lun_attach(PASSIVE_WWN, PASSIVE_BS_PATH, PASSIVE_BS); });
    ASSERT_EQ(0, lrc);
    std::string sd = wait_photon_sd(PASSIVE_SIZE / 512);
    ASSERT_FALSE(sd.empty());
    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++)
        wbuf[i] = (char)(i * 5 + 9);
    auto chk = img.lfs->open(PASSIVE_IMG, O_RDONLY);
    ASSERT_NE(nullptr, chk);
    DEFER(delete chk);
    ASSERT_EQ(0, device_io(sd, wbuf, true, IO_OFF, chk));

    // detach keeps the registration and frees the flock, so a FRESH HBA's
    // scan reports the device again
    ASSERT_EQ(0, dev->detach(/*wait_pending=*/false));
    EXPECT_TRUE(lock_free(PASSIVE_BS));
    ASSERT_EQ(0, ::access(PASSIVE_BS_PATH, F_OK));
    delete sys;
    sys = new_tcmu_hba("user_0");
    ASSERT_NE(nullptr, sys);
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::ADDED, ev.kind);
    EXPECT_TRUE(ev.synthesized) << "the restart scan did not re-report the device";
    EXPECT_STREQ(PASSIVE_BS, ev.bs_name);

    // the same device object -- orphaned by the HBA's death, which costs it
    // nothing but the event linkage -- re-adopts the registration and harvests
    // the ring, so the initiator sees no interruption
    ASSERT_EQ(0, dev->start(bk));
    EXPECT_FALSE(lock_free(PASSIVE_BS));
    ASSERT_EQ(0, device_io(sd, wbuf, true, IO_OFF, chk));

    // teardown: the operator detaches its LUN first (the kernel refuses to remove
    // a backstore a LUN still references), then we destroy the registration
    test::run_off_vcpu([&] { lun_detach(PASSIVE_WWN, PASSIVE_BS); });
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(PASSIVE_BS_PATH, F_OK));
    EXPECT_TRUE(lock_free(PASSIVE_BS));
}

}  // namespace blk
}  // namespace photon

int main(int argc, char** argv) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
