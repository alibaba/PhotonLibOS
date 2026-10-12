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
// the target_core_user + tcm_loop modules with configfs mounted; without them
// every case prints a skip notice and returns (see test::SkippableTest).

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

// The namespaces this suite claims: its configfs HBA directory (and a second one
// for the two-instance test), the ownership tag written into dev_config, and the
// tombstone directory. All are stated here rather than left to a library default,
// because there is no default -- two photon-based applications that shared any of
// them would see each other's backstores as their own orphans. The configfs paths
// below are spelled with the macros so that changing one cannot leave the other
// stale.
#define SUITE_SUBTYPE  "user_0"
#define SUITE_SUBTYPE2 "user_1"
#define SUITE_HBANUM   "0"        // SUITE_SUBTYPE after "user_"
#define SUITE_CORE     "/sys/kernel/config/target/core/"
static const char DEV_CONFIG_PREFIX[] = "photon/";
static const char SUITE_LOCKS[]       = "/run/photon-blk";

static const char IMG_PATH[]      = "/tmp/photon-blk-tcmu.img";
static constexpr uint64_t IMG_SIZE = 64ull << 20;
static const char TEST_IDENTITY[] = "photon-tcmu-test";
// a fixed WWN keeps the tcm_loop path deterministic for cross-test cleanup
static const char TEST_WWN[]      = "naa.50000000000007e5";
static const char BS_PATH[]       = SUITE_CORE SUITE_SUBTYPE "/photon-tcmu-test";
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
static const char PASSIVE_BS_PATH[] = SUITE_CORE SUITE_SUBTYPE "/photon-passive";
static const char PASSIVE_LB_PATH[] = "/sys/kernel/config/target/loopback/naa.50000000000007e6";
static const char REFUSE_BS[]    = "photon-refuse";
static const char REFUSE_BS_PATH[] = SUITE_CORE SUITE_SUBTYPE "/photon-refuse";

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

// The scan's tombstone path. Mirrors tcmu's own lock_file_name(), a public
// static of a type that does not exist outside its .cpp -- so this is a
// deliberate second copy of the convention, and it is the only one: every
// probe below goes through here rather than spelling the path out again.
static std::string lock_path(const char* identity) {
    return std::string(SUITE_LOCKS) + "/tcmu-" + identity + ".lock";
}

// probe the per-device flock: free => no live server (daemon or active path)
static bool lock_free(const char* identity) {
    std::string lp = lock_path(identity);
    int fd = ::open(lp.c_str(), O_RDONLY);
    if (fd < 0) return false;
    bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (free) ::flock(fd, LOCK_UN);
    ::close(fd);
    return free;
}

// Distinguishes still-held from everything else, which lock_free() above
// deliberately does not: it collapses "absent" and "held" into one false, and a
// residue check must not, because a lock file that merely EXISTS is this
// transport's own orphan-scan convention rather than residue (devlock_release
// unlocks and closes, it never unlinks). So only a lock that is still HELD
// counts, and that is what would wedge every later run.
static const char* lock_state(const char* identity) {
    std::string lp = lock_path(identity);
    int fd = ::open(lp.c_str(), O_RDONLY);
    if (fd < 0) return "not-held";
    DEFER(::close(fd));
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) return "HELD";
    ::flock(fd, LOCK_UN);
    return "not-held";
}

// A configfs registration marked as ours, which is what makes list_orphans()
// report it at all -- the scan takes ownership from dev_config and nothing else.
// Returns 0 or -errno.
static int plant_backstore(const char* bs_path, const char* identity, uint64_t size) {
    if (::mkdir(bs_path, 0755) != 0 && errno != EEXIST)
        return -errno;
    if (int rc = cfs_write(std::string(bs_path) + "/attrib/dev_config",
                           std::string(DEV_CONFIG_PREFIX) + identity))
        return rc;
    return cfs_write(std::string(bs_path) + "/attrib/dev_size", std::to_string(size));
}

// The state a server that died mid-life leaves behind: that registration,
// enabled, plus a tombstone nobody holds. Planted rather than built through a
// device on purpose -- a device object keeps tracking the registration
// destroy_orphan() is about to take away, and its destructor then goes looking
// for it, re-creating the tombstone and polling for a uio node that cannot
// appear. That would put litter behind the assertions' back.
static int plant_orphan(const char* bs_path, const char* identity, uint64_t size) {
    if (int rc = plant_backstore(bs_path, identity, size))
        return rc;
    if (int rc = cfs_write(std::string(bs_path) + "/enable", "1"))
        return rc;
    std::string lp = lock_path(identity);
    int fd = ::open(lp.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -errno;
    ::close(fd);
    return 0;
}

// How many uio nodes the kernel currently exposes. An enabled tcmu backstore
// gets exactly one, so this is the kernel's own answer to "is a backstore of
// ours still live" -- it reads nothing this suite wrote, which is what makes it
// able to say so rather than merely reporting our bookkeeping.
static int uio_count() {
    int n = 0;
    if (DIR* d = ::opendir("/sys/class/uio")) {
        struct dirent* e;
        while ((e = ::readdir(d)))
            if (strncmp(e->d_name, "uio", 3) == 0 && e->d_name[3] >= '0' && e->d_name[3] <= '9')
                n++;
        ::closedir(d);
    }
    return n;
}

// The kernel-side state a start() refused part-way through could leave behind,
// as one comparable string: the backstore directory, the tcm_loop WWN directory,
// the uio node count, and whether this identity's flock is still held.
static std::string residue() {
    std::string r = "bs=";
    r += (::access(BS_PATH, F_OK) == 0) ? "present" : "absent";
    r += " lb=";
    r += (::access(LB_PATH, F_OK) == 0) ? "present" : "absent";
    r += " uio=";
    r += std::to_string(uio_count());
    r += " lock=";
    r += lock_state(TEST_IDENTITY);
    return r;
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

// A backend whose fallocate always fails EOPNOTSUPP, so IFile::trim() and
// IFile::zero_range() -- both plain wrappers over the VIRTUAL fallocate -- are
// unsupported and zero_fill() must fall back to writing zeroes through pwritev.
// This is the backend shape that makes the LBPRZ contract observable: the device
// advertises LBPRZ (reads after an unmap return zeros), so an unmap the backend
// cannot punch a hole for still has to zero the range rather than report GOOD over
// the old bytes. Does NOT own the wrapped file, exactly as BackendProbe does not.
class NoTrimFile : public test::BackendProbe {
public:
    explicit NoTrimFile(fs::IFile* f) : test::BackendProbe(f) {}
    int fallocate(int, off_t, off_t) override { errno = EOPNOTSUPP; return -1; }
};

// One SG_IO passthrough to the tcm_loop LUN, so a case can send a raw CDB the block
// layer would never build (a malformed WRITE SAME, a transfer count that disagrees
// with the buffer) and read back OUR sense. tcmu forwards the raw CDB to the ring
// (the kernel's tcmu_parse_cdb is passthrough_parse_cdb), so what returns is
// emul_*'s own reply -- the same property unknown_opcode relies on. CALL FROM OFF
// THE VCPU: open_node and the ioctl block. Returns 0 if SG_IO itself ran (status
// and sense are then valid), else the errno.
struct SgResult {
    uint8_t status = 0xff;      // SAM_STAT_*
    unsigned sb_len = 0;        // sense bytes returned
    uint8_t sense[64] = {};
    int key()  const { return sb_len >= 3  ? (int)(sense[2] & 0x0f) : -1; }
    int asc()  const { return sb_len >= 13 ? (int)sense[12] : -1; }
    int ascq() const { return sb_len >= 14 ? (int)sense[13] : -1; }
};

static int sg_io(const std::string& sd, const uint8_t* cdb, size_t cdb_len,
                 void* data, size_t data_len, int dxfer, SgResult& out) {
    int fd = open_sd(sd, O_RDWR);
    if (fd < 0) fd = open_sd(sd, O_RDONLY);
    if (fd < 0) return errno ? errno : EIO;
    DEFER(::close(fd));
    sg_io_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.interface_id = 'S';
    h.cmdp = const_cast<uint8_t*>(cdb);
    h.cmd_len = cdb_len;
    if (data && data_len) {
        h.dxferp = data;
        h.dxfer_len = data_len;
        h.dxfer_direction = dxfer;
    } else {
        h.dxfer_direction = SG_DXFER_NONE;
    }
    h.sbp = out.sense;
    h.mx_sb_len = sizeof(out.sense);
    h.timeout = 30000;
    if (::ioctl(fd, SG_IO, &h) < 0) return errno ? errno : EIO;
    out.status = h.status;
    out.sb_len = h.sb_len_wr;
    return 0;
}

// Assert `r` is CHECK CONDITION carrying ILLEGAL_REQUEST / INVALID FIELD IN CDB,
// the sense emul_* returns for a malformed CDB. Spelled once because four cases
// below pin exactly this and a wrong sense key or ASC must fail them all alike.
#define EXPECT_INVALID_FIELD(r) do {                                            \
        EXPECT_EQ(0x02, (int)(r).status) << "expected CHECK CONDITION";         \
        ASSERT_GE((r).sb_len, 14u) << "no usable sense, sb_len_wr=" << (r).sb_len; \
        EXPECT_EQ(0x05, (r).key()) << "sense key is not ILLEGAL_REQUEST";       \
        EXPECT_EQ(0x24, (r).asc()) << "ASC is not INVALID FIELD IN CDB";        \
    } while (0)

class TcmuTest : public test::SkippableTest {
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
            return report_skip("tcmu test requires root");
        if (::access("/sys/kernel/config/target/core", F_OK) != 0)
            return report_skip("configfs target not mounted (target_core_mod)");
        if (::access("/sys/module/target_core_user", F_OK) != 0)
            return report_skip("target_core_user module not loaded");
        if (::access("/sys/module/tcm_loop", F_OK) != 0)
            return report_skip("tcm_loop module not loaded");
        force_cleanup();  // start from a clean slate
        // after the cleanup, so the startup scan finds nothing to synthesize
        sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS);
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

    // run blocking device IO off the photon vcpu, in a spawned consumer child;
    // harness.h's device_io is the authoritative statement of what it returns.
    // verify_backend reads the range back from backend_file (default: the
    // fixture's backend `file`).
    int device_io(const std::string& sd, const std::vector<char>& wbuf, bool verify_backend,
                  uint64_t off = IO_OFF, fs::IFile* backend_file = nullptr) {
        test::DeviceIoOpts o;
        o.backend = verify_backend ? (backend_file ? backend_file : file) : nullptr;
        return test::device_io(sd, wbuf.data(), wbuf.size(), off, o);
    }

    std::vector<char> pattern(uint8_t seed, size_t n = IO_LEN) { return test::pattern(seed, n); }
};

// The capability half of BlkDevInfo: what lets a caller branch on behaviour instead of
// inferring it from which factory built the object. The axes are properties of the
// transport, so they are already correct on a constructed device; `negotiated` is the
// one member start() has to fill in.
TEST_F(TcmuTest, capabilities_descriptor) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/false);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    const BlkDevInfo& i = dev->get_info();

    // The ring lives in the uio mapping and outlives this process, so a later start()
    // takes over and harvests it. shutdown() unlinks the owned tcm_loop LUN FIRST --
    // scsi_remove_device ends the consumer's session with no refusal of its own -- so
    // this Disconnects rather than refuses; the backstore rmdir's EBUSY, the only
    // refusal there is, covers a LUN an EXTERNAL initiator attached, not the owned one.
    // resize() writes the dev_size attrib and a failure there is an error return, with
    // the kernel raising the UNIT ATTENTION that tells the initiator.
    EXPECT_EQ(BlkBacklog::KernelSide, i.backlog);
    EXPECT_EQ(BlkShutdownRefusal::Disconnects, i.shutdown_refusal);
    EXPECT_EQ(BlkResizeEffect::NotifiedOrFailed, i.resize_effect);
    // start() compares the registered identity and size against this config and refuses
    // a mismatch with EINVAL, and stops there: the feature set is not compared, which is
    // why this is IdentityAndSize and not Full.
    EXPECT_EQ(BlkAdoption::IdentityAndSize, i.adoption);
    // The sharpest illustration of blk.h's two detach() promises being different:
    // serve_stop gates only its ring drain on wait_pending, and its in_flight wait runs
    // unconditionally -- so detach(false) leaves the backlog alone and still waits for
    // the requests already dispatched.
    EXPECT_EQ(false, i.detach_no_wait);
    EXPECT_EQ(FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES, i.offered);
    EXPECT_EQ(0ull, i.negotiated);   // nothing registered yet

    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    // There is no handshake that could decline a feature here: the set is published
    // through the configfs attribs and the inquiry pages, and a failed publish is a
    // failed start(). Spelled out rather than derived from `features & offered`, which
    // is how the implementation computes it and would agree with itself if it were wrong.
    EXPECT_EQ(FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES,
              dev->get_info().negotiated);
}

TEST_F(TcmuTest, config_validation) {
    if (skip_reason) return;
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

// Two devices for one identity map to one backstore name, and the registry keys by that
// name. The second construction must be REFUSED, not allowed to evict the first: an
// unconditional register unregistered a constructed-but-idle first device and let the
// second's start() take over the backstore, and the first's later destructor then erased
// the second -- leaving the name unregistered while a device still believed it served
// it. With the fix the second is not the registered owner, so start() reports EBUSY
// before touching configfs or the flock, and the first stays the owner. Killing
// mutation: restore register_link's unconditional erase+insert and drop start()'s
// !registered EBUSY -- the second device then evicts the first and start()s (returns 0).
TEST_F(TcmuTest, a_second_device_for_a_taken_name_is_refused) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/false);
    auto a = sys->new_device(cfg);
    ASSERT_NE(nullptr, a);
    DEFER(delete a);
    auto b = sys->new_device(cfg);   // same identity => same backstore name
    ASSERT_NE(nullptr, b);
    DEFER(delete b);
    // b was refused the registration, so start() reports EBUSY without serving
    errno = 0;
    EXPECT_EQ(-1, b->start(file));
    EXPECT_EQ(EBUSY, errno);
    // a is still the sole registered owner and can start
    ASSERT_EQ(0, a->start(file));
    EXPECT_EQ(0, a->shutdown());
}

// blk.h's start() contract, the half test::CountingFile exists to witness: an OWNED
// backend is deleted on shutdown, not only by the destructor. Both shutdowns must
// succeed for the release to be owed -- tcmu's propagates a failure to remove the
// backstore, and a registration that survives is one this server can re-serve, so it
// keeps its backend. Backstore only, no tcm_loop LUN: this is about the pointer.
TEST_F(TcmuTest, shutdown_releases_a_backend_it_owns) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/false);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    std::atomic<int> destroyed{0};
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), /*ownership=*/true));
    EXPECT_EQ(0, destroyed.load());
    ASSERT_EQ(0, dev->shutdown());
    EXPECT_EQ(1, destroyed.load());

    // Usable again, which is the transition the leak hid behind: a second start()
    // re-registers the backstore and serves a second backend, instead of overwriting
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
    // leaked after shutdown()->start() was fixed. detach keeps the configfs
    // registration on purpose, and it cannot release the backend either: it doubles
    // as the rollback path of a failed start, where the caller owns it. So start() is
    // the only place that can release what an earlier session owned -- and both
    // halves below are load-bearing, because releasing in detach() instead would
    // pass the second and break the first.
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), true));
    EXPECT_EQ(3, destroyed.load());
    ASSERT_EQ(0, dev->detach(false));
    EXPECT_EQ(3, destroyed.load()) << "detach released a backend it is meant to keep";
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), true));
    EXPECT_EQ(4, destroyed.load()) << "start() overwrote the owned backend detach had kept";
    ASSERT_EQ(0, dev->shutdown());
    EXPECT_EQ(5, destroyed.load());
}

TEST_F(TcmuTest, loopback_io) {
    if (skip_reason) return;
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
    if (skip_reason) return;
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
    if (skip_reason) return;
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
    if (skip_reason) return;
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

    // blk.h's "detach() + destroy" contract: shutdown() from the detached
    // state re-serves transiently and tears the registration + LUN down, fast --
    // a live pump answers the LUN-removal commands, so no cmd_time_out stall
    EXPECT_EQ(0, rc);
    EXPECT_FALSE(bs_exists)  << "registration leaked";
    EXPECT_FALSE(lun_exists) << "LUN leaked";
    EXPECT_LT(ms, 10000) << "shutdown stalled (no live pump for LUN teardown)";
}

TEST_F(TcmuTest, read_only) {
    if (skip_reason) return;
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
    if (skip_reason) return;
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
    if (skip_reason) return;
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

    // Removing the tombstone must make the device UNreportable rather than
    // reportable, and the two states differ here -- the loop above just found
    // it -- which is what makes this an assertion rather than a comment. Note
    // what the tombstone decides: liveness, not ownership. The scan takes
    // ownership from the backstore's dev_config and asks the flock only
    // whether somebody is serving it. The DEFER re-creates the file if this
    // case exits early, before start() below re-plants it; it ignores its own
    // open failure, so it is a best-effort tidy-up, not a guarantee.
    std::string lp = lock_path(TEST_IDENTITY);
    DEFER({
        if (::access(lp.c_str(), F_OK) != 0) {
            int fd = ::open(lp.c_str(), O_CREAT | O_RDWR, 0600);
            if (fd >= 0) ::close(fd);
        }
    });
    ASSERT_EQ(0, ::unlink(lp.c_str()));
    for (auto& o : sys->list_orphans())
        EXPECT_NE(TEST_IDENTITY, o.identity);

    // recover: re-start harvests the orphan, then a clean shutdown removes it
    ASSERT_EQ(0, dev->start(file));
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(BS_PATH, F_OK));
}

TEST_F(TcmuTest, destroy_orphan_removes_a_dead_registration) {
    if (skip_reason) return;
    // Planted with NO HBA listening, for the reason the directory-tombstone case
    // below states in full: an `enable` makes the kernel multicast an ADDED, and
    // the listener's on_added() PROBES the tombstone it is told about -- tcmu.cpp
    // calls devlock_free there, holding LOCK_EX while it does (utils.cpp:148). A
    // probe from this vcpu that overlaps it reads "a live server holds it", so
    // list_orphans() drops that entry -- which cost this case one of its two
    // records -- and destroy_orphan() answers EBUSY. new_tcmu_hba() returns only
    // once the one probe the listener still owes an already-enabled backstore
    // (initial_scan's) is over, so after it there is nothing left to race.
    delete sys;
    sys = nullptr;
    ASSERT_EQ(0, plant_orphan(BS_PATH, TEST_IDENTITY, IMG_SIZE));
    ASSERT_EQ(0, plant_orphan(REFUSE_BS_PATH, REFUSE_BS, IMG_SIZE));
    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS);
    ASSERT_NE(nullptr, sys);
    std::string lp1 = lock_path(TEST_IDENTITY), lp2 = lock_path(REFUSE_BS);
    DEFER({ ::unlink(lp1.c_str()); ::unlink(lp2.c_str());
            ::rmdir(BS_PATH); ::rmdir(REFUSE_BS_PATH); });

    // BEFORE, at observation points that read nothing this suite wrote
    ASSERT_EQ(0, ::access(BS_PATH, F_OK));
    ASSERT_EQ(0, ::access(REFUSE_BS_PATH, F_OK));
    ASSERT_EQ(0, ::access(lp1.c_str(), F_OK));
    ASSERT_EQ(0, ::access(lp2.c_str(), F_OK));
    std::vector<BlkDevInfo> recs;
    for (auto& o : sys->list_orphans())
        if (o.identity == TEST_IDENTITY || o.identity == REFUSE_BS)
            recs.push_back(o);
    ASSERT_EQ(2u, recs.size()) << "the planted registrations were not both reported as orphans";
    for (auto& r : recs)
        EXPECT_EQ(IMG_SIZE, r.size);

    // A counter, not just a return code: a case whose only assertion is "it said
    // 0" can pass while doing nothing at all.
    int destroyed = 0;
    for (auto& r : recs) {
        errno = 0;
        int rc = sys->destroy_orphan(r);
        int e = errno;
        EXPECT_EQ(0, rc) << r.identity << " (errno " << e << ")";
        if (rc == 0)
            destroyed++;
    }
    EXPECT_EQ(2, destroyed);

    // AFTER. The registration and the tombstone are two separate observations
    // because the scan's gate is the registration: once that directory is gone
    // readdir never yields it again, whatever the tombstone does. So "the list no
    // longer reports it" cannot witness a leaked tombstone, and the tombstone has
    // to be asserted on its own.
    EXPECT_NE(0, ::access(BS_PATH, F_OK)) << "the configfs registration survived";
    EXPECT_NE(0, ::access(REFUSE_BS_PATH, F_OK)) << "the configfs registration survived";
    EXPECT_NE(0, ::access(lp1.c_str(), F_OK)) << "the tombstone survived";
    EXPECT_NE(0, ::access(lp2.c_str(), F_OK)) << "the tombstone survived";
    for (auto& o : sys->list_orphans()) {
        EXPECT_NE(TEST_IDENTITY, o.identity);
        EXPECT_NE(REFUSE_BS, o.identity);
    }
}

TEST_F(TcmuTest, destroy_orphan_refuses_a_live_device) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty()) << "no /dev/sdX appeared for the tcm_loop LUN";

    // serving, so a server holds the flock and the scan does not report it
    for (auto& o : sys->list_orphans())
        EXPECT_NE(TEST_IDENTITY, o.identity);

    // A caller's BlkDevInfo from an EARLIER scan. That the scan would not hand
    // back this identity just now is the point of the case rather than a defect
    // in it: the record can predate this server taking the identity over, which
    // is the window the EBUSY gate exists to close.
    BlkDevInfo stale = make_info();
    errno = 0;
    int rc = sys->destroy_orphan(stale);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EBUSY, e) << "a live identity must be refused, not destroyed";

    // Nothing was torn down, and the proof is that it still works: one real I/O
    // through the LUN, verified against the backend image.
    EXPECT_EQ(0, ::access(BS_PATH, F_OK));
    EXPECT_STREQ("HELD", lock_state(TEST_IDENTITY));
    std::vector<char> wbuf = pattern(0x5a);
    EXPECT_EQ(0, device_io(sd, wbuf, /*verify_backend=*/true));
    for (auto& o : sys->list_orphans())
        EXPECT_NE(TEST_IDENTITY, o.identity);
    // Still enabled, too. Worth stating but NOT what witnesses the flock gate:
    // measured, the kernel refuses the removal path's `enable=0` write while a
    // LUN is attached (the best-effort write logs "wrote -1 of 1 (tolerated)")
    // and then refuses the rmdir with EBUSY on its own. So this device is
    // protected twice, and deleting the flock gate would leave this case green --
    // which is why the next case uses a device the kernel will not protect.
    std::string en;
    ASSERT_TRUE(sysfs_read(std::string(BS_PATH) + "/enable", en));
    EXPECT_EQ("1", en) << "a refused destroy left the backstore disabled";
}

// The flock gate's real witness. A backstore-only device has no LUN, so the
// kernel disables and rmdirs it without complaint: the flock is the ONLY thing
// between a caller's stale BlkDevInfo and a live server's registration being
// taken away from under it. This is the case that goes red when the gate is
// deleted, and it is the more realistic of the two -- backstore-only is a
// supported configuration, not a corner.
TEST_F(TcmuTest, destroy_orphan_refuses_a_live_device_the_kernel_will_not) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/false);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    ASSERT_EQ(0, ::access(BS_PATH, F_OK));
    EXPECT_STREQ("HELD", lock_state(TEST_IDENTITY));
    // live, so the scan does not report it
    for (auto& o : sys->list_orphans())
        EXPECT_NE(TEST_IDENTITY, o.identity);

    // a BlkDevInfo from an earlier scan, which is the window the gate closes
    BlkDevInfo stale = make_info();
    errno = 0;
    int rc = sys->destroy_orphan(stale);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EBUSY, e) << "here the flock is the only gate this device has";

    // nothing was touched: still registered, still enabled, still held
    EXPECT_EQ(0, ::access(BS_PATH, F_OK)) << "a live device's registration was destroyed";
    std::string en;
    ASSERT_TRUE(sysfs_read(std::string(BS_PATH) + "/enable", en));
    EXPECT_EQ("1", en) << "a live device was disabled";
    EXPECT_STREQ("HELD", lock_state(TEST_IDENTITY));
    // and it still shuts down cleanly, which it could not if the registration
    // had been taken from under it
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(BS_PATH, F_OK));
}

// Every namespace this library registers into is a required argument with no
// default to fall back on: two photon-based applications that shared one would see
// each other's backstores as their own orphans. This pins the refusals, so a
// default reintroduced later fails here rather than in somebody's deployment.
//
// errno is only asserted where the refusal happens before anything is constructed.
// A bad subtype is rejected inside the HBA's own start(), and the object built
// before it is then deleted -- so what errno holds afterwards is whatever that
// teardown left, which is not this test's business to pin.
TEST_F(TcmuTest, requires_its_namespaces) {
    if (skip_reason) return;
    for (const char* bad : {(const char*)nullptr, ""}) {
        errno = 0;
        EXPECT_EQ(nullptr, new_tcmu_hba(bad, DEV_CONFIG_PREFIX, SUITE_LOCKS)) << "subtype";
        EXPECT_EQ(EINVAL, errno);
        errno = 0;
        EXPECT_EQ(nullptr, new_tcmu_hba(SUITE_SUBTYPE, bad, SUITE_LOCKS)) << "dev_config_prefix";
        EXPECT_EQ(EINVAL, errno);
        errno = 0;
        EXPECT_EQ(nullptr, new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, bad)) << "lock_dir";
        EXPECT_EQ(EINVAL, errno);
    }
    // a prefix the longest backstore name could not fit behind, inside the
    // 256-byte dev_config attrib
    errno = 0;
    std::string long_prefix(64, 'p');
    EXPECT_EQ(nullptr, new_tcmu_hba(SUITE_SUBTYPE, long_prefix.c_str(), SUITE_LOCKS));
    EXPECT_EQ(ENAMETOOLONG, errno);
    // not a TCM-USER fabric directory, so there is no HBA number to answer with
    EXPECT_EQ(nullptr, new_tcmu_hba("not-a-user-hba", DEV_CONFIG_PREFIX, SUITE_LOCKS));
}

// The tcm_loop WWN is required rather than derived, for the same reason: the WWN
// space is host-wide too, so a WWN computed from the identity would hang two
// applications' LUNs off one target whenever they chose the same identity.
TEST_F(TcmuTest, requires_loopback_wwn) {
    if (skip_reason) return;
    TcmuHBA::Config cfg(make_info());
    cfg.loopback_wwn.clear();
    auto bad = sys->new_device(cfg);   // loopback_lun defaults to true
    int e = errno;
    EXPECT_EQ(nullptr, bad);
    EXPECT_EQ(EINVAL, e);

    cfg.loopback_lun = false;          // no LUN, so no WWN to name
    auto dev = sys->new_device(cfg);
    EXPECT_NE(nullptr, dev);
    delete dev;
}

TEST_F(TcmuTest, destroy_orphan_validates_the_identity) {
    if (skip_reason) return;
    // An identity longer than a backstore name can be. The name mapping truncates
    // at 64 bytes, so without the length check this would name the PREFIX's
    // backstore -- planted here at exactly 64 bytes -- and destroy that instead
    // of what the caller asked for.
    const std::string PREFIX(64, 'a');
    std::string pre_path = std::string(SUITE_CORE SUITE_SUBTYPE "/") + PREFIX;
    ASSERT_EQ(0, plant_backstore(pre_path.c_str(), PREFIX.c_str(), IMG_SIZE));
    DEFER(::rmdir(pre_path.c_str()));
    BlkDevInfo over = make_info();
    over.identity = PREFIX + "BBBBBB";
    errno = 0;
    int rc = sys->destroy_orphan(over);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EINVAL, e) << "an over-long identity must not be truncated into another device";
    EXPECT_EQ(0, ::access(pre_path.c_str(), F_OK)) << "the 64-byte prefix's backstore was destroyed";

    // A traversal identity. The mapping turns '/' into '_', so a separator cannot
    // survive -- but that is the claim under test, so the sentinel is a real
    // directory at the exact path this identity WOULD name if one did.
    const char SENTINEL[] = "/tmp/photon-tcmu-destroy-sentinel";
    ASSERT_EQ(0, ::mkdir(SENTINEL, 0755));
    DEFER(::rmdir(SENTINEL));
    BlkDevInfo trav = make_info();
    trav.identity = "../../../../tmp/photon-tcmu-destroy-sentinel";
    errno = 0;
    rc = sys->destroy_orphan(trav);
    e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(ENOENT, e) << "a traversal identity must not resolve to anything";
    EXPECT_EQ(0, ::access(SENTINEL, F_OK)) << "the sentinel was touched";

    // "." and ".." survive the mapping unchanged, because it allows '.', and they
    // are complete path components: they would aim the rmdir at this HBA's own
    // directory and at its parent. Both exist, so both are asserted afterwards
    // rather than assumed to have been spared.
    for (const char* dot : {".", ".."}) {
        BlkDevInfo d = make_info();
        d.identity = dot;
        errno = 0;
        rc = sys->destroy_orphan(d);
        e = errno;
        EXPECT_EQ(-1, rc) << "identity " << dot;
        EXPECT_EQ(EINVAL, e) << "identity " << dot;
    }
    EXPECT_EQ(0, ::access(SUITE_CORE SUITE_SUBTYPE, F_OK)) << "this HBA was removed";
    EXPECT_EQ(0, ::access("/sys/kernel/config/target/core", F_OK)) << "the fabric's core directory was removed";

    // An identity this HBA has no backstore for, and no tombstone either.
    BlkDevInfo gone = make_info();
    gone.identity = "photon-tcmu-never-existed";
    errno = 0;
    rc = sys->destroy_orphan(gone);
    e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(ENOENT, e);
}

TEST_F(TcmuTest, destroy_orphan_outlives_a_directory_tombstone) {
    if (skip_reason) return;
    // Enabled with NO HBA listening; the HBA comes up only afterwards. `echo 1 >
    // enable` makes the kernel multicast an ADDED, and the listener that receives
    // it PROBES the tombstone of the backstore it is told about before queueing
    // the event (tcmu.cpp's on_added calls devlock_free) -- and a probe holds
    // LOCK_EX for as long as it runs (utils.cpp:148). A probe of the same path
    // from this vcpu that overlaps it reads "a live server holds it", which is
    // the flake this case had in both of its shapes: list_orphans() skips a held
    // entry silently, and destroy_orphan() refuses one with EBUSY. Both are the
    // right answers about a held tombstone -- the holder was just the listener's
    // own probe, so the case must not be racing it at all.
    //
    // Bringing the HBA up after the enable removes the race rather than narrowing
    // it. The only probe the listener then owes this backstore is initial_scan's,
    // and vcpu_main() calls publish(1) as soon as initial_scan() returns, which
    // signals the started_sem start() waits on -- so new_tcmu_hba() cannot
    // return before that probe is over. The one event the backstore can still
    // raise is the REMOVED from destroy_orphan()'s own enable=0, and on_removed()
    // never touches the flock.
    delete sys;
    sys = nullptr;
    ASSERT_EQ(0, plant_backstore(BS_PATH, TEST_IDENTITY, IMG_SIZE));
    ASSERT_EQ(0, cfs_write(std::string(BS_PATH) + "/enable", "1"));
    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS);
    ASSERT_NE(nullptr, sys);
    std::string lp = lock_path(TEST_IDENTITY);
    // A DIRECTORY where the tombstone should be. devlock_free() opens O_RDONLY
    // and flocks, and both succeed on a directory fd, so the scan still reads
    // this entry as free and reports it; it is devlock_acquire()'s O_CREAT|O_RDWR
    // that refuses one. That asymmetry is what makes the entry listed but
    // unadoptable, and it is measured below rather than assumed.
    //
    // A regular tombstone is already here -- an earlier case's device planted it,
    // and devlock_release() unlocks and closes but never unlinks -- so it has to
    // go before the directory can take its place.
    ::unlink(lp.c_str());
    ASSERT_EQ(0, ::mkdir(lp.c_str(), 0755));
    DEFER({ ::rmdir(lp.c_str()); ::rmdir(BS_PATH); });

    BlkDevInfo rec;
    bool found = false;
    for (auto& o : sys->list_orphans())
        if (o.identity == TEST_IDENTITY) { rec = o; found = true; }
    ASSERT_TRUE(found) << "a directory tombstone should still read as free to the scan";

    // Adoption cannot proceed, and this is the very open devlock_acquire()
    // performs, so the refusal is measured at the gate adoption actually uses.
    errno = 0;
    int fd = ::open(lp.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    int oe = errno;
    EXPECT_EQ(-1, fd) << "the tombstone path was claimable after all";
    EXPECT_EQ(EISDIR, oe);
    if (fd >= 0) ::close(fd);

    // destroy_orphan() still takes the registration away -- recovering the orphan
    // is the point of the call -- but it does not rmdir a directory in the lock
    // dir on the operator's behalf, so the tombstone half is refused and reported
    // rather than smoothed over into a success.
    errno = 0;
    int rc = sys->destroy_orphan(rec);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EISDIR, e) << "a directory tombstone is operator state: report it, do not delete it";
    EXPECT_NE(0, ::access(BS_PATH, F_OK)) << "the registration should be gone even though the tombstone was refused";
    EXPECT_EQ(0, ::access(lp.c_str(), F_OK)) << "the directory was deleted on the operator's behalf";
    for (auto& o : sys->list_orphans())
        EXPECT_NE(TEST_IDENTITY, o.identity);

    // Once the operator clears it, the identity names nothing at all -- so a
    // retry is not stuck, it just has nothing left to do.
    ASSERT_EQ(0, ::rmdir(lp.c_str()));
    errno = 0;
    rc = sys->destroy_orphan(rec);
    e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(ENOENT, e);
}

TEST_F(TcmuTest, discard_write_zeroes) {
    if (skip_reason) return;
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
    if (skip_reason) return;
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

// The mode-page half of the durability advertisement. A plain write is a bare pwritev
// into the backend's cache, so reporting WCE=0 would tell the initiator every write is
// already durable and it would skip the SYNCHRONIZE CACHE that actually makes it so.
// With FEATURE_FLUSH negotiated (make_info sets it) the caching page must report the
// write cache ENABLED, and DPOFUA must be set because emul_write honours the FUA bit
// (pwritev then fdatasync). Killing mutation: drop `buf[n+2] |= 0x04` (WCE reads back
// 0) or build the device-specific byte without the 0x10 DPOFUA bit.
TEST_F(TcmuTest, mode_sense_reports_write_cache_and_fua) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    uint8_t data[64] = {};
    SgResult r;
    int rc = -1;
    test::run_off_vcpu([&] {
        // MODE SENSE(6), page code 8 (caching), PC=0; we return no block descriptor
        uint8_t cdb[6] = {0x1a, 0x00, 0x08, 0x00, (uint8_t)sizeof(data), 0x00};
        rc = sg_io(sd, cdb, sizeof(cdb), data, sizeof(data), SG_DXFER_FROM_DEV, r);
    });
    ASSERT_EQ(0, rc) << "SG_IO failed, errno=" << rc;
    EXPECT_EQ(0x00, (int)r.status) << "MODE SENSE(6) did not return GOOD";
    // mode parameter header byte 2 is device-specific; DPOFUA is bit 4
    EXPECT_TRUE(data[2] & 0x10) << "DPOFUA not advertised, but emul_write honours FUA";
    // caching page begins at byte 4 (block-descriptor length is 0): [4]=page code,
    // [5]=page length, [6]=flags with WCE at bit 2
    ASSERT_EQ(0x08, (int)data[4]) << "caching page not returned at byte 4";
    EXPECT_TRUE(data[6] & 0x04) << "WCE not set despite FEATURE_FLUSH being negotiated";
}

// The kernel forwards a data CDB to tcmu WITHOUT target_cmd_size_check
// (target_core_device.c sets SCF_SCSI_DATA_CDB then returns TCM_NO_SENSE), so a CDB
// whose transfer count disagrees with the ring's data area reaches emul_read/emul_write.
// data_len is the whole ring area, not the CDB's count; serving it blindly would move
// the wrong number of bytes. Both must refuse the mismatch with INVALID FIELD before
// any backend I/O. This is the end-to-end SG_IO probe the finding called for. Killing
// mutation: drop the cdb_xfer_bytes()!=data_len guard in either handler (the command
// then completes GOOD and, for the write, lands bytes it was never asked to).
TEST_F(TcmuTest, data_cdb_count_must_match_the_transfer) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    // a known pattern at IO_OFF, so "the refused write changed nothing" is checkable
    std::vector<char> before(4096, 0x5a);
    struct iovec iov_b{before.data(), before.size()};
    ASSERT_EQ((ssize_t)before.size(), file->preadv(&iov_b, 1, IO_OFF));

    const uint32_t lba = (uint32_t)(IO_OFF / 512);
    std::vector<char> big(4096, 0x11);   // buffer far larger than the CDB's one block
    SgResult rr, wr;
    int rrc = -1, wrc = -1;
    test::run_off_vcpu([&] {
        // READ(10) and WRITE(10): transfer count 1 (512B) against a 4096B buffer
        uint8_t rcdb[10] = {0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                            (uint8_t)(lba >> 8), (uint8_t)lba, 0, 0, 1, 0};
        rrc = sg_io(sd, rcdb, sizeof(rcdb), big.data(), big.size(), SG_DXFER_FROM_DEV, rr);
        uint8_t wcdb[10] = {0x2a, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                            (uint8_t)(lba >> 8), (uint8_t)lba, 0, 0, 1, 0};
        wrc = sg_io(sd, wcdb, sizeof(wcdb), big.data(), big.size(), SG_DXFER_TO_DEV, wr);
    });
    ASSERT_EQ(0, rrc) << "READ SG_IO failed, errno=" << rrc;
    ASSERT_EQ(0, wrc) << "WRITE SG_IO failed, errno=" << wrc;
    EXPECT_INVALID_FIELD(rr);
    EXPECT_INVALID_FIELD(wr);

    // the refused write must not have touched the backend
    std::vector<char> after(4096, 0);
    struct iovec iov_a{after.data(), after.size()};
    ASSERT_EQ((ssize_t)after.size(), file->preadv(&iov_a, 1, IO_OFF));
    EXPECT_EQ(0, memcmp(before.data(), after.data(), before.size()))
        << "a WRITE whose CDB count mismatched the data still modified the backend";
}

// WRITE SAME(16) cdb[1] flag bits, per the kernel's sbc_setup_write_same (which it
// applies to both the 10- and 16-byte forms): 0x01 NDOB, 0x02 LBDATA, 0x04 PBDATA,
// 0x08 UNMAP, 0x10 ANCHOR. The old code read 0x10 as NDOB -- it is ANCHOR -- and
// accepted LBDATA/PBDATA. ANCHOR must be refused (ANC_SUP is 0) and LBDATA/PBDATA
// refused (no logical/physical block data), while true NDOB (0x01) still zero-fills.
// Killing mutation: read ndob from 0x10 (ANCHOR then zero-fills and returns GOOD
// instead of INVALID FIELD) or drop either the 0x06 or the 0x10 rejection.
TEST_F(TcmuTest, write_same_rejects_anchor_and_block_data_bits) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    // a non-zero pattern at IO_OFF, so "NDOB zeroed it" is not "it was already zero"
    const uint32_t nblocks = 8;   // 4 KiB
    std::vector<char> pat(nblocks * 512);
    for (size_t i = 0; i < pat.size(); i++) pat[i] = (char)(i * 13 + 7);
    struct iovec piov{pat.data(), pat.size()};
    ASSERT_EQ((ssize_t)pat.size(), file->pwritev(&piov, 1, IO_OFF));

    const uint64_t lba = IO_OFF / 512;
    SgResult anchor, lbdata, ndob;
    int arc = -1, lrc = -1, nrc = -1;
    test::run_off_vcpu([&] {
        auto ws16 = [&](uint8_t flags, uint8_t* cdb) {
            memset(cdb, 0, 16);
            cdb[0] = 0x93;   // WRITE SAME(16)
            cdb[1] = flags;
            for (int i = 0; i < 8; i++) cdb[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
            cdb[10] = (uint8_t)(nblocks >> 24); cdb[11] = (uint8_t)(nblocks >> 16);
            cdb[12] = (uint8_t)(nblocks >> 8);  cdb[13] = (uint8_t)nblocks;
        };
        uint8_t c[16];
        ws16(0x10, c); arc = sg_io(sd, c, 16, nullptr, 0, SG_DXFER_NONE, anchor);   // ANCHOR
        ws16(0x02, c); lrc = sg_io(sd, c, 16, nullptr, 0, SG_DXFER_NONE, lbdata);   // LBDATA
        ws16(0x01, c); nrc = sg_io(sd, c, 16, nullptr, 0, SG_DXFER_NONE, ndob);     // NDOB
    });
    ASSERT_EQ(0, arc); ASSERT_EQ(0, lrc); ASSERT_EQ(0, nrc);
    EXPECT_INVALID_FIELD(anchor);   // ANCHOR: ANC_SUP is 0
    EXPECT_INVALID_FIELD(lbdata);   // LBDATA: no logical block data
    // NDOB (0x01) is the real no-data-out-buffer zero write: it must succeed and zero
    // the range, not be mistaken for the ANCHOR bit the old code keyed on.
    EXPECT_EQ(0x00, (int)ndob.status) << "NDOB WRITE SAME(16) did not return GOOD";
    std::vector<char> bbuf(pat.size(), 0x5a);
    struct iovec iov{bbuf.data(), bbuf.size()};
    ASSERT_EQ((ssize_t)bbuf.size(), file->preadv(&iov, 1, IO_OFF));
    for (size_t i = 0; i < bbuf.size(); i++)
        ASSERT_EQ(0, bbuf[i]) << "NDOB did not zero the backend at byte " << i;
}

// WSNZ=1 in VPD 0xB0 byte 4 says a WRITE SAME count of zero is invalid, not "zero from
// here to the end of the device". The old page reported WSNZ=0 while emul_write_same
// returned GOOD for count 0 as a no-op before even the bounds check -- a spec-following
// initiator would read that as "you just zeroed the rest of the disk". Both halves must
// match the kernel's target, which hardcodes WSNZ=1 and rejects count 0 for both forms.
// Killing mutation: leave buf[4]=0 in build_vpd_b0, or restore the `len==0 -> GOOD`
// no-op in emul_write_same.
TEST_F(TcmuTest, write_same_count_zero_is_invalid_and_vpd_b0_reports_wsnz) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    uint8_t b0[64] = {};
    SgResult vpd_r, ws_r;
    int vrc = -1, wrc = -1;
    test::run_off_vcpu([&] {
        // INQUIRY EVPD page 0xB0 (block limits): byte 4 bit 0 is WSNZ
        uint8_t icdb[6] = {0x12, 0x01, 0xb0, 0x00, (uint8_t)sizeof(b0), 0x00};
        vrc = sg_io(sd, icdb, sizeof(icdb), b0, sizeof(b0), SG_DXFER_FROM_DEV, vpd_r);
        // WRITE SAME(16) with a count of zero (cdb[10..13] left 0)
        const uint64_t lba = IO_OFF / 512;
        uint8_t c[16] = {};
        c[0] = 0x93;
        for (int i = 0; i < 8; i++) c[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
        wrc = sg_io(sd, c, 16, nullptr, 0, SG_DXFER_NONE, ws_r);
    });
    ASSERT_EQ(0, vrc) << "INQUIRY SG_IO failed, errno=" << vrc;
    EXPECT_EQ(0x00, (int)vpd_r.status) << "INQUIRY VPD 0xB0 did not return GOOD";
    EXPECT_EQ(0xb0, (int)b0[1]) << "not the block-limits VPD page";
    EXPECT_TRUE(b0[4] & 0x01) << "WSNZ not set in VPD 0xB0 byte 4";
    ASSERT_EQ(0, wrc) << "WRITE SAME SG_IO failed, errno=" << wrc;
    EXPECT_INVALID_FIELD(ws_r);   // count 0 is invalid under WSNZ=1
}

// LBPRZ, advertised in RC16 byte 14, promises reads after an unmap return zeros. When
// the backend cannot punch a hole -- trim() answers EOPNOTSUPP and zero_range() with it,
// so zero_fill() falls back to writing zeroes -- emul_unmap must still zero the range
// instead of swallowing the error and reporting GOOD over the old bytes. NoTrimFile
// makes that fallback the only path. Killing mutation: restore
// `trim() < 0 && errno != EOPNOTSUPP && errno != ENOSYS` (the unsupported trim is
// swallowed, the range keeps its pattern, and the backend readback below is non-zero).
TEST_F(TcmuTest, unmap_zeroes_when_the_backend_cannot_trim) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    NoTrimFile backend(file);   // trim/zero_range unsupported; pwritev reaches `file`
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&backend));   // ownership=false: `file` stays the fixture's
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> wbuf(IO_LEN);
    for (size_t i = 0; i < IO_LEN; i++) wbuf[i] = (char)(i * 13 + 7);
    int rc = -1;
    test::run_off_vcpu([&] {
        int fd = open_sd(sd, O_RDWR);
        if (fd < 0) { rc = errno ? errno : EIO; return; }
        DEFER(::close(fd));
        if (::pwrite(fd, wbuf.data(), IO_LEN, IO_OFF) != (ssize_t)IO_LEN) { rc = errno ? errno : EIO; return; }
        if (::fsync(fd) < 0) { rc = errno ? errno : EIO; return; }
        uint64_t range[2] = {IO_OFF, IO_LEN};
        if (::ioctl(fd, BLKDISCARD, range) < 0) { rc = errno ? errno : EIO; return; }
        rc = 0;
    });
    int e = rc;
    ASSERT_EQ(0, e) << "write/BLKDISCARD with a no-trim backend failed (errno=" << e << ")";

    // authoritative check on the backend image: the trim was unsupported, so the fix
    // zero-filled the range rather than leave the pattern and report GOOD.
    std::vector<char> bbuf(IO_LEN, 0x5a);
    struct iovec iov{bbuf.data(), IO_LEN};
    ASSERT_EQ((ssize_t)IO_LEN, file->preadv(&iov, 1, IO_OFF));
    for (size_t i = 0; i < IO_LEN; i++)
        ASSERT_EQ(0, bbuf[i]) << "backend range not zeroed at byte " << i;
}

TEST_F(TcmuTest, timeout_knobs) {
    if (skip_reason) return;
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
    if (skip_reason) return;
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
    if (skip_reason) return;
    // TestPool is built with the same request mask this suite's main() inited
    // the caller's vcpu with, which is what makes check_pool_engines pass: it
    // derives its requirement from the caller's own vcpu and then reads back
    // each pool vcpu's engine name to compare against it, so the match it
    // verifies still has to be arranged by construction -- nothing answers with
    // the mask a vcpu was inited with. See TEST_EVENT_ENGINE.
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
    if (skip_reason) return;
    // ONE pool vcpu, so the placement set is {pool} or {pool, caller}, and
    // only the teardown path can add the caller. Engines are the shared pair
    // main() also inits with (see TEST_EVENT_ENGINE), and the pool is declared
    // before cfg/dev so it outlives the device -- see
    // pool_placement_pump_off_the_caller_vcpu (CONTRACT 1)
    photon::WorkPool pool(1, test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE);
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
    // Safety net, release FIRST: a writer parked in a gated backend IO only leaves
    // it once the gate opens, and stop() waits for the writer to leave its IO. That
    // wait is bounded, so the wrong order no longer hangs this case -- it abandons
    // five writers instead. On the normal path these are each object's SECOND stop:
    // the case body's own stop loop ran first, and stop() says true of a writer that
    // is not running, so the assertion that makes an abandoned writer red is that
    // loop's -- this one only bites when an ASSERT_ ended the case before it. And
    // EXPECT, not ASSERT: an ASSERT_ fails the case too, but its return leaves this
    // lambda, so the four stops after the first are left to the writers' own
    // destructors, where an abandoned one has nobody to tell.
    DEFER({
        rec.release_gate(4096);
        for (auto& b : bw)
            EXPECT_TRUE(b.stop());
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

    // A coroutine, not a statement beside the call: placed after the detach it
    // never runs at all, because detach(true) cannot return while the gate is
    // shut -- serve_stop needs a dispatch slot to flush with and in_flight at
    // zero, and the gate-parked IOs hold both. Placed before it, the pump is
    // still alive and eats the backlog before serve_stop joins it, so the flush
    // gets nothing. The length is free but the sleep is not: sleeping is what
    // yields this vcpu, so the teardown thunk queued behind this coroutine
    // reaches its park first at any value, zero included, and joins the pump
    // before the gate opens. What no length can reach is WHICH vcpu the flush
    // runs on -- run_serve_stop moves the teardown to the vcpu the pump
    // recorded, a choice that reads no gate and no clock. Measured at zero and
    // as written: deleting that move is caught either way, and the case is
    // green either way.
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
        EXPECT_TRUE(b.stop());
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
    if (skip_reason) return;
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
    if (skip_reason) return;
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

// check_pool_engines' integration half: the helper is unit-tested on its own, this
// proves the transport actually asks. A pool whose vcpus cannot host the serving
// coroutines is a configuration error, so start() refuses it rather than serve
// pathologically.
//
// The refusal is NOT up front here: the guard sits in TcmuServer::start(), which
// start() reaches only after the backstore exists and is enabled and its uio node
// has been found, and only before attach_lun(). So it arrives as a rollback that
// has to destroy a live backstore, and the residue comparison is what makes that
// observable. It is taken inside the case because the fixture's TearDown runs
// force_cleanup() afterwards and would remove a leak before any outside check
// could see it.
TEST_F(TcmuTest, pool_without_an_event_engine_is_refused) {
    if (skip_reason) return;
    const std::string residue_before = residue();
    photon::WorkPool bad(2);      // ev_engine defaults to 0: no engine at all
    test::RecordingFile rec(file);

    auto cfg = make_cfg(/*loopback=*/true);
    cfg.pool = &bad;
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(&rec));
    EXPECT_EQ(EINVAL, errno);
    EXPECT_EQ(0u, rec.vcpu_count());
    EXPECT_EQ(residue_before, residue());
}

// High-concurrency stress on the LUN's /dev/sdX: many O_DIRECT threads (so
// every IO goes through the ring, not the page cache), mixed block sizes, and
// self-describing blocks (harness.h) that a reader can validate without
// knowing who wrote them last. DISJOINT proves no cross-thread misrouting
// (each block must carry the reader's own tid+seq); SHARED proves the pump
// survives maximal contention on one region -- a torn or lost write breaks a
// block's invariants even when the owner is legitimately someone else.
TEST_F(TcmuTest, concurrent_stress) {
    if (skip_reason) return;
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
    if (skip_reason) return;
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
    DEFER(EXPECT_TRUE(w.stop()));

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
    if (skip_reason) return;
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
    std::string want_dev = std::string("tcm-user/" SUITE_HBANUM "/") + TEST_IDENTITY +
                           "/" + DEV_CONFIG_PREFIX + TEST_IDENTITY;
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
    if (skip_reason) return;
    delete sys;   // SetUp built this one with netlink_reply off; replace it
    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS,
                       /*netlink_reply=*/true);
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
    if (skip_reason) return;
    const char HBA0[] = SUITE_CORE SUITE_SUBTYPE;
    const char HBA1[] = SUITE_CORE SUITE_SUBTYPE2;
    const char BS1[]  = SUITE_CORE SUITE_SUBTYPE2 "/photon-twoinst";
    int hrc = ::mkdir(HBA0, 0755);
    ASSERT_TRUE(hrc == 0 || errno == EEXIST) << "cannot create the HBA dir: " << strerror(errno);

    // the opt-out is a per-backstore attrib on v4.15+; probe it on a throwaway
    // (never enabled) backstore. Below v4.15 a plain instance beside a reply-mode
    // one needs defensive_reply=true instead, so this case cannot run here.
    std::string probe = std::string(HBA0) + "/photon-probe-attrib";
    ASSERT_EQ(0, ::mkdir(probe.c_str(), 0755));
    DEFER(::rmdir(probe.c_str()));   // never enabled, so the rmdir always works
    if (::access((probe + "/attrib/nl_reply_supported").c_str(), F_OK) != 0)
        return report_skip("no nl_reply_supported (kernel v4.15+)");

    delete sys;   // SetUp built this one with netlink_reply off; replace it
    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS,
                       /*netlink_reply=*/true);
    ASSERT_NE(nullptr, sys);

    DEFER(::rmdir(HBA1));            // registered first, so it runs last
    auto plain = new_tcmu_hba(SUITE_SUBTYPE2, DEV_CONFIG_PREFIX, SUITE_LOCKS);
    ASSERT_NE(nullptr, plain);
    DEFER(delete plain);             // before the fixture's TearDown restores the flag

    auto cfg = make_cfg(/*loopback=*/false);   // no LUN: keep to the configfs path
    cfg.info.identity = "photon-twoinst";
    auto dev = plain->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));   // -ENOSYS from the reply-mode HBA without the opt-out
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
    if (skip_reason) return;
    delete sys;   // SetUp built this one with netlink_reply off; replace it
    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS,
                       /*netlink_reply=*/true);
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

// Stopping the HBA must refuse every event still queued for wait_for_event, not only
// the notifications drain_replies finds resident in the socket. Each queued event armed
// the kernel's tcmu_wait_genl_cmd_reply -- an unbounded wait_for_completion that the
// SET_FEATURES restore cannot wake -- so dropping it hangs the operator's `echo 1 >
// enable` until reset_netlink. The fix swaps the queue out and answers each with
// -ENOSYS. Killing mutation: delete the queue drain in vcpu_main -- the ADDED below is
// already queued (the sleep lets the listener dequeue it from the socket), so nothing
// answers it and op.join() never returns, which is exactly the hang this fixes.
TEST_F(TcmuTest, stopping_the_hba_refuses_an_unconsumed_added) {
    if (skip_reason) return;
    delete sys;   // SetUp built this one with netlink_reply off; the drain needs it on
    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS,
                       /*netlink_reply=*/true);
    ASSERT_NE(nullptr, sys);

    // The operator enables a backstore by raw configfs; the ADDED is queued for
    // wait_for_event and the kernel blocks the enable write on ADDED_DEVICE_DONE. We
    // deliberately never consume the event, so it is still in the queue at stop().
    Operator op;
    op.run([] {
        if (::mkdir(PASSIVE_BS_PATH, 0755) != 0) return -errno;
        if (int rc = cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_config", PASSIVE_IMG)) return rc;
        if (int rc = cfs_write(std::string(PASSIVE_BS_PATH) + "/attrib/dev_size", std::to_string(PASSIVE_SIZE))) return rc;
        return cfs_write(std::string(PASSIVE_BS_PATH) + "/enable", "1");   // blocks until answered
    });
    // let the listener dequeue the notification and queue() the event, so it is the
    // queue drain -- not drain_replies' socket pass -- that has to answer it
    photon::thread_usleep(1000 * 1000);

    delete sys;   // -> stop() -> vcpu_main drains the queue with -ENOSYS
    sys = nullptr;
    // the enable failed cleanly (refused) rather than hung; -ENOSYS is the status the
    // drain sent. TearDown's force_cleanup removes the disabled backstore dir.
    EXPECT_EQ(-ENOSYS, op.join());
}

// netlink_reply=false: no synchronous feedback. Every operator write returns at
// once and the event loop learns about the device asynchronously.
TEST_F(TcmuTest, passive_daemon_async) {
    if (skip_reason) return;
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
    if (skip_reason) return;
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

    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS);
    ASSERT_NE(nullptr, sys);
    TcmuHBA::Event ev;
    ASSERT_EQ(0, sys->wait_for_event(&ev, TMO));
    ASSERT_EQ(TcmuHBA::EventKind::ADDED, ev.kind);
    EXPECT_TRUE(ev.synthesized) << "the startup scan did not report the device";
    EXPECT_EQ(0u, ev.dev_id);
    EXPECT_EQ(PASSIVE_SIZE, ev.size);
    EXPECT_STREQ(PASSIVE_BS, ev.bs_name);
    // an operator's backstore is not one of ours: list_orphans() reports only
    // registrations whose dev_config carries this HBA's ownership prefix
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
    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS);
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

// BlkConfig::spin_us has three modes that select what the pump does when idle:
//   0          — block immediately (zero idle CPU)
//   UINT32_MAX — spin forever (lowest latency, burns a vCPU)
//   other      — spin for that many µs after last work, then block
// All three must produce correct I/O; a broken predicate (wrong comparison,
// dropped UINT32_MAX case) will hang or fail in at least one mode.
TEST_F(TcmuTest, spin_us_modes) {
    if (skip_reason) return;
    const uint32_t modes[] = {0, UINT32_MAX, 50000};
    for (uint32_t sp : modes) {
        auto cfg = make_cfg(/*loopback=*/true);
        cfg.spin_us = sp;
        auto dev = sys->new_device(cfg);
        ASSERT_NE(nullptr, dev) << "spin_us=" << sp;
        DEFER(delete dev);
        ASSERT_EQ(0, dev->start(file)) << "spin_us=" << sp;
        DEFER(dev->shutdown());

        std::string sd = wait_photon_sd(IMG_SIZE / 512);
        ASSERT_FALSE(sd.empty()) << "spin_us=" << sp;

        auto wbuf = pattern((uint8_t)(sp & 0xff));
        EXPECT_EQ(0, device_io(sd, wbuf, /*verify_backend=*/true))
            << "I/O failed under spin_us=" << sp;

        dev->shutdown();
    }
}

// ---------------------------------------------------------------------------
// The 2026-10-10 tcmu review round and the CDB sweep it prompted.
//
// Every case below is written to DIFFER at the commit that round reviewed: the
// assertion it makes is one the code failed then, so none of them can pass by
// describing the old behaviour. The single exception is called out in its own
// comment rather than left to be mistaken for a witness.
// ---------------------------------------------------------------------------

// READ/WRITE(12) carry their transfer count as a be32 at cdb[6..9]. The code read
// it at cdb[7] -- the offset the (10) form's be16 lives at -- which shifts the count
// left eight bits and folds in cdb[10], the group number: a one-block CDB with a
// clear group byte parsed as 256 blocks, disagreed with the 512-byte data area, and
// was refused as INVALID FIELD. So every legitimate (12) failed. The kernel's own
// transport_get_sectors_12() reads be32(&cdb[6]); (6), (10) and (16) already agreed
// with it here and only (12) did not.
// Killing mutation: cdb_be32(cdb + 7) in cdb_xfer_bytes.
TEST_F(TcmuTest, read_write_12_take_their_transfer_count_from_cdb_byte_6) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    // a known pattern at IO_OFF, so the read can be checked against the backend
    std::vector<char> want(512);
    for (size_t i = 0; i < want.size(); i++) want[i] = (char)(i * 31 + 5);
    struct iovec wiov{want.data(), want.size()};
    ASSERT_EQ((ssize_t)want.size(), file->pwritev(&wiov, 1, IO_OFF));

    const uint32_t lba = (uint32_t)(IO_OFF / 512);
    std::vector<char> rbuf(512, 0x5a), wbuf(512, 0x22);
    SgResult rr, wr;
    int rrc = -1, wrc = -1;
    test::run_off_vcpu([&] {
        // 12-byte forms: LBA be32 at [2..5], transfer count be32 at [6..9],
        // group number at [10], control at [11]. Count 1 = 512 bytes, matching the
        // buffer, which is the shape the old offset could not parse.
        uint8_t rcdb[12] = {0xa8, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                            (uint8_t)(lba >> 8), (uint8_t)lba, 0, 0, 0, 1, 0, 0};
        rrc = sg_io(sd, rcdb, sizeof(rcdb), rbuf.data(), rbuf.size(), SG_DXFER_FROM_DEV, rr);
        uint8_t wcdb[12] = {0xaa, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                            (uint8_t)(lba >> 8), (uint8_t)lba, 0, 0, 0, 1, 0, 0};
        wrc = sg_io(sd, wcdb, sizeof(wcdb), wbuf.data(), wbuf.size(), SG_DXFER_TO_DEV, wr);
    });
    ASSERT_EQ(0, rrc) << "READ(12) SG_IO failed, errno=" << rrc;
    ASSERT_EQ(0, wrc) << "WRITE(12) SG_IO failed, errno=" << wrc;
    EXPECT_EQ(0x00, (int)rr.status) << "READ(12) with a one-block count was refused, asc=" << rr.asc();
    EXPECT_EQ(0x00, (int)wr.status) << "WRITE(12) with a one-block count was refused, asc=" << wr.asc();
    EXPECT_EQ(0, memcmp(want.data(), rbuf.data(), want.size())) << "READ(12) returned the wrong bytes";

    // the WRITE(12) landed, and a count that still disagrees with the data area is
    // refused -- the guard this case exists beside must not have been removed to make
    // the one-block case pass
    std::vector<char> after(512, 0);
    struct iovec aiov{after.data(), after.size()};
    ASSERT_EQ((ssize_t)after.size(), file->preadv(&aiov, 1, IO_OFF));
    EXPECT_EQ(0, memcmp(wbuf.data(), after.data(), after.size())) << "WRITE(12) did not reach the backend";

    std::vector<char> big(4096, 0x33);
    SgResult mism;
    int mrc = -1;
    test::run_off_vcpu([&] {
        uint8_t cdb[12] = {0xaa, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                           (uint8_t)(lba >> 8), (uint8_t)lba, 0, 0, 0, 1, 0, 0};
        mrc = sg_io(sd, cdb, sizeof(cdb), big.data(), big.size(), SG_DXFER_TO_DEV, mism);
    });
    ASSERT_EQ(0, mrc);
    EXPECT_INVALID_FIELD(mism);
}

// A device that does not offer FEATURE_FLUSH reports WCE=0 in its caching page, and
// Linux's sd_set_flush_flag() then clears BLK_FEAT_WRITE_CACHE *and* BLK_FEAT_FUA
// together -- DPOFUA is parsed and discarded -- so such an initiator sends neither
// SYNCHRONIZE CACHE nor FUA and has no way to ask for durability. Every GOOD it gets
// is therefore this device's word that the bytes are already on stable storage, which
// a bare pwritev into a buffered file does not make true. write_through is that word
// being kept. The control half matters as much as the experiment: without it, a build
// that synced unconditionally would pass the first half and silently cost a syscall
// per write for every device that DID offer FLUSH.
// Killing mutations: drop `|| write_through` (first half reddens); sync
// unconditionally (the control reddens).
TEST_F(TcmuTest, an_ordinary_write_is_durable_when_flush_is_not_configured) {
    if (skip_reason) return;
    BlkDevInfo no_flush = make_info();
    no_flush.features = 0;
    TcmuHBA::Config cfg(no_flush);
    cfg.loopback_lun = true;
    cfg.loopback_wwn = TEST_WWN;
    cfg.timeout = 30;
    test::BackendProbe probe(file);   // does not own `file`
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> wbuf(512, 0x44);
    SgResult w1;
    int rc = -1;
    test::run_off_vcpu([&] {
        uint8_t cdb[10] = {0x2a, 0, 0, 0, (uint8_t)(IO_OFF / 512 / 256), (uint8_t)(IO_OFF / 512),
                           0, 0, 1, 0};
        rc = sg_io(sd, cdb, sizeof(cdb), wbuf.data(), wbuf.size(), SG_DXFER_TO_DEV, w1);
    });
    ASSERT_EQ(0, rc) << "WRITE(10) SG_IO failed, errno=" << rc;
    EXPECT_EQ(0x00, (int)w1.status) << "ordinary write was refused, asc=" << w1.asc();
    EXPECT_EQ(1, probe.writes.load()) << "the write never reached the backend";
    EXPECT_EQ(1, probe.datasyncs.load())
        << "an ordinary WRITE was acked without being made durable, on a device that "
           "advertised no write cache and gave the initiator no FLUSH to ask with";
    EXPECT_EQ(0, probe.syncs.load()) << "fdatasync is the primitive, not fsync";
}

TEST_F(TcmuTest, an_ordinary_write_stays_cached_when_flush_is_configured) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);   // make_info() sets FEATURE_FLUSH
    test::BackendProbe probe(file);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> wbuf(512, 0x55);
    SgResult w1;
    int rc = -1;
    test::run_off_vcpu([&] {
        uint8_t cdb[10] = {0x2a, 0, 0, 0, (uint8_t)(IO_OFF / 512 / 256), (uint8_t)(IO_OFF / 512),
                           0, 0, 1, 0};
        rc = sg_io(sd, cdb, sizeof(cdb), wbuf.data(), wbuf.size(), SG_DXFER_TO_DEV, w1);
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0x00, (int)w1.status);
    EXPECT_EQ(1, probe.writes.load());
    EXPECT_EQ(0, probe.datasyncs.load())
        << "a device that offered FLUSH synced per write; the initiator can batch";
}

// The caching page's length byte said 0x0a, describing a 12-byte page; the SPC
// caching page is 20 bytes and the kernel's own spc_emulate_caching_page writes 0x12
// and returns 20. Linux's sd reads only the flags byte, so the wrong length was
// invisible to it -- but MODE SENSE for "all pages" uses it to find the NEXT page,
// and MODE SELECT compares exactly that many bytes, so both would have been wrong
// against a spec-following initiator. Asserted together with WCE because both live in
// the same page and the features=0 configuration had no case at all.
// Killing mutation: p[1] = 0x0a in build_caching_page.
TEST_F(TcmuTest, mode_sense_reports_its_real_caching_page_length_and_no_write_cache) {
    if (skip_reason) return;
    BlkDevInfo no_flush = make_info();
    no_flush.features = 0;
    TcmuHBA::Config cfg(no_flush);
    cfg.loopback_lun = true;
    cfg.loopback_wwn = TEST_WWN;
    cfg.timeout = 30;
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> data(192, 0x5a);
    SgResult ms;
    int rc = -1;
    test::run_off_vcpu([&] {
        // MODE SENSE(10), page 0x08, allocation length 192
        uint8_t cdb[10] = {0x5a, 0, 0x08, 0, 0, 0, 0, 0, 192, 0};
        rc = sg_io(sd, cdb, sizeof(cdb), data.data(), data.size(), SG_DXFER_FROM_DEV, ms);
    });
    ASSERT_EQ(0, rc) << "MODE SENSE(10) SG_IO failed, errno=" << rc;
    EXPECT_EQ(0x00, (int)ms.status);
    // 8-byte mode parameter header, then the page: [8]=code, [9]=length, [10]=flags
    EXPECT_EQ(0x08, (int)(uint8_t)data[8]) << "not the caching page";
    EXPECT_EQ(0x12, (int)(uint8_t)data[9]) << "caching page length is not the SPC one";
    EXPECT_EQ(0, (int)(uint8_t)data[10] & 0x04) << "WCE is set on a device with no FEATURE_FLUSH";
    // DPOFUA is reported whatever the cache policy: the handler does honour FUA. It is
    // inert for a Linux initiator here, which is exactly why write_through exists.
    EXPECT_NE(0, (int)(uint8_t)data[3] & 0x10) << "DPOFUA dropped";
}

// INQUIRY left byte 7 at zero, so CmdQue was clear: scsi_scan.c gates
// sdev->tagged_supported and simple_tags on `inq_result[7] & 2`, and this device does
// serve concurrently -- one coroutine per ring entry, bounded by queue_depth. The
// practical effect is narrow (tcm_loop is not one of the legacy LLDs that read
// simple_tags, so queue depth is not capped; Linux reports queue_type "none" and skips
// SCMD_TAGGED), but it is the same class as the LBPRZ advertisement that was already
// fixed: a capability claim contradicting the code. The VERSION byte moved 0x05 -> 0x06
// in the same edit, so both are pinned here; sd still selects READ CAPACITY(16)
// because both read as scsi_level > SCSI_SPC_2 after Linux's +1.
// Killing mutation: buf[7] = 0x00, or buf[2] = 0x05.
TEST_F(TcmuTest, inquiry_reports_tagged_queueing_and_the_version_it_implements) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> data(36, 0x5a);
    SgResult iq;
    int rc = -1;
    test::run_off_vcpu([&] {
        uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
        rc = sg_io(sd, cdb, sizeof(cdb), data.data(), data.size(), SG_DXFER_FROM_DEV, iq);
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0x00, (int)iq.status);
    EXPECT_EQ(0x06, (int)(uint8_t)data[2]) << "VERSION is not SPC-4";
    EXPECT_NE(0, (int)(uint8_t)data[7] & 0x02) << "CmdQue is clear on a device that serves concurrently";
}

// An UNMAP descriptor past the end of the device used to be skipped with `continue`,
// so the command returned GOOD having unmapped nothing. That is worse than a wrong
// status here: LBPRZ is advertised, which promises reads after an unmap return zeros,
// so an initiator is entitled to trust a GOOD. The kernel fails the whole command with
// TCM_ADDRESS_OUT_OF_RANGE and unwinds nothing, so a valid descriptor before the bad
// one stays unmapped -- asserted both ways, because "fails" alone would also pass on a
// build that refused the entire list up front.
// Killing mutation: restore `if (out_of_bounds(off, len)) continue;`.
TEST_F(TcmuTest, an_out_of_range_unmap_descriptor_fails_the_whole_command) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    // 8-byte parameter list header + two 16-byte descriptors: the first in range at
    // IO_OFF, the second past the end of a 64 MiB device.
    const uint64_t last_lba = IMG_SIZE / 512 - 1;
    std::vector<uint8_t> p(8 + 32, 0);
    p[0] = 0; p[1] = (uint8_t)((8 + 32) - 2);   // UNMAP data length
    p[2] = 0; p[3] = 32;                        // block descriptor data length
    auto desc = [&](int i, uint64_t lba, uint32_t n) {
        uint8_t* d = p.data() + 8 + i * 16;
        for (int b = 0; b < 8; b++) d[b] = (uint8_t)(lba >> (56 - 8 * b));
        d[8] = (uint8_t)(n >> 24); d[9] = (uint8_t)(n >> 16);
        d[10] = (uint8_t)(n >> 8); d[11] = (uint8_t)n;
    };
    desc(0, IO_OFF / 512, 1);
    desc(1, last_lba + 8, 1);
    // tell the kernel the list is 8 + 16 bytes so it hands over both descriptors
    p[3] = 32;

    SgResult um;
    int rc = -1;
    test::run_off_vcpu([&] {
        uint8_t cdb[10] = {0x42, 0, 0, 0, 0, 0, 0, 0, (uint8_t)p.size(), 0};
        rc = sg_io(sd, cdb, sizeof(cdb), p.data(), p.size(), SG_DXFER_TO_DEV, um);
    });
    ASSERT_EQ(0, rc) << "UNMAP SG_IO failed, errno=" << rc;
    EXPECT_EQ(0x02, (int)um.status) << "expected CHECK CONDITION, got GOOD over an unmapped nothing";
    ASSERT_GE(um.sb_len, 14u);
    EXPECT_EQ(0x05, um.key()) << "sense key is not ILLEGAL_REQUEST";
    EXPECT_EQ(0x21, um.asc()) << "ASC is not LBA OUT OF RANGE";
}

// VERIFY was not in the opcode switch at all, so it fell through to unknown_op and
// answered INVALID COMMAND OPERATION CODE for a command sbc_parse_cdb accepts and
// routes to sbc_emulate_noop. It carries no data phase, so what there is to check is
// the range: the kernel runs check_lba on it, and so does this.
// Killing mutation: delete the OPC_VERIFY cases from the switch.
TEST_F(TcmuTest, verify_is_accepted_in_range_and_refused_out_of_it) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    const uint32_t lba = (uint32_t)(IO_OFF / 512);
    const uint32_t past = (uint32_t)(IMG_SIZE / 512);
    SgResult ok_r, bad_r, ok16, bad16;
    int a = -1, b = -1, c = -1, d = -1;
    test::run_off_vcpu([&] {
        uint8_t v10[10] = {0x2f, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                           (uint8_t)(lba >> 8), (uint8_t)lba, 0, 0, 1, 0};
        a = sg_io(sd, v10, sizeof(v10), nullptr, 0, SG_DXFER_NONE, ok_r);
        uint8_t v10b[10] = {0x2f, 0, (uint8_t)(past >> 24), (uint8_t)(past >> 16),
                            (uint8_t)(past >> 8), (uint8_t)past, 0, 0, 1, 0};
        b = sg_io(sd, v10b, sizeof(v10b), nullptr, 0, SG_DXFER_NONE, bad_r);
        // VERIFY(16): LBA be64 at [2..9], verification length be32 at [10..13],
        // group at [14], control at [15]. Length 1, LBA 0.
        uint8_t v16[16] = {0x8f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
        c = sg_io(sd, v16, sizeof(v16), nullptr, 0, SG_DXFER_NONE, ok16);
        uint8_t v16b[16] = {0x8f, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf0,
                            0, 0, 0, 1, 0, 0};
        d = sg_io(sd, v16b, sizeof(v16b), nullptr, 0, SG_DXFER_NONE, bad16);
    });
    ASSERT_EQ(0, a); ASSERT_EQ(0, b); ASSERT_EQ(0, c); ASSERT_EQ(0, d);
    EXPECT_EQ(0x00, (int)ok_r.status) << "VERIFY(10) in range was refused, asc=" << ok_r.asc();
    EXPECT_EQ(0x00, (int)ok16.status) << "VERIFY(16) in range was refused, asc=" << ok16.asc();
    EXPECT_EQ(0x02, (int)bad_r.status);
    EXPECT_EQ(0x21, bad_r.asc()) << "VERIFY(10) past the end is not LBA OUT OF RANGE";
    EXPECT_EQ(0x02, (int)bad16.status);
    EXPECT_EQ(0x21, bad16.asc()) << "VERIFY(16) reads its LBA as be64 at cdb[2]";
}

// REQUEST SENSE with the DESC bit set asks for the descriptor format. Everything this
// device builds is fixed format (0x70), and spc_emulate_request_sense refuses rather
// than answer in the wrong format -- handing fixed-format bytes to a descriptor-format
// ask is how an initiator parses garbage.
// Killing mutation: delete the `cdb[1] & 0x01` check.
TEST_F(TcmuTest, request_sense_refuses_the_descriptor_format) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> fixed(18, 0x5a), desc(18, 0x5a);
    SgResult f, d;
    int a = -1, b = -1;
    test::run_off_vcpu([&] {
        uint8_t c1[6] = {0x03, 0x00, 0, 0, 18, 0};
        a = sg_io(sd, c1, sizeof(c1), fixed.data(), fixed.size(), SG_DXFER_FROM_DEV, f);
        uint8_t c2[6] = {0x03, 0x01, 0, 0, 18, 0};
        b = sg_io(sd, c2, sizeof(c2), desc.data(), desc.size(), SG_DXFER_FROM_DEV, d);
    });
    ASSERT_EQ(0, a); ASSERT_EQ(0, b);
    EXPECT_EQ(0x00, (int)f.status);
    EXPECT_EQ(0x70, (int)(uint8_t)fixed[0]) << "fixed-format response code";
    EXPECT_EQ(0x02, (int)d.status) << "a DESC REQUEST SENSE was answered in the wrong format";
    EXPECT_EQ(0x24, d.asc()) << "ASC is not INVALID FIELD IN CDB";
}

// READ CAPACITY(10)'s PMI bit and LBA field are obsolete in SBC-3 but not ignorable:
// sbc_emulate_read_capacity rejects a nonzero LBA when PMI is clear, because that asks
// for the capacity at a specific block, which this device does not compute. The 16-byte
// form gets no such check upstream, so it gets none here and this case pins the
// difference rather than assuming both behave alike.
// Killing mutation: delete the PMI check in emul_read_capacity.
TEST_F(TcmuTest, read_capacity_10_refuses_a_nonzero_lba_without_pmi) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    std::vector<char> plain(8, 0), withlba(8, 0);
    SgResult p, l;
    int a = -1, b = -1;
    test::run_off_vcpu([&] {
        uint8_t c1[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        a = sg_io(sd, c1, sizeof(c1), plain.data(), plain.size(), SG_DXFER_FROM_DEV, p);
        uint8_t c2[10] = {0x25, 0, 0, 0, 0, 8, 0, 0, 0, 0};   // LBA 8, PMI clear
        b = sg_io(sd, c2, sizeof(c2), withlba.data(), withlba.size(), SG_DXFER_FROM_DEV, l);
    });
    ASSERT_EQ(0, a); ASSERT_EQ(0, b);
    EXPECT_EQ(0x00, (int)p.status) << "an all-zero READ CAPACITY(10) must still work";
    EXPECT_EQ(0x02, (int)l.status) << "a nonzero LBA with PMI clear was answered";
    EXPECT_EQ(0x24, l.asc());
}

// MODE SELECT answered GOOD to anything at all, without reading the parameter list. A
// device with no changeable parameters still has to say so: spc_emulate_modeselect
// accepts only a list that writes back what MODE SENSE reported, and the alternative is
// an initiator believing it changed the write cache when nothing was even parsed. The
// read-back comparison is against build_caching_page(), the same function MODE SENSE
// uses, so the two cannot disagree about what this device reports.
// Killing mutation: restore `set_status(ent, SAM_GOOD)` as the whole handler.
TEST_F(TcmuTest, mode_select_refuses_a_list_that_is_not_what_mode_sense_reported) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    SgResult empty_r, short_r, nopf_r, badpage_r, wrong_r;
    int a = -1, b = -1, c = -1, d = -1, e = -1;
    // an 8-byte mode parameter header (MODE SELECT(10)) followed by a caching page
    std::vector<uint8_t> good(8 + 20, 0);
    good[8] = 0x08; good[9] = 0x12;
    good[10] = 0x04;                        // WCE, which is what MODE SENSE reports here
    std::vector<uint8_t> wrong = good;
    wrong[10] = 0x00;                       // asking to turn the write cache off
    std::vector<uint8_t> badpage = good;
    badpage[8] = 0x0a;                      // a page this device does not report
    std::vector<uint8_t> tiny(4, 0);
    test::run_off_vcpu([&] {
        uint8_t c0[10] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, 0, 0};   // zero-length list
        a = sg_io(sd, c0, sizeof(c0), nullptr, 0, SG_DXFER_TO_DEV, empty_r);
        uint8_t c1[10] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, 4, 0};
        b = sg_io(sd, c1, sizeof(c1), tiny.data(), tiny.size(), SG_DXFER_TO_DEV, short_r);
        uint8_t c2[10] = {0x55, 0x00, 0, 0, 0, 0, 0, 0, (uint8_t)good.size(), 0};  // PF clear
        c = sg_io(sd, c2, sizeof(c2), good.data(), good.size(), SG_DXFER_TO_DEV, nopf_r);
        uint8_t c3[10] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, (uint8_t)badpage.size(), 0};
        d = sg_io(sd, c3, sizeof(c3), badpage.data(), badpage.size(), SG_DXFER_TO_DEV, badpage_r);
        uint8_t c4[10] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, (uint8_t)wrong.size(), 0};
        e = sg_io(sd, c4, sizeof(c4), wrong.data(), wrong.size(), SG_DXFER_TO_DEV, wrong_r);
    });
    ASSERT_EQ(0, a); ASSERT_EQ(0, b); ASSERT_EQ(0, c); ASSERT_EQ(0, d); ASSERT_EQ(0, e);
    EXPECT_EQ(0x00, (int)empty_r.status) << "an empty parameter list changes nothing and is GOOD";
    EXPECT_EQ(0x02, (int)short_r.status);
    EXPECT_EQ(0x1a, short_r.asc()) << "a truncated list is PARAMETER LIST LENGTH ERROR";
    EXPECT_EQ(0x02, (int)nopf_r.status);
    EXPECT_EQ(0x24, nopf_r.asc()) << "PF clear is INVALID FIELD IN CDB";
    EXPECT_EQ(0x02, (int)badpage_r.status);
    EXPECT_EQ(0x24, badpage_r.asc()) << "a page this device does not report";
    EXPECT_EQ(0x02, (int)wrong_r.status);
    EXPECT_EQ(0x26, wrong_r.asc()) << "changing a reported value is INVALID FIELD IN PARAMETER LIST";

    // the round trip that must still work: write back exactly what MODE SENSE reported
    std::vector<char> sensed(192, 0);
    SgResult ms, acc;
    int f = -1, g = -1;
    test::run_off_vcpu([&] {
        uint8_t cdb[10] = {0x5a, 0, 0x08, 0, 0, 0, 0, 0, 192, 0};
        f = sg_io(sd, cdb, sizeof(cdb), sensed.data(), sensed.size(), SG_DXFER_FROM_DEV, ms);
    });
    ASSERT_EQ(0, f);
    ASSERT_EQ(0x00, (int)ms.status);
    std::vector<uint8_t> back(8 + 20, 0);
    memcpy(back.data() + 8, sensed.data() + 8, 20);
    test::run_off_vcpu([&] {
        uint8_t cdb[10] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, (uint8_t)back.size(), 0};
        g = sg_io(sd, cdb, sizeof(cdb), back.data(), back.size(), SG_DXFER_TO_DEV, acc);
    });
    ASSERT_EQ(0, g);
    EXPECT_EQ(0x00, (int)acc.status) << "writing back what MODE SENSE reported was refused, asc=" << acc.asc();
}

// WRITE SAME carries a WRPROTECT field in cdb[1] bits 7-5 and this device advertises no
// protection at all (RC16 byte 12 is zero), so a nonzero value asks for something it
// cannot do. Rejected before the UNMAP branch, which reads the same byte.
// Killing mutation: delete the `cdb[1] & 0xe0` check.
TEST_F(TcmuTest, write_same_refuses_protection_this_device_does_not_advertise) {
    if (skip_reason) return;
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty());

    const uint64_t lba = IO_OFF / 512;
    SgResult prot, nprot10;
    int a = -1, b = -1;
    test::run_off_vcpu([&] {
        uint8_t c1[16] = {0x93, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};   // WRPROTECT=1
        for (int i = 0; i < 8; i++) c1[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
        a = sg_io(sd, c1, sizeof(c1), nullptr, 0, SG_DXFER_NONE, prot);
        // the 10-byte form has no NDOB, so bit 0 there is reserved rather than a feature
        uint8_t c2[10] = {0x41, 0x01, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                          (uint8_t)(lba >> 8), (uint8_t)lba, 0, 0, 1, 0};
        b = sg_io(sd, c2, sizeof(c2), nullptr, 0, SG_DXFER_NONE, nprot10);
    });
    ASSERT_EQ(0, a); ASSERT_EQ(0, b);
    EXPECT_EQ(0x02, (int)prot.status) << "WRITE SAME(16) with WRPROTECT was served";
    EXPECT_EQ(0x24, prot.asc());
    EXPECT_EQ(0x02, (int)nprot10.status) << "WRITE SAME(10) with a reserved bit set was served";
    EXPECT_EQ(0x24, nprot10.asc());
}

// NOT A WITNESS for the claim-across-cleanup fix, and labelled so it cannot be mistaken
// for one: destroy_orphan() has always refused a tombstone its leading devlock_free()
// probe finds held, so this case passes both before and after that change. What it pins
// is the refusal vocabulary and that a refused cleanup leaves the registration alone --
// which the hold depends on being right, and which nothing else asserted for tcmu (vduse
// has the analogous case, tcmu did not). The window the hold closes is between that
// probe and the rmdir, and closing it is only observable as an interleaving, so it stays
// booked rather than covered by a case that would pass either way.
TEST_F(TcmuTest, destroy_orphan_refuses_a_registration_another_holder_claimed) {
    if (skip_reason) return;
    // No HBA while planting, for the reason destroy_orphan_removes_a_dead_registration
    // states in full: a listener's on_added() probe overlaps this one and reads it as a
    // live server.
    delete sys;
    sys = nullptr;
    ASSERT_EQ(0, plant_orphan(BS_PATH, TEST_IDENTITY, IMG_SIZE));
    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS);
    ASSERT_NE(nullptr, sys);
    std::string lp = lock_path(TEST_IDENTITY);
    DEFER({ ::unlink(lp.c_str()); ::rmdir(BS_PATH); });

    // hold the tombstone the way a successor daemon's devlock_acquire would
    int fd = ::open(lp.c_str(), O_RDWR | O_CLOEXEC);
    ASSERT_GE(fd, 0) << "cannot open the planted tombstone";
    DEFER(::close(fd));
    ASSERT_EQ(0, ::flock(fd, LOCK_EX | LOCK_NB));

    // built by hand rather than taken from list_orphans(): the scan uses the same probe,
    // so a held tombstone is not reported as an orphan at all. That is the point -- the
    // race this guards is a claim landing after that scan and before the rmdir.
    BlkDevInfo rec = make_info();
    errno = 0;
    EXPECT_EQ(-1, sys->destroy_orphan(rec));
    EXPECT_EQ(EBUSY, errno) << "a claimed tombstone must be refused as busy, not forced";
    EXPECT_EQ(0, ::access(BS_PATH, F_OK)) << "a refused destroy_orphan removed the registration anyway";
    EXPECT_EQ(0, ::access(lp.c_str(), F_OK)) << "a refused destroy_orphan unlinked a lock file it does not hold";
}

// shutdown() used to release the claim before destroy_backstore(), so a failure there
// returned -1 leaving an ENABLED backstore wearing a FREE tombstone -- precisely what
// list_orphans() and the HBA's initial_scan hand to the next adopter, which would then
// take over a registration this server still owns and still has to remove. ublk orders
// it the other way for the same reason, and this file's own rollback() already did.
//
// The failure is made real rather than injected: a LUN under a DIFFERENT wwn, which the
// device never attached (loopback_lun is false) and so never detaches, makes configfs
// answer the rmdir with EBUSY on s_dependent_count. That is the shape the fix's comment
// names -- "a LUN still references the backstore" -- and it is the one the reviewer
// pointed at, so the case does not depend on a fault only this test can produce.
//
// Killing mutations: move release_lock() back above destroy_backstore() (the HELD
// assertion and the list_orphans one both reddens); drop the `lock_fd < 0` guard on the
// retry (the second shutdown reddens, because flock is per open file description and an
// unguarded acquire_lock() answers EWOULDBLOCK against ourselves and reports the device
// as held by another server).
//
// THIS CASE TAKES ABOUT 61 SECONDS and that is not a hang. Measured: 61166 ms with the
// fix and 61702 ms against the unfixed code, with the gap in the same place both times --
// between start() returning and the first line of the teardown -- so it is the cost of
// lun_attach() below and not of anything this round changed. Attaching a LUN to a
// backstore that is enabled AND being served makes the kernel scan a new loopback host
// through this daemon; the three existing lun_attach() callers pay 150-250 ms because
// they attach to a planted backstore nothing is serving. No cheaper way to get a genuine
// EBUSY was found: configfs answers EBUSY on rmdir only while s_dependent_count is
// nonzero, and a LUN symlink is what raises it. Kept at that price because it is the only
// case that reddens the fix, and it is what demonstrated the harm at HEAD -- the device
// really was being offered to orphan recovery while this server still owned it.
TEST_F(TcmuTest, shutdown_keeps_the_claim_when_backstore_removal_fails) {
    if (skip_reason) return;
    TcmuHBA::Config cfg(make_info());
    cfg.loopback_lun = false;   // so shutdown() cannot detach the LUN this case attaches
    cfg.loopback_wwn = TEST_WWN;
    cfg.timeout = 30;
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    ASSERT_STREQ("HELD", lock_state(TEST_IDENTITY));

    const char* bs_name = "photon-tcmu-test";   // BS_PATH's last component
    ASSERT_EQ(0, lun_attach(PASSIVE_WWN, BS_PATH, bs_name))
        << "could not attach the extra LUN that makes the rmdir busy";
    DEFER(lun_detach(PASSIVE_WWN, bs_name));

    errno = 0;
    EXPECT_EQ(-1, dev->shutdown());
    EXPECT_EQ(EBUSY, errno) << "the teardown failed for some other reason";
    // THE WITNESS. Before the fix this read "not-held" while the backstore was still
    // enabled and still this object's to remove.
    EXPECT_STREQ("HELD", lock_state(TEST_IDENTITY))
        << "the claim was handed back although the teardown did not finish";
    EXPECT_EQ(0, ::access(BS_PATH, F_OK)) << "the registration went away anyway";
    bool offered = false;
    for (auto& o : sys->list_orphans())
        if (o.identity == TEST_IDENTITY) offered = true;
    EXPECT_FALSE(offered)
        << "a device this server still owns was offered to orphan recovery";

    // Once the LUN is gone the retry must finish the job, reusing the claim it is
    // still holding rather than trying to take it again.
    lun_detach(PASSIVE_WWN, bs_name);
    EXPECT_EQ(0, dev->shutdown()) << "the retry after a busy teardown failed";
    EXPECT_STREQ("not-held", lock_state(TEST_IDENTITY));
    EXPECT_NE(0, ::access(BS_PATH, F_OK)) << "the registration survived a successful shutdown";
}

// ---------------------------------------------------------------------------
// The abrupt-exit witness for the ring-replay fix.
//
// Everything else in this suite reaches a dead daemon through detach(), and no
// detach() can produce the state this needs: serve_stop()'s `while (in_flight)`
// wait is unconditional, and every handle_cmd advances cmd_tail before it
// decrements in_flight, so on any orderly exit nothing dispatched is left
// un-tailed. The bug lives in the window between a handler writing into the
// shared ring and the tail reaching it, and the only way into that window is a
// process that dies without running any of its teardown.
//
// So this is a child, in the harness's suite-child mode -- the mode that exists
// for a child which is not a consumer, and which drops the inherited fd table and
// bounds the wait before the body runs. The child builds a whole server (photon,
// HBA, device, LUN), parks the ring in the target state, and _exit()s: no
// serve_stop, no munmap, no devlock_release, no destructors. The kernel keeps the
// ring for it: tcmu_release() frees only the data of KEEP_BUF commands and clears
// TCMU_DEV_BIT_OPEN, so neither cmd_head, cmd_tail nor the entries between them are
// a daemon's to lose, and its successor resumes from cmd_tail exactly as an
// adopting start() after a detach() does.
//
// The state it leaves, and why it exercises BOTH halves of the fix:
//   entry A -- dispatched, parked inside the backend's pwritev, never answered.
//              Its union still holds a request, so a successor must re-dispatch
//              it. Before the fix the iov_base fields had been converted to
//              absolute pointers IN PLACE, so the successor added its own mapping
//              base to them and every buffer landed outside its mapping.
//   entry B -- dispatched, answered GOOD, marked. Its bytes are already in the
//              backend, so this is the "committed data survives the crash" half.
//   entry X -- dispatched, answered CHECK CONDITION, marked. This is the one that
//              gives the marker teeth, and B alone cannot: a GOOD response leaves
//              the whole sense buffer zero, so a successor that re-dispatched it
//              would read cdb_off 0 and decode the mailbox's version byte as an
//              unknown opcode -- harmless. X's sense carries an ASC at
//              sense_buffer[12], which is cdb_off's byte 4, so re-dispatching it
//              reads cdb_off = ASC << 32, an address tens of gigabytes past the
//              mapping, and faults on the first byte of the CDB.
//
// B and X are both stuck behind A because advance_tail() publishes only a
// contiguous run, and a re-dispatch of either reads iov_cnt out of
// {scsi_status, padding} and cdb_off out of sense_buffer[8..15].
//
// The parent then adopts and asserts on the machine, not on the child's word: a
// successful start() that did not fault, entry A's pattern present (the successor
// harvested it), B's and X's patterns present from the writes the dead daemon
// already completed, an untouched region still zero -- the corruption detector --
// and one live write through the adopted ring.
// ---------------------------------------------------------------------------

// The child's failure codes, written into ConsumerReport::stage. There is no code
// for success and there cannot be one: the success path is _exit(), which is the
// only thing that stops the destructors and serve_stop() from draining the ring the
// case is about, and a child that exits without the harness publishing `magic` gives
// the parent nothing but its wait status. So the parent reads success as
// "status 0 and stage CONS_EXIT" -- the one combination this body can produce, since
// every failure returns an errno and the harness publishes that. 1000+ because the
// codes below that are the harness's own.
enum : int32_t {
    CRASH_DONE  = 0,
    CRASH_ARGV  = 1000,   // its argv did not decode
    CRASH_INIT,           // photon::init
    CRASH_IMAGE,          // opening the backend image
    CRASH_HBA,            // new_tcmu_hba
    CRASH_START,          // new_device / start()
    CRASH_NODE,           // waiting for the scsi node
    CRASH_PARK,           // the first write never reached the gate
    CRASH_LAND,           // the second write never returned from the backend
    CRASH_SHORT,          // the short write never returned from the backend
};

static const char* crash_step_name(int32_t s) {
    switch (s) {
    case CRASH_DONE:  return "nothing failed";
    case CRASH_ARGV:  return "decoding its argv";
    case CRASH_INIT:  return "photon::init";
    case CRASH_IMAGE: return "opening the backend image";
    case CRASH_HBA:   return "creating the HBA";
    case CRASH_START: return "creating or starting the device";
    case CRASH_NODE:  return "waiting for the scsi node";
    case CRASH_PARK:  return "parking the first write in the backend";
    case CRASH_LAND:  return "the second write reaching the backend";
    case CRASH_SHORT: return "the short write reaching the backend";
    default:          return "an unknown step";
    }
}

// The child builds an HBA, starts a device, attaches a loopback LUN and waits for
// the node, so it needs room for a scsi scan on top of the two writes -- and the
// scan is the expensive part, measured at a minute in this suite's own
// shutdown_keeps_the_claim case. Generous because an abandoned child is worse than
// a slow one: the harness never kills it, and this one would still be serving.
static constexpr uint64_t CRASH_WAIT_US = 240ull * 1000000;
// cmd_time_out for the child's device, in seconds. Short on purpose: the three
// commands it leaves behind are never answered, and letting the kernel expire them
// is the more interesting case for the successor -- tcmu_check_expired_ring_cmd()
// sets TCMU_CMD_BIT_EXPIRED, completes the se_cmd and leaves both the ring entry and
// its data area alone, so retiring a marked entry still finds the command and
// tcmu_handle_completion() treats an expired one as reclaim-and-free rather than as a
// broken ring. Freeing the data area waits for the tail to reach the entry, which is
// why an expired entry A still carries the payload its successor is about to write.
static constexpr uint64_t CRASH_CMD_TIMEOUT = 5;
// How long the child waits between the short write returning from the backend and
// _exit(). handle_cmd writes the response and the mark immediately after emul_write
// returns -- no blocking step in between -- so this is margin, not a race being
// papered over.
static constexpr uint64_t CRASH_SETTLE_US = 500000;

// One page per write, so each is a single SCSI command and therefore a single ring
// entry. IO_LEN (256 KiB) would be several: hw_max_sectors bounds a request and the
// block layer splits what exceeds it, and a split turns "entry A is parked, entries B
// and X are answered behind it" into a race over how many of A's pieces are still
// queued.
static constexpr size_t CRASH_LEN = 4096;

// Where the three writes land, a region nothing ever writes -- the corruption
// detector, since a successor that re-dispatched an answered entry would take cdb_off
// out of its sense data and put its buffers somewhere else -- and a fourth region for
// the live IO that ends the case, so that check cannot be read as having disturbed
// any of the three it follows.
static constexpr uint64_t CRASH_OFF_A = IO_OFF;                 // parked, un-answered
static constexpr uint64_t CRASH_OFF_B = 9ull << 20;             // answered GOOD
static constexpr uint64_t CRASH_OFF_X = 17ull << 20;            // answered CHECK CONDITION
static constexpr uint64_t CRASH_OFF_C = 32ull << 20;            // never touched
static constexpr uint64_t CRASH_OFF_D = 40ull << 20;            // the adopted device's own IO

// A backend that parks its FIRST pwritev and never lets it go, so the command
// carrying it stays dispatched and un-answered while later ones are served and
// answered behind it. That ordering is the whole fixture: the ring's tail can only
// advance contiguously, so answered entries behind a parked one are exactly the
// state a crash leaves for a successor to misread.
//
// The wait is photon::thread_usleep, not usleep: this runs on a handle_cmd
// coroutine, and blocking the vcpu would stop the pump that has to dispatch the
// later commands. `open` is never set -- the process ends by _exit() with the write
// still parked, which is the point, so the loop below is not a leak.
//
// `shorten` is the other knob, and it is what makes the third entry a CHECK
// CONDITION: emul_write answers a short count with MEDIUM ERROR, whose ASC lands at
// sense_buffer[12] -- cdb_off's byte 4 -- so a successor that re-dispatched that
// entry would fault tens of gigabytes outside its mapping rather than decode
// something harmless. The count is shortened, not the write: the bytes do land, so
// the parent can see that the dead daemon really served it.
class FirstWriteGate : public test::BackendProbe {
public:
    // does NOT own f, exactly as BackendProbe does not
    explicit FirstWriteGate(fs::IFile* f) : test::BackendProbe(f) {}

    std::atomic<int> parked{0};   // 1 while the first write is held
    std::atomic<int> landed{0};   // writes that reached the underlying file
    std::atomic<bool> open{false};
    std::atomic<bool> shorten{false};

    ssize_t pwritev(const struct iovec* iov, int iovcnt, off_t offset) override {
        if (landed.load() + parked.load() == 0) {
            parked.store(1);
            while (!open.load())
                photon::thread_usleep(2000);
            parked.store(0);
        }
        ssize_t r = test::BackendProbe::pwritev(iov, iovcnt, offset);
        if (r > 0) {
            landed.fetch_add(1);
            if (shorten.load())
                r -= 1;
        }
        return r;
    }
};

// The child. Returns only on a failure; the success path is _exit(), which is what
// makes the ring state survive it.
static int tcmu_crash_child(const test::ConsumerSuiteChild& c) {
    // argv is [0] this binary, [1] the sentinel, [2] the tag, [3] the channel fd,
    // [4..] the suite's fields with the node LAST. This body takes no fields of its
    // own: the identity, image path, subtype, prefix and lock dir are all constants
    // of this same binary, which is what the child is.
    if (c.argc < 5 || !c.rep) {
        if (c.rep)
            c.rep->stage = CRASH_ARGV;
        return EINVAL;
    }
    test::ConsumerReport* rep = c.rep;
    int32_t step = CRASH_DONE;
    // Written straight into the page rather than kept locally and copied at the end,
    // because the success path never reaches an end: _exit() runs no destructors.
    auto note = [&](int32_t s, int e) {
        if (step != CRASH_DONE) return;
        step = s;
        rep->stage = s;
        rep->child_errno = e;
    };

    if (photon::init(test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE) != 0) {
        note(CRASH_INIT, EIO);
        return EIO;
    }
    // No photon::fini() and no DEFER anywhere below: this child's contract is that
    // nothing of its teardown runs.

    // NOT TestImage::create(), which unlinks the path and makes a new inode: the
    // parent's `file` was opened on the inode its own SetUp created, and every
    // assertion in the case is read through it. SetUp already sized IMG_PATH, so the
    // child opens that inode -- which is also the honest shape of a crash, where the
    // dead daemon and its successor share a backend rather than each making one.
    auto* lfs = fs::new_localfs_adaptor();
    if (!lfs) {
        note(CRASH_IMAGE, ENOMEM);
        return ENOMEM;
    }
    fs::IFile* img_file = lfs->open(IMG_PATH, O_RDWR);
    if (!img_file) {
        int e = errno ? errno : ENOENT;
        note(CRASH_IMAGE, e);
        return e;
    }

    auto* hba = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS);
    if (!hba) {
        note(CRASH_HBA, errno);
        return errno;
    }

    BlkDevInfo info;
    info.identity = TEST_IDENTITY;
    info.size = IMG_SIZE;
    info.sector_size_shift = 9;   // tcmu is always a 512-byte sector
    info.features = FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
    TcmuHBA::Config cfg(info);
    cfg.loopback_lun = true;
    cfg.loopback_wwn = TEST_WWN;
    cfg.timeout = CRASH_CMD_TIMEOUT;

    FirstWriteGate gate(img_file);
    auto* dev = hba->new_device(cfg);
    if (!dev || dev->start(&gate) < 0) {
        note(CRASH_START, errno);
        return errno ? errno : EIO;
    }
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    if (sd.empty()) {
        note(CRASH_NODE, ENODEV);
        return ENODEV;
    }

    // Submitted from coroutines rather than this one: an sg_io here would block the
    // vcpu that serves the ring, and the first of these is never going to be
    // answered at all. WRITE(10)'s transfer length is cdb[7..8], in 512-byte sectors.
    auto submit_write = [&](uint64_t off, void* data) {
        photon::thread_create11([&, off, data] {
            test::run_off_vcpu([&] {
                SgResult r;
                uint32_t lba = (uint32_t)(off / 512);
                uint8_t cdb[10] = {0x2a, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                                   (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                                   (uint8_t)(CRASH_LEN / 512 / 256), (uint8_t)(CRASH_LEN / 512), 0};
                sg_io(sd, cdb, sizeof(cdb), data, CRASH_LEN, SG_DXFER_TO_DEV, r);
            });
        });
    };
    // Poll on the vcpu, yielding: whatever sets the condition is a handle_cmd
    // coroutine that has to run for this wait to end. 10s per predicate.
    auto spin_until = [&](auto pred) {
        for (int i = 0; i < 5000; i++) {
            if (pred())
                return true;
            photon::thread_usleep(2000);
        }
        return pred();
    };

    auto p1 = test::pattern(0xa1, CRASH_LEN);
    auto p2 = test::pattern(0xb2, CRASH_LEN);
    auto p3 = test::pattern(0xcc, CRASH_LEN);

    submit_write(CRASH_OFF_A, p1.data());
    // Entry A must be dispatched and parked BEFORE B is submitted, or the ring order
    // -- and with it which entries the tail is stuck behind -- is a coin toss.
    if (!spin_until([&] { return gate.parked.load() != 0; })) {
        note(CRASH_PARK, ETIMEDOUT);
        return ETIMEDOUT;
    }

    submit_write(CRASH_OFF_B, p2.data());
    if (!spin_until([&] { return gate.landed.load() >= 1; })) {
        note(CRASH_LAND, ETIMEDOUT);
        return ETIMEDOUT;
    }

    // X, the entry that gives the marker its teeth. B's GOOD response leaves the
    // sense buffer all zeros, so a successor that re-dispatched B would read
    // cdb_off 0 and decode the mailbox's version byte as an unknown opcode --
    // harmless, and the marker would be untested. X answers CHECK CONDITION instead,
    // and an ASC at sense_buffer[12] is cdb_off's byte 4, so re-dispatching it faults
    // tens of gigabytes outside the mapping. `landed` reaching 2 is the child's own
    // proof that X was served; that it was then ANSWERED is what CRASH_SETTLE_US
    // covers, exactly as for B.
    gate.shorten.store(true);
    submit_write(CRASH_OFF_X, p3.data());
    if (!spin_until([&] { return gate.landed.load() >= 2; })) {
        note(CRASH_SHORT, ETIMEDOUT);
        return ETIMEDOUT;
    }

    // Both answered entries have reached the backend; give handle_cmd the moment it
    // needs to write each response and mark, then die with all three in the ring.
    photon::thread_usleep(CRASH_SETTLE_US);
    ::_exit(0);
}

// Installed from main() ahead of consumer_child_main(), because the child is this
// same binary re-executed and so runs the same main(). See consumer_set_suite_body
// for why an installation call site beats a namespace-scope initializer.
static void install_tcmu_crash_child() {
    test::consumer_set_suite_body(tcmu_crash_child);
}

TEST_F(TcmuTest, a_daemon_that_dies_mid_ring_is_recovered_by_its_successor) {
    if (skip_reason) return;
    // ONE listener. The child creates its own HBA, and two HBAs on a subtype both
    // receive the multicast ADDED for the backstore it enables, so the fixture's has
    // to go first -- the same reason destroy_orphan_removes_a_dead_registration
    // deletes it before planting.
    delete sys;
    sys = nullptr;

    test::ConsumerIoResult r;
    test::run_off_vcpu([&] {
        r = test::consumer_spawn_suite(std::vector<std::string>(), TEST_IDENTITY, CRASH_WAIT_US);
    });
    // The child's last act is _exit(), so the harness never publishes `magic` and
    // there is no report to read: what comes back is the wait status alone. Status 0
    // with stage CONS_EXIT is the combination only that _exit() produces -- every
    // failure returns an errno, which the harness publishes with the stage it noted,
    // and a signal sets status to EIO. Diagnostic only: the witness is the machine
    // state asserted below, not anything the child said about itself.
    ASSERT_FALSE(r.hung) << "the child was abandoned; pid " << (int64_t)r.pid << " may still be serving";
    ASSERT_EQ(0, r.status)
        << "the child died at "
        << ((int32_t)r.stage >= CRASH_ARGV ? crash_step_name((int32_t)r.stage)
                                           : test::consumer_stage_name(r.stage))
        << ", errno " << r.child_errno << ", exit " << r.exit_code;
    ASSERT_EQ((int32_t)test::CONS_EXIT, (int32_t)r.stage)
        << "the child reported instead of dying abruptly, so it ran its teardown and "
           "drained the ring this case is about";

    // What the death must have left behind, checked here rather than trusted: an
    // enabled registration, a LUN and therefore a node, and a tombstone the flock
    // died with.
    ASSERT_EQ(0, ::access(BS_PATH, F_OK)) << "the backstore the child created is gone";
    EXPECT_STREQ("not-held", lock_state(TEST_IDENTITY))
        << "the child's claim outlived it, so this is not a crash recovery at all";
    std::string sd = wait_photon_sd(IMG_SIZE / 512);
    ASSERT_FALSE(sd.empty()) << "no scsi node survived the child";

    sys = new_tcmu_hba(SUITE_SUBTYPE, DEV_CONFIG_PREFIX, SUITE_LOCKS);
    ASSERT_NE(nullptr, sys);
    auto cfg = make_cfg(/*loopback=*/true);
    auto dev = sys->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    // THE WITNESS, and it is that this process is still alive to run the assertions
    // below. Before the fix, start()'s drain_ring re-dispatched entry X and read its
    // cdb_off out of the ASC the dead daemon had written into sense_buffer[12], which
    // is that field's byte 4: an address tens of gigabytes past the mapping, faulting
    // on the first byte of the CDB. And because the tail can never pass an entry the
    // handler faulted on, every restart re-poisons the same slot.
    ASSERT_EQ(0, dev->start(file))
        << "adopting a ring a dead daemon left half-finished was refused, errno " << errno;
    DEFER(dev->shutdown());

    // Give the successor's pump the moment it needs to harvest entry A, whose write
    // was parked in a backend that died with the child and is re-run now against
    // this one.
    auto p1 = test::pattern(0xa1, CRASH_LEN);
    auto p2 = test::pattern(0xb2, CRASH_LEN);
    auto p3 = test::pattern(0xcc, CRASH_LEN);
    for (int i = 0; i < 2500; i++) {
        std::vector<char> a(CRASH_LEN, 0);
        struct iovec iov{a.data(), a.size()};
        if (file->preadv(&iov, 1, CRASH_OFF_A) == (ssize_t)CRASH_LEN &&
            memcmp(a.data(), p1.data(), CRASH_LEN) == 0)
            break;
        photon::thread_usleep(2000);
    }
    auto read_at = [&](uint64_t off) {
        std::vector<char> b(CRASH_LEN, 0);
        struct iovec iov{b.data(), b.size()};
        EXPECT_EQ((ssize_t)CRASH_LEN, file->preadv(&iov, 1, off));
        return b;
    };
    // Entry A was dispatched and never answered, so it was still a pristine request
    // and the successor served it. This is the half the private iovec copy fixes:
    // its iov_base fields used to be converted in place, and the successor added its
    // own mapping base to pointers that were already absolute.
    EXPECT_EQ(0, memcmp(read_at(CRASH_OFF_A).data(), p1.data(), CRASH_LEN))
        << "the un-answered entry was not harvested by the successor";
    // Entries B and X were answered and marked, so the successor retired them
    // instead of re-dispatching. Their bytes are what the dead daemon wrote -- for X
    // that is a write it reported as an error, since the short count was synthetic --
    // and neither was run a second time.
    EXPECT_EQ(0, memcmp(read_at(CRASH_OFF_B).data(), p2.data(), CRASH_LEN))
        << "the entry answered GOOD does not hold what the dead daemon wrote";
    EXPECT_EQ(0, memcmp(read_at(CRASH_OFF_X).data(), p3.data(), CRASH_LEN))
        << "the entry answered CHECK CONDITION does not hold what the dead daemon wrote";
    // The corruption detector: nothing was ever written here, so a successor that
    // re-dispatched an answered entry through a wild cdb_off would have left a mark.
    std::vector<char> untouched(CRASH_LEN, 0x5a);
    struct iovec uiov{untouched.data(), untouched.size()};
    ASSERT_EQ((ssize_t)CRASH_LEN, file->preadv(&uiov, 1, CRASH_OFF_C));
    for (size_t i = 0; i < untouched.size(); i++)
        ASSERT_EQ(0, untouched[i]) << "a re-dispatched entry scribbled at byte " << i;

    // And the device is a working device afterwards, not merely a survived one:
    // one write through the adopted ring, checked in the backend it now points at.
    EXPECT_EQ(0, device_io(sd, test::pattern(0xc3, CRASH_LEN), /*verify_backend=*/true, CRASH_OFF_D))
        << "the adopted device does not serve I/O";
}

}  // namespace blk
}  // namespace photon

int main(int argc, char** argv) {
    // The crash child is this same binary re-executed with the consumer sentinel in
    // argv[1] and CONS_MODE_SUITE in argv[2], so its body has to be installed before
    // the dispatch below -- and before photon::init() and InitGoogleTest(), which the
    // dispatch itself has to precede.
    photon::blk::install_tcmu_crash_child();
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
