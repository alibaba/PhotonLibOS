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

// Built only on Linux (the vduse transport is LINUX-gated). Requires root, the
// vduse + virtio_vdpa modules and iproute2's `vdpa` tool; without them every
// case prints a skip notice and returns (see test::SkippableTest).
//
// The consumer side (the vdpa bus attach that turns the VDUSE registration into
// a /dev/vdX) is orchestrated by the tests themselves -- mirroring production,
// where QEMU's vhost-vdpa or an operator's `vdpa dev add` plays that role.
// CRITICAL ordering (a lesson paid for with a wedged VM): the daemon must serve
// BEFORE the consumer attaches, and the consumer must be detached BEFORE the
// daemon stops -- an unserved vduse device leaves any process touching its
// /dev/vdX in unkillable D state.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // O_DIRECT
#endif

#include "../blk.h"
// The VDUSE uapi copies, for the one registration blk cannot create (see
// raw_vduse_create). Shared with vduse.cpp instead of taken from
// <linux/vduse.h>: that header arrived in the kernel later than many build
// hosts' header packages, so a translation unit that includes it does not
// compile there at all.
#include "../vduse-uapi.h"

#include "../../test/gtest.h"
#include "harness.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>
#include <photon/common/utility.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>
// All three are C++-safe; <linux/virtio_ring.h> is the one that is not, and
// nothing here needs it.
#include <linux/virtio_blk.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace photon {
namespace blk {

static const char IMG_PATH[]       = "/tmp/photon-blk-vduse.img";
static constexpr uint64_t IMG_SIZE = 64ull << 20;
static const char TEST_NAME[]      = "photon-vduse-test";   // the vduse device name
static const char NAME_PREFIX[]    = "photon-vduse";        // sweep filter

// The lock directory every controller in this suite is built on. Stated here rather
// than left to a library default, because there is no default: a directory two
// applications share lets each one's orphan scan adopt the other's devices. Every
// tombstone path this file checks is built from it, so the suite cannot drift onto
// one directory for its controllers and probe another.
static const char SUITE_LOCKS[] = "/run/photon-blk";

static constexpr uint64_t IO_OFF = 1ull << 20;   // 1 MiB
static constexpr size_t   IO_LEN = 256ull << 10; // 256 KiB

// attach a vduse registration to the vdpa bus; returns the new /dev/vdX ("" on
// failure). Identifies OUR device by diffing /sys/block -- never by guessing a
// vd letter (the VM's own disks are virtio-blk too).
static std::string vdpa_attach(const char* name) {
    std::string cmd =
        "ls /sys/block | sort > /tmp/pv.before;"
        " vdpa dev add mgmtdev vduse name ";
    cmd += name;
    cmd += " 2>&1 || { echo VDUSE_TEST_ATTACH_FAIL; exit 0; };"
           " for i in $(seq 60); do"
           "  ls /sys/block | sort > /tmp/pv.after;"
           "  new=$(comm -13 /tmp/pv.before /tmp/pv.after | head -1);"
           "  [ -n \"$new\" ] && [ -b /dev/$new ] && { echo $new; exit 0; };"
           "  sleep 0.2;"
           " done;"
           " echo VDUSE_TEST_ATTACH_TIMEOUT";
    std::string out = test::sh_off_vcpu(cmd);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    if (out.empty() || out.find("VDUSE_TEST_ATTACH") != std::string::npos) {
        LOG_ERROR("vdpa attach of ` failed: `", name, out);
        return "";
    }
    return "/dev/" + out;
}

// Whether one of this suite's flock tombstones is CLAIMED right now: "HELD",
// "not-held", or "missing". Only a lock still held is residue -- the FILE surviving
// is the convention, since devlock_release() unlocks and closes and never unlinks --
// while a held lock hides a device from every later scan, because devlock_free()
// reads 0 and list_orphans() skips the entry. "missing" is its own answer rather than
// folded into not-held: a successful destroy_orphan() unlinks the tombstone, and
// reading that as "free" would turn the difference between the two outcomes into
// nothing. File-local rather than in the harness, which also compiles on macOS where
// neither this nor fd_count() below has the interface it needs.
static const char* lock_state(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return "missing";
    DEFER(::close(fd));
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        return "HELD";
    ::flock(fd, LOCK_UN);
    return "not-held";
}

// The bare removal. Every caller in this file goes through vdpa_detach() below, which
// is the only place allowed to issue it.
static void vdpa_del(const char* name) {
    std::string cmd = "vdpa dev del ";
    cmd += name;
    cmd += " 2>/dev/null; true";
    test::sh_off_vcpu(cmd);
}

// Is a vdpa consumer attached to this registration? The bus directory is the kernel's
// own record, and asking it is a stat -- no genl command is sent, which is the point,
// since a genl command against an unserved registration is what wedges.
static bool vdpa_attached(const char* name) {
    std::string p = std::string("/sys/bus/vdpa/devices/") + name;
    return ::access(p.c_str(), F_OK) == 0;
}

// Serve an orphaned registration through the product path, remove its consumer while
// it is served, then let go. start() on an orphan IS the productized rescue, so this
// needs no raw uapi. list_orphans() both finds the registration and proves nothing else
// is serving it: a tombstone is claimed before the char dev is opened and released by
// detach(), so an orphan that scan lists is unserved by construction -- and a scan
// cannot see one whose tombstone is missing at all, devlock_free() answering -1 for it.
// The recorded capacity is what stops the adopt being refused for config drift.
// Returns false if the registration could not be served, leaving it as it was found.
static bool rescue_serve(const char* name) {
    auto c = new_vduse_controller(SUITE_LOCKS);
    if (!c)
        return false;
    DEFER(delete c);
    for (auto& rec : c->list_orphans()) {
        if (rec.identity != name)
            continue;
        BlkConfig cfg;
        cfg.info.identity = rec.identity;
        cfg.info.size = rec.size ? rec.size : (4 << 20);
        auto d = c->new_device(cfg);
        if (!d)
            return false;
        DEFER(delete d);
        auto l = fs::new_localfs_adaptor();
        if (!l)
            return false;
        DEFER(delete l);
        auto f = l->open(IMG_PATH, O_RDWR | O_CREAT, 0644);
        if (!f)
            return false;
        if (d->start(f, /*ownership=*/true) < 0) {
            delete f;   // a refused start leaves the backend the caller's to delete
            return false;
        }
        vdpa_del(name);
        d->shutdown();
        return true;
    }
    return false;
}

// Remove this registration's vdpa consumer -- and never let that removal run against a
// registration nothing serves.
//
// The precondition belongs here rather than in each caller because the callers that
// matter are the ones running after an assertion failed. Removing a consumer of an
// UNSERVED vduse registration blocks forever in the kernel's queue freeze
// (blk_mq_freeze_queue_wait, under del_gendisk via virtblk_remove) waiting on a request
// only a daemon can answer, and it holds the machine-wide genl_lock while blocked, so
// nbd and tcmu stall with it; a task in D state ignores SIGKILL, and recovery is either
// a reboot or vduse-cli.cc's rescue drill. Measured, not theorised: on 2026-10-05
// orphan_recovery's adopt was refused, its DEFER issued a bare `vdpa dev del`, and
// three tasks -- that vdpa, a udev-worker closing the node, and a read-only
// `vdpa dev show` -- sat in D state for 30 minutes.
//
// Declining rather than removing is the fallback and not a fix: a stranded registration
// only wedges the NEXT command sent to it, whereas the removal wedges now. It is loud
// because the residue needs the drill above.
static void vdpa_detach(const char* name) {
    if (!vdpa_attached(name))
        return;   // nothing to remove; the bare command would have been a silent no-op
    // A held tombstone means a live daemon is in there serving it, so the removal can
    // drain. That is the normal case: every DEFER here orders the consumer off BEFORE
    // the daemon stops, precisely so that this holds.
    std::string ln = std::string(SUITE_LOCKS) + "/vduse-" + name + ".lock";
    if (!strcmp(lock_state(ln), "HELD"))
        return vdpa_del(name);
    if (rescue_serve(name))
        return;
    LOG_ERROR("vduse `: a consumer is attached to a registration nothing serves, and adopting it was refused, so `vdpa dev del` is being withheld -- it would block in the kernel's queue freeze holding the machine-wide genl_lock. Recover the registration with blk/test/vduse-cli.cc's rescue drill", name);
}

// How many descriptors this process holds. What it is for: a claim or a control
// channel that a call takes and forgets to give back is invisible to a directory
// listing or an access() check once the file it locked has been unlinked -- a flock
// on a deleted inode shows up in neither. Counting the table does. Measured the same
// way on both sides so the descriptor opendir() itself takes cancels out.
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

// The raw uapi, for the one registration blk cannot create: the create path
// clamps cfg.queues, so nothing reachable through blk.h can declare a vq_num
// above the clamp. System headers only -- pulling blk/utils.h in for its virtio
// structs is what the standing rule spelled out at PEER_MAX_QUEUES below forbids.
// Both return 0 or the errno of the step that failed, captured before the control
// fd is closed, so a DEFER can test the result without depending on errno
// surviving a close(2).
//
// Nothing this creates is ever attached to the vdpa bus, so there is no /dev/vdX
// and nothing that can wedge a process in D state.
static int raw_vduse_create(const char* name, uint32_t vq_num) {
    int c = ::open("/dev/vduse/control", O_RDWR | O_CLOEXEC);
    if (c < 0) {
        int e = errno ? errno : EIO;
        LOG_ERRNO_RETURN(0, e, "raw vduse create: open /dev/vduse/control failed");
    }
    DEFER(::close(c));
    uint64_t ver = 0;   // the classic single-address-space ABI
    if (::ioctl(c, VDUSE_SET_API_VERSION, &ver) < 0) {
        int e = errno ? errno : EIO;
        LOG_ERRNO_RETURN(0, e, "raw vduse create: SET_API_VERSION failed");
    }
    alignas(vduse_dev_config) uint8_t raw[sizeof(vduse_dev_config) + sizeof(::virtio_blk_config)];
    memset(raw, 0, sizeof(raw));
    auto* cc = (vduse_dev_config*)raw;
    snprintf(cc->name, sizeof(cc->name), "%s", name);
    cc->vendor_id = 0x1af4;
    cc->device_id = VIRTIO_ID_BLOCK;
    // Both bits, and ACCESS_PLATFORM is the one that is not obvious: CREATE_DEV
    // answers EINVAL for a device that does not offer it, measured on
    // 7.0.0-31-generic with everything else in this payload held constant.
    cc->features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_F_ACCESS_PLATFORM);
    cc->vq_num = vq_num;
    cc->vq_align = (uint32_t)sysconf(_SC_PAGESIZE);
    cc->config_size = sizeof(::virtio_blk_config);
    auto* bc = (::virtio_blk_config*)(raw + sizeof(vduse_dev_config));
    bc->capacity = 8192;   // 4 MiB @512
    bc->blk_size = 512;
    bc->num_queues = (uint16_t)vq_num;
    if (::ioctl(c, VDUSE_CREATE_DEV, cc) < 0) {
        int e = errno ? errno : EIO;
        LOG_ERRNO_RETURN(0, e, "raw vduse create: CREATE_DEV of ` with vq_num ` failed", name, vq_num);
    }
    return 0;
}

static int raw_vduse_destroy(const char* name) {
    int c = ::open("/dev/vduse/control", O_RDWR | O_CLOEXEC);
    if (c < 0) {
        int e = errno ? errno : EIO;
        LOG_ERRNO_RETURN(0, e, "raw vduse destroy: open /dev/vduse/control failed");
    }
    DEFER(::close(c));
    uint64_t ver = 0;
    ::ioctl(c, VDUSE_SET_API_VERSION, &ver);   // best effort
    char nm[VDUSE_NAME_MAX];
    memset(nm, 0, sizeof(nm));
    snprintf(nm, sizeof(nm), "%s", name);
    if (::ioctl(c, VDUSE_DESTROY_DEV, nm) < 0) {
        int e = errno ? errno : EIO;
        LOG_ERRNO_RETURN(0, e, "raw vduse destroy: DESTROY_DEV of ` failed", name);
    }
    return 0;
}

// The geometry every config in this file starts from. At file scope rather than a
// fixture member because two fixtures need it: the gated one, and the ungated one
// covering what a constructed device already knows before anything reaches the
// kernel. One definition, so the two cannot drift onto different geometries.
static BlkDevInfo make_info() {
    BlkDevInfo i;
    i.identity = TEST_NAME;
    i.size = IMG_SIZE;
    i.sector_size_shift = 9;
    i.features = FEATURE_FLUSH;   // DISCARD/WRITE_ZEROES not offered in P1
    return i;
}

// The kernel-side state a start() refused part-way through could leave behind,
// as one sortable listing: the vduse registrations, and each of this suite's
// flock files marked HELD or FREE. The lock FILE is expected to survive --
// devlock_release() unlocks and closes, it never unlinks, and the orphan scan
// keys on "file exists and is free" -- so what must not survive is a lock still
// held, which would wedge every later run.
static std::string residue() {
    return test::sh_off_vcpu(
        "{ ls -1 /dev/vduse 2>/dev/null;"
        "  for f in " + std::string(SUITE_LOCKS) +
        "/vduse-*.lock; do [ -e \"$f\" ] || continue;"
        "    flock -n \"$f\" -c true 2>/dev/null && echo \"FREE $f\" || echo \"HELD $f\"; done;"
        "} | sort");
}

// The capacity record inside one of this suite's tombstones, read with plain POSIX
// rather than through the transport. Two reasons: the suite's standing rule is that
// it must not reach into blk/'s internals to learn the values it asserts against,
// and a witness that shared the implementation's own reader would agree with it by
// construction. Returns false for anything that is not exactly one eight-byte
// record -- an empty file, a missing one, a longer one -- because "no record" has
// to be an answer the implementation can distinguish from a recorded capacity, and
// this is how the cases below tell the two apart.
static bool tombstone_capacity(const std::string& name, uint64_t* out) {
    std::string p = std::string(SUITE_LOCKS) + "/vduse-" + name + ".lock";
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    DEFER(::close(fd));
    char buf[64];
    if (::read(fd, buf, sizeof(buf)) != (ssize_t)sizeof(uint64_t))
        return false;
    *out = *(const uint64_t*)buf;
    return true;
}

// The virtio bus publishes the consumer's NEGOTIATED feature set as a
// bitstring -- one char per bit, bit 0 first -- in the sysfs of the virtio
// device the gendisk hangs off. That is the kernel's record of what arrived
// over the wire, not a readback of our own config image, so it is independent
// evidence for the F_MQ gate, and it comes from a different wire field than
// the mq directory count does (the feature word vs config-space num_queues).
// Returns -1 when the attribute cannot be read.
static int virtio_feature_bit(const std::string& kname, uint32_t bit) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/block/%s/device/features", kname.c_str());
    FILE* f = ::fopen(path, "r");
    if (!f)
        return -1;
    DEFER(::fclose(f));
    char buf[128] = {};
    if (!::fgets(buf, sizeof(buf), f))
        return -1;
    return strlen(buf) > bit ? (buf[bit] == '1' ? 1 : 0) : -1;
}

// What the guest reads back as this disk's serial -- the far end of the wire from our
// own `serial` member, not a readback of it.
//
// Read through sh_off_vcpu and NOT with fopen, because the attribute is not cached:
// reading it makes the kernel issue a live GET_ID and block until the device answers.
// This device is served by this process's own photon vcpu, so reading it from that
// vcpu deadlocks the daemon against itself. That is measured, not conjecture: the
// reader sat in uninterruptible sleep in the block layer's request wait, a udev worker
// that later closed the node blocked behind it, neither could be killed, and the
// machine needed a hard reset. sh_off_vcpu runs the read in a child of another OS
// thread, which leaves the vcpu free to serve the GET_ID the read asks for. kname is
// a kernel-assigned node name (vdX), never caller text.
// Returns false when the attribute is missing or unreadable, which the caller treats
// as a failed case rather than substituting a value.
static bool sysfs_serial(const std::string& kname, std::string* out) {
    std::string cmd = "cat /sys/block/";
    cmd += kname;
    cmd += "/serial 2>/dev/null || echo VDUSE_TEST_SERIAL_UNREADABLE";
    std::string s = test::sh_off_vcpu(cmd);
    if (s.find("VDUSE_TEST_SERIAL_UNREADABLE") != std::string::npos)
        return false;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    if (s.empty())
        return false;
    *out = s;
    return true;
}

// The kernel's own per-device bound on how long it waits for a daemon that has gone
// quiet, in microseconds. Read rather than spelled: the transport writes
// BlkConfig::timeout into this same attribute at start(), so what comes back is the
// value the device under test is actually running with, and it is the length of the
// stall that silencing the message loop early can produce. Returns 0 when the
// attribute is missing or unreadable, which the one caller treats as a failed case
// rather than substituting a constant -- a bound that was never measured cannot
// witness anything.
static uint64_t msg_timeout_us(const char* name) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/vduse/%s/msg_timeout", name);
    FILE* f = ::fopen(path, "r");
    if (!f)
        return 0;
    DEFER(::fclose(f));
    char buf[32] = {};
    if (!::fgets(buf, sizeof(buf), f))
        return 0;
    return strtoull(buf, nullptr, 10) * 1000 * 1000;
}

// virtio-blk's multiqueue feature bit (virtio 1.2 §5.2) and the transport's
// queue clamp, spelled out here on purpose -- the suite family's standing rule
// (see test-vhost-user.cpp's PEER_MAX_QUEUES): a suite must not reach into
// blk/utils.h to learn the values it asserts against, because that is how a
// suite becomes self-consistently wrong. If the two ever disagree these tests
// go red, which is the point.
static constexpr uint32_t FEAT_BIT_BLK_MQ = 12;
static constexpr uint32_t PEER_MAX_QUEUES = 64;

// The construction-time half of this suite: what a caller can learn from a config
// and from a device that has been built but not started.
//
// Ungated, and that is the whole point of splitting it out. new_device() runs
// validate() -- pure by its own contract, no I/O and no kernel access -- and a
// constructor that only derives fields from the config, and start() rejects a null
// backend before it opens the control device. So none of this needs root, the vduse
// module or the vdpa tool. Behind VduseTest's gate CI executed none of it: the
// runners there are unprivileged containers on a 5.10 node kernel, which predates
// vduse entirely, so the gate could never open no matter how the suite was written.
//
// The controller still gets SUITE_LOCKS rather than a writable stand-in. Nothing
// here opens it, and if a case ever starts to, a non-root run fails loudly instead
// of passing against a directory only some machines happen to permit.
class VduseConfigTest : public ::testing::Test {
public:
    VduseController* ctl = nullptr;

    void SetUp() override {
        ctl = new_vduse_controller(SUITE_LOCKS);
        ASSERT_NE(nullptr, ctl);
    }

    void TearDown() override {
        delete ctl;
        ctl = nullptr;
    }
};

class VduseTest : public test::SkippableTest {
public:
    test::TestImage img;
    fs::IFile* file = nullptr;
    // The default lock dir, and the only route this fixture has to a device: every
    // new_device() and every list_orphans() below goes through it, which is the
    // point of the controller -- a recovery loop cannot scan one dir and claim in
    // another.
    VduseController* ctl = nullptr;

    void SetUp() override {
        if (geteuid() != 0)
            return report_skip("vduse test requires root");
        ::system("modprobe vduse 2>/dev/null");
        ::system("modprobe virtio_vdpa 2>/dev/null");   // separate: `modprobe a b`
                                                        // passes b as a PARAMETER of a
        if (::access("/dev/vduse/control", F_OK) != 0)
            return report_skip("vduse module not available");
        if (::system("which vdpa >/dev/null 2>&1") != 0)
            return report_skip("iproute2's vdpa tool not available");
        ctl = new_vduse_controller(SUITE_LOCKS);
        ASSERT_NE(nullptr, ctl);
        sweep();
        ASSERT_EQ(0, img.create(IMG_PATH, IMG_SIZE));
        file = img.file;
    }

    void TearDown() override {
        img.release();
        sweep();
        delete ctl;
        ctl = nullptr;
    }

    // Remove leftovers of crashed runs, then adopt+shutdown every orphan
    // registration. Owns its controller rather than using ctl: a skipped SetUp
    // returns before ctl exists, and TearDown still runs.
    //
    // Consumers are removed per registration, from inside the loop below and through
    // vdpa_detach() -- never by a blanket `vdpa dev del` ahead of the loop, which is
    // what this function used to open with. That ordering was wrong and its own header
    // said so: detaching a consumer before anything serves its registration blocks the
    // removal in the kernel's queue freeze while holding the machine-wide genl_lock.
    // Per registration loses no coverage, because a consumer can only exist for a
    // registration this loop visits -- the tombstone is planted before CREATE_DEV, and
    // the only thing that unlinks it is destroy_orphan(), which unlinks the registration
    // with it. And vdpa_detach() carries the precondition itself, so the hazardous case
    // is SERVED before it is removed rather than merely routed around here.
    //
    // Recovery from a wedge is to serve the registration again, not to kill anything:
    // a task in D state ignores SIGKILL, and serving it is what lets the removal
    // finish. The finish is not instantaneous, so a census taken the moment the server
    // exits can still show D -- reading that as failure is what prompts a second
    // detach on top of the first.
    //
    // The adopt is how this sweep normally destroys a registration: rollback()
    // destroys only what it created, and a failed start clears the state the
    // destructor keys on -- so when the adopt fails, the registration survives
    // this sweep and every later one, and each later start() on that name dies
    // the same way. destroy_orphan() below is the second way, and the one that
    // works on exactly the orphans the adopt cannot take.
    void sweep() {
        auto c = new_vduse_controller(SUITE_LOCKS);   // the same scope as ctl
        if (!c)
            return;
        DEFER(delete c);
        for (auto& rec : c->list_orphans()) {
            if (rec.identity.rfind(NAME_PREFIX, 0) != 0)
                continue;   // never touch devices we did not create
            vdpa_detach(rec.identity.c_str());
            BlkConfig cfg;
            cfg.info.identity = rec.identity;
            // The recorded capacity when the tombstone carries one: start() refuses
            // an adoption that disagrees with it, so a fixed size here would strand
            // every orphan this suite created, and a stranded vduse registration is
            // what wedges the machine. 4 MiB when it carries none, which is what
            // raw_vduse_create's registrations look like until a start() claims one
            // -- and it is the capacity that helper declares, so a sweep adopting
            // one serves the size the registration was built with.
            cfg.info.size = rec.size ? rec.size : (4 << 20);
            auto d = c->new_device(cfg);
            if (!d)
                continue;
            auto f = lfs_open_dummy();
            if (d->start(f, true) == 0)
                d->shutdown();
            else
                delete f;
            delete d;
            // The adopt is not always available. Counting an orphan's queues goes
            // through VDUSE_VQ_GET_INFO, and on this registration -- one whose
            // consumer had been removed, and whose removal had finished -- that
            // ioctl answered EPERM instead of the EINVAL the walk reads as "no such
            // index", so the adopt gave up and left the registration standing. Why it
            // answered EPERM is NOT established: a registration that was never bound
            // answers rc=0 for an in-range index, so being unbound is not by itself
            // the trigger, and the state this one had been through (bound, wedged
            // mid-removal, then drained) is not one the walk was measured against.
            // destroy_orphan() asks no ioctl of the queues, so it does not depend on
            // the answer. Ordered AFTER the adopt rather than instead of it: an
            // orphan whose consumer is still mid-removal can only be drained by
            // serving it, and the adopt is what serves it.
            std::string reg = std::string("/dev/vduse/") + rec.identity;
            if (::access(reg.c_str(), F_OK) == 0)
                c->destroy_orphan(rec);
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
                  bool verify_backend, uint64_t off = IO_OFF, bool read_only = false) {
        test::DeviceIoOpts o;
        o.backend = verify_backend ? file : nullptr;
        o.read_only = read_only;
        return test::device_io(node, wbuf.data(), wbuf.size(), off, o);
    }

    std::vector<char> pattern(uint8_t seed, size_t n = IO_LEN) { return test::pattern(seed, n); }
};

TEST_F(VduseConfigTest, config_validation) {
    // the pure config checks are construction-time now: no object at all
    BlkConfig bad(make_info());
    bad.info.identity = "";
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    bad = BlkConfig(make_info());
    bad.info.identity = "has/slash";
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    bad = BlkConfig(make_info());
    bad.info.size = 0;
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    bad = BlkConfig(make_info());
    bad.info.size = 4097 * 512 + 1;   // not a multiple of 512 (the capacity unit)
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    // A null backend is start()'s to reject: it is not part of the config. The
    // rejection is the second statement of start(), ahead of the control device
    // being opened, so it needs no kernel -- and it leaves nothing behind, since
    // `backend` is only assigned once the argument is known good.
    BlkConfig good(make_info());
    auto dev = ctl->new_device(good);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(nullptr));
    EXPECT_EQ(EINVAL, errno);
}

// The two refusals that do need a registration to refuse against, which is why they
// stayed behind the gate when the construction-time half above moved out.
TEST_F(VduseTest, start_refusals_ealready_and_ebusy) {
    if (skip_reason) return;
    BlkConfig good(make_info());
    auto dev = ctl->new_device(good);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    // a second start on the same object must be EALREADY
    ASSERT_EQ(0, dev->start(file));
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EALREADY, errno);
    ASSERT_EQ(0, dev->shutdown());

    // a live server holds the identity: a second object's start must be EBUSY.
    // The tombstone refuses it now, before the single-opener char dev is even
    // reached -- same errno, one step earlier.
    ASSERT_EQ(0, dev->start(file));
    auto dev2 = ctl->new_device(good);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    errno = 0;
    EXPECT_EQ(-1, dev2->start(file));
    EXPECT_EQ(EBUSY, errno);
    EXPECT_EQ(0, dev->shutdown());
}

// The capability half of BlkDevInfo: what lets a caller branch on behaviour instead of
// inferring it from which factory built the object. Every axis pinned here is a property
// of the transport, so all of them are already correct on a constructed device, before
// anything touches the kernel.
TEST_F(VduseConfigTest, capabilities_descriptor) {
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    const BlkDevInfo& i = dev->get_info();

    // The ring lives in the kernel's vduse device and outlives this process, so a later
    // start() has something to harvest. DESTROY_DEV answers EBUSY on a bound device
    // rather than tearing it down. INJECT_CONFIG_IRQ failing only logs, so a successful
    // resize() does not mean the guest was told. Adoption asks the registration for its
    // queue count and nothing else about its CONFIG -- the uapi has no config readback --
    // and the grade stays at that even though start() also refuses a capacity its own
    // tombstone record disagrees with, because a record can be missing and then the
    // geometry goes unverified. The registered feature set is never checked. start() does
    // read the ring's cursors before adopting, but those are handover state rather than
    // configuration, so they do not raise the grade either.
    EXPECT_EQ(BlkBacklog::KernelSide, i.backlog);
    EXPECT_EQ(BlkShutdownRefusal::RefusesWhenAttached, i.shutdown_refusal);
    EXPECT_EQ(BlkResizeEffect::BestEffortNotify, i.resize_effect);
    EXPECT_EQ(BlkAdoption::QueueCountOnly, i.adoption);
    // stop_serving() drains the requests it already dispatched on BOTH paths; only the
    // avail backlog is what wait_pending controls
    EXPECT_EQ(false, i.detach_no_wait);
    // Nothing has settled yet, and a fresh start() does not change that: the word the
    // adoption resync reads is only negotiated once FEATURES_OK is set, and no ioctl
    // says whether it is, so the descriptor stays at "not witnessed" rather than guess.
    EXPECT_EQ(0ull, i.negotiated);
}

// The second half of that claim, and the half that makes the publish mapping
// testable: start() with no guest attached still reports 0. It cannot be witnessed
// the other way round, because a real guest ALWAYS negotiates FLUSH here -- it is
// the only feature this transport offers -- so a writer that published FLUSH
// unconditionally would be indistinguishable after an attach. The reset is the one
// moment where the correct value and a stuck-on value differ.
TEST_F(VduseTest, negotiated_stays_unwitnessed_until_a_guest_negotiates) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    EXPECT_EQ(0ull, dev->get_info().negotiated);
}

// The requested/offered split. This transport accepts DISCARD and WRITE_ZEROES in a
// config and serves neither; before the split a caller could only learn that by reading
// the constructor, and now it is arithmetic on the descriptor. All of it is arithmetic
// on a constructed device, so it belongs to the ungated fixture.
TEST_F(VduseConfigTest, offered_exposes_requests_the_transport_cannot_honour) {
    BlkConfig cfg(make_info());
    cfg.info.features = FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
    auto dev = ctl->new_device(cfg);
    // accepted: validate_info checks geometry, not the feature set
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    const BlkDevInfo& i = dev->get_info();
    EXPECT_EQ(FEATURE_FLUSH, i.offered);
    EXPECT_EQ(cfg.info.features, i.features);   // the request is recorded, not rewritten
    EXPECT_EQ(FEATURE_DISCARD | FEATURE_WRITE_ZEROES, i.features & ~i.offered);
}

// blk.h's start() contract, the half test::CountingFile exists to witness: an OWNED
// backend is deleted on shutdown, not only by the destructor. Both shutdowns must
// succeed for the release to be owed -- vduse's propagates a DESTROY_DEV failure, and
// a registration that survives is one an adopter can re-serve, so it keeps its backend.
TEST_F(VduseTest, shutdown_releases_a_backend_it_owns) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
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
}

TEST_F(VduseTest, basic_io) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());          // fires LAST (declared first)
    DEFER(vdpa_detach(TEST_NAME));   // consumer off BEFORE the daemon: an
                                     // unserved device wedges its users in D state
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    EXPECT_EQ(0, device_io(node, pattern(0x5a), true));
    EXPECT_EQ(0, device_io(node, pattern(0xa5), true, IMG_SIZE - IO_OFF - IO_LEN));
}

// #208: what the guest reads back as this disk's serial has to identify THIS
// registration. A fixed per-transport string made every device one daemon served
// report the same serial to its guest, and that serial is what a guest uses to tell
// two disks apart. The identity is spelled out rather than read from
// make_info().identity, and the hash is recomputed here from the two published
// constants rather than called out of the engine -- the shape the vhost-user suite's
// case of the same name uses -- so a change to the engine's rule shows up here as a
// mismatch instead of being mirrored. The digest this produced was corroborated
// against a real guest readback of /sys/block/vdX/serial (72c5b4683ab92eae), so the
// rule is not resting only on this file's copy of it. What the fixed-width fill itself
// does -- all 20 bytes written, NUL past the end of the serial -- is witnessed against
// the shared engine in test-blk-vq.cpp, so this case is about the value the transport
// hands it.
TEST_F(VduseTest, get_id_reports_this_device_not_the_transport) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());          // fires LAST (declared first)
    DEFER(vdpa_detach(TEST_NAME));   // consumer off BEFORE the daemon: an
                                     // unserved device wedges its users in D state
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;

    // GET_ID answers with an FNV-1a hash of the identity, not the identity: a copy
    // truncated to the field's 20 bytes would give two devices whose identities share
    // a prefix the same serial, which is the collision the hash exists to prevent.
    char want[20] = {};
    {
        uint64_t h = 14695981039346656037ULL;   // FNV offset basis
        for (const char* p = "photon-vduse-test"; *p; p++) {
            h ^= (uint8_t)*p;
            h *= 1099511628211ULL;              // FNV prime
        }
        snprintf(want, sizeof(want), "%016llx", (unsigned long long)h);
    }
    std::string serial;
    ASSERT_TRUE(sysfs_serial(kname, &serial)) << "no serial attribute under " << kname;
    EXPECT_EQ(std::string(want), serial);
}

// The engine suite pins what write_through does and the vhost-user suite pins where
// it comes from on that transport. This pair pins the same derivation here, which
// has two sites of its own -- the FEATURES_OK handler and the adoption resync, the
// second resting on an argument the first does not need -- and neither is reachable
// from a socket test.
//
// The oracle is the stress path and not device_io, and the reason is worth keeping
// because it cost a red run to find: device_io's consumer child fsyncs the node
// after every write, so on a device that negotiated FLUSH that fsync arrives as a
// flush request and the daemon's sync count is 1 either way. The count measured a
// sync, just not the one the assertion claimed. StressCfg::flush is false by
// default, so the stress child writes without ever asking for persistence and every
// sync the daemon performs is one it decided to perform.
//
// This is the control half: FLUSH is offered, the driver negotiates it, and the
// device stays in write-back.
TEST_F(VduseTest, write_back_when_flush_is_negotiated) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    test::BackendProbe probe(file);   // declared before dev: it does not own what it
                                      // wraps, and the device's last IO must land
                                      // while it is alive
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());          // fires LAST (declared first)
    DEFER(vdpa_detach(TEST_NAME));   // consumer off BEFORE the daemon: an
                                     // unserved device wedges its users in D state
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());

    test::StressCfg sc;
    sc.node = node;
    sc.size = IMG_SIZE;
    sc.threads = 2;
    sc.iters = 4;
    test::StressResult sr = test::stress_off_vcpu(sc);
    // the success counter first: a phase that wrote nothing would satisfy every
    // count below by doing nothing at all
    ASSERT_GT(sr.ios, 0u) << sr.first_error;
    ASSERT_EQ(0, sr.failures) << sr.first_error;
    EXPECT_GT(probe.writes.load(), 0);
    EXPECT_EQ(0, probe.datasyncs.load());
    EXPECT_EQ(0, probe.syncs.load());
}

// The experiment half: FLUSH is not offered, so no driver can negotiate it, and a
// driver with no flush command has no way to ask for persistence. Every write has to
// be durable before its completion, which makes the two counts equal.
TEST_F(VduseTest, write_through_when_flush_is_not_offered) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    cfg.info.features = 0;   // nothing offered, so nothing for a driver to accept
    test::BackendProbe probe(file);
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());          // fires LAST (declared first)
    DEFER(vdpa_detach(TEST_NAME));   // consumer off BEFORE the daemon: an
                                     // unserved device wedges its users in D state
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());

    test::StressCfg sc;
    sc.node = node;
    sc.size = IMG_SIZE;
    sc.threads = 2;
    sc.iters = 4;
    test::StressResult sr = test::stress_off_vcpu(sc);
    ASSERT_GT(sr.ios, 0u) << sr.first_error;
    ASSERT_EQ(0, sr.failures) << sr.first_error;
    // one persist per write that reached the backend, and nothing asked for any of
    // them: the child never fsyncs, and no flush command was negotiable
    EXPECT_GT(probe.writes.load(), 0);
    EXPECT_EQ(probe.writes.load(), probe.datasyncs.load());
    EXPECT_EQ(0, probe.syncs.load());
}

// High-concurrency stress through the vdpa/virtio-blk driver: many O_DIRECT
// threads keep the single virtqueue full, the driver splits the larger blocks
// into multi-descriptor chains, and every completion comes back through the
// daemon's bounce mappings. The self-describing blocks (harness.h) attribute
// any misrouted, torn or lost IO; DISJOINT additionally requires each block to
// carry its reader's own tid+seq.
TEST_F(VduseTest, concurrent_stress) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());          // fires LAST (declared first)
    DEFER(vdpa_detach(TEST_NAME));   // consumer off BEFORE the daemon: an
                                     // unserved device wedges its users in D state
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());

    EXPECT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "vduse"));
}

TEST_F(VduseTest, read_only) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    cfg.read_only = true;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    DEFER(vdpa_detach(TEST_NAME));
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());

    // VIRTIO_BLK_F_RO makes the gendisk read-only: reads work, writes fail at
    // the block layer (EPERM), and open(O_RDWR) still succeeds (the ublk/tcmu
    // verified semantics)
    EXPECT_EQ(0, device_io(node, pattern(0x33), false, IO_OFF, /*read_only=*/true));
    EXPECT_EQ(0, test::expect_write_rejected(node, IO_OFF, 4096));
}

TEST_F(VduseTest, resize_dev) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    DEFER(vdpa_detach(TEST_NAME));
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());

    constexpr uint64_t NEW_SIZE = 96ull << 20;
    ASSERT_EQ(0, file->ftruncate(NEW_SIZE));
    ASSERT_EQ(0, dev->resize(NEW_SIZE));

    // the config IRQ makes the driver revalidate the capacity (asynchronously)
    uint64_t sz = 0;
    test::run_off_vcpu([&] {
        for (int i = 0; i < 100 && sz != NEW_SIZE; i++) {
            int fd = ::open(node.c_str(), O_RDONLY);
            if (fd >= 0) {
                if (::ioctl(fd, BLKGETSIZE64, &sz) < 0)
                    sz = 0;
                ::close(fd);
            }
            if (sz != NEW_SIZE)
                ::usleep(50 * 1000);
        }
    });
    EXPECT_EQ(NEW_SIZE, sz);

    // IO past the old size now works
    EXPECT_EQ(0, device_io(node, pattern(0x66), true, IMG_SIZE + IO_OFF));

    // shrink is rejected
    errno = 0;
    EXPECT_EQ(-1, dev->resize(IMG_SIZE));
    EXPECT_EQ(EINVAL, errno);
}

TEST_F(VduseTest, shutdown_busy) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    // Registered BEFORE the attach, not after: vdpa_attach's timeout branch
    // returns "" with the consumer already added, so the ASSERT below is itself
    // inside the window. Firing twice is harmless -- see sweep()'s header.
    DEFER(vdpa_detach(TEST_NAME));
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());

    // the vdpa consumer holds the registration: DESTROY_DEV must EBUSY with
    // nothing torn down
    errno = 0;
    EXPECT_EQ(-1, dev->shutdown());
    EXPECT_EQ(EBUSY, errno);
    EXPECT_EQ(0, ::access(("/dev/vduse/" + std::string(TEST_NAME)).c_str(), F_OK));
    EXPECT_EQ(0, device_io(node, pattern(0x77), true));   // still serving

    // The state this refusal leaves is one of the two a failed teardown has to
    // define, and it is the one where nothing was torn down: the object is still
    // serving, so it still says so and a start() must refuse. That is the mirror
    // of a DESTROY_DEV that fails AFTER serving stopped, where serving cannot be
    // put back and the same start() must instead be free to adopt the
    // registration and try again. Asserting the refusal here is what keeps the
    // two outcomes from being collapsed into one behaviour.
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EALREADY, errno);
    EXPECT_EQ(0, device_io(node, pattern(0x77), true));   // and still serving after

    vdpa_detach(TEST_NAME);   // consumer goes first, while we still serve
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(("/dev/vduse/" + std::string(TEST_NAME)).c_str(), F_OK));
}

TEST_F(VduseTest, orphan_recovery) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev1 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev1);
    DEFER(delete dev1);
    ASSERT_EQ(0, dev1->start(file));
    // Registered BEFORE the attach, not after: vdpa_attach's timeout branch
    // returns "" with the consumer already added, so the ASSERT below is itself
    // inside the window. Firing twice is harmless -- see sweep()'s header.
    DEFER(vdpa_detach(TEST_NAME));
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    ASSERT_EQ(0, device_io(node, pattern(0x11), true));

    // daemon goes away, consumer STAYS attached: the registration is an orphan
    // and the device's users would wedge on new IO until adoption
    ASSERT_EQ(0, dev1->detach(false));

    bool found = false;
    for (auto& rec : ctl->list_orphans())
        if (rec.identity == TEST_NAME)
            found = true;
    ASSERT_TRUE(found);

    // A fresh object adopts it by name and resumes from used->idx; the consumer never
    // noticed, and its backlog gets served. The combination this pins is the one
    // detach(false) actually produces: the previous daemon's cursor equals used->idx --
    // it completed everything it dispatched -- while avail->idx has moved past it,
    // because the attached consumer kept submitting into a ring nobody was reading.
    // Measured 2026-10-05 as cursor 75, used idx 75, avail idx 76: one un-fetched entry,
    // which is fresh work for the adopter rather than lost work. Requiring the two
    // indices to be equal here would make adoption impossible against any consumer still
    // submitting, which is the only kind that needs recovering.
    BlkConfig cfg2(make_info());
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    DEFER(dev2->shutdown());
    DEFER(vdpa_detach(TEST_NAME));   // fires BEFORE dev2->shutdown: the consumer
                                     // must go while the daemon still serves
    ASSERT_EQ(0, dev2->start(file));
    EXPECT_EQ(0, device_io(node, pattern(0x22), true));   // write+read round trip
    std::vector<char> rbuf(IO_LEN);
    int rc = -1;
    test::run_off_vcpu([&] {
        int fd = ::open(node.c_str(), O_RDONLY);
        if (fd < 0) { rc = errno ? errno : EIO; return; }
        DEFER(::close(fd));
        rc = ::pread(fd, rbuf.data(), rbuf.size(), IO_OFF) == (ssize_t)rbuf.size() ? 0 : EIO;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0, memcmp(pattern(0x22).data(), rbuf.data(), IO_LEN));
}

// Start, then detach so the daemon is gone but the registration stays -- that is the
// orphan -- with no vdpa consumer attached, so nothing refuses the removal.
TEST_F(VduseTest, destroy_orphan_removes_a_dead_registration) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    ASSERT_EQ(0, dev->detach(false));
    std::string reg = std::string("/dev/vduse/") + TEST_NAME;
    std::string lp = std::string(SUITE_LOCKS) + "/vduse-" + TEST_NAME + ".lock";
    ASSERT_EQ(0, ::access(reg.c_str(), F_OK));
    ASSERT_EQ(0, ::access(lp.c_str(), F_OK));

    BlkDevInfo rec;
    bool found = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == TEST_NAME) { rec = i; found = true; }
    ASSERT_TRUE(found) << "the detached device was not reported as an orphan";

    // Bracketing the call, so the pair covers exactly the descriptors it takes: the
    // claim, the probing open of the single-opener char device, and the control
    // channel it opens and hands the DESTROY_DEV to.
    int fds_before = fd_count();
    ASSERT_LT(0, fds_before);
    errno = 0;
    int rc = ctl->destroy_orphan(rec);
    int e = errno;
    EXPECT_EQ(0, rc) << "errno " << e;
    EXPECT_EQ(fds_before, fd_count()) << "destroy_orphan kept a descriptor";

    // Both halves, asserted separately and BEFORE the device's destructor runs:
    // shutdown() re-claims the tombstone on its way out, which re-creates the file.
    EXPECT_NE(0, ::access(reg.c_str(), F_OK)) << "the registration survived";
    EXPECT_NE(0, ::access(lp.c_str(), F_OK)) << "the tombstone survived";
    for (auto& i : ctl->list_orphans())
        EXPECT_NE(std::string(TEST_NAME), i.identity);
}

TEST_F(VduseTest, destroy_orphan_refuses_a_live_device) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    DEFER(dev->shutdown());
    ASSERT_EQ(0, dev->start(file));
    std::string reg = std::string("/dev/vduse/") + TEST_NAME;
    std::string lp = std::string(SUITE_LOCKS) + "/vduse-" + TEST_NAME + ".lock";

    // live, so this server holds the tombstone and the scan does not report it
    for (auto& i : ctl->list_orphans())
        EXPECT_NE(std::string(TEST_NAME), i.identity);

    // A record from an EARLIER scan: it can predate this server taking the name over,
    // which is the window the claim closes. Without it, DESTROY_DEV would tear down a
    // device that is being served right now.
    BlkDevInfo stale;
    stale.identity = TEST_NAME;
    errno = 0;
    int rc = ctl->destroy_orphan(stale);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EBUSY, e) << "a live device must be refused, not destroyed";
    EXPECT_EQ(0, ::access(reg.c_str(), F_OK)) << "a live device's registration was destroyed";
    EXPECT_STREQ("HELD", lock_state(lp)) << "the refusal did not leave the server's own claim in place";

    // nothing was torn down, and the proof is that it still works: a real round trip
    // through a consumer attached after the refusal
    DEFER(vdpa_detach(TEST_NAME));
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    EXPECT_EQ(0, device_io(node, pattern(0x55), true));
}

// The refusal that is specific to vduse: a consumer still attached to a registration
// whose daemon is gone. Tearing that down would leave the consumer's users wedged on
// a device nothing serves, so the call has to say no and leave it recoverable.
//
// THE CLEANUP ORDER BELOW IS LOAD-BEARING. `vdpa dev del` against a registration
// nothing serves does not fail -- it wedges in D state, because removing the disk has
// to drain IO and then reset the device, and both need an answer only a daemon can
// give. Measured twice, and it blocks at one point or the other: once waiting for the
// block queue to freeze, once later waiting for the reset to be acknowledged. A task
// in D state ignores SIGKILL, so nothing outside can undo it; what does undo it is the
// same cause run in reverse. Both times, an adopter that served the registration
// cleared it -- the first wedge had stood for 19 minutes, then one sweep adopted and
// served it, resuming an avail entry it found waiting, and the disk went with both
// wedged tasks after it.
//
// The clearing is NOT instantaneous, and mistaking that for failure is how a second
// `vdpa dev del` gets issued on top of the first. A census taken the moment the
// adopter exits can still show the task in D state; it was gone by the next look, with
// no further help. So: serve it, then wait and look again before concluding anything.
//
// Hence the consumer comes off WHILE an adopter serves, exactly the ordering
// orphan_recovery documents from the other side, and never while the device is an
// orphan.
TEST_F(VduseTest, destroy_orphan_refuses_an_attached_consumer) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    // Registered BEFORE the attach, not after: vdpa_attach's timeout branch returns ""
    // with the consumer already added, so the ASSERT below is itself inside the
    // window. Firing twice is harmless -- see sweep()'s header.
    DEFER(vdpa_detach(TEST_NAME));
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    // daemon goes away, consumer STAYS attached
    ASSERT_EQ(0, dev->detach(false));

    std::string reg = std::string("/dev/vduse/") + TEST_NAME;
    std::string lp = std::string(SUITE_LOCKS) + "/vduse-" + TEST_NAME + ".lock";
    std::string sp = std::string("/sys/bus/vdpa/devices/") + TEST_NAME;
    // The precondition, measured rather than assumed: this sysfs entry is what the
    // refusal keys on, so if it were absent the case would go green having tested
    // nothing but the happy path.
    ASSERT_EQ(0, ::access(sp.c_str(), F_OK))
        << "the consumer is not attached, so nothing below is about a consumer";

    BlkDevInfo rec;
    bool found = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == TEST_NAME) { rec = i; found = true; }
    ASSERT_TRUE(found) << "the detached device was not reported as an orphan";

    errno = 0;
    int rc = ctl->destroy_orphan(rec);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EBUSY, e) << "a consumer is attached; destroying would strand its users";
    EXPECT_EQ(0, ::access(reg.c_str(), F_OK)) << "the registration was destroyed under a consumer";
    // THE POINT for vduse, and the one ublk has no reachable witness for: this refusal
    // happens AFTER the claim was taken, so the claim has to come back with it. A
    // leaked one is not a lost descriptor but an unreportable device.
    EXPECT_STREQ("not-held", lock_state(lp))
        << "a refused destroy kept the claim, which hides this device from every later scan";
    bool still = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == TEST_NAME) still = true;
    EXPECT_TRUE(still) << "a refused destroy should leave the orphan reportable";

    // Adopt, and only then take the consumer off -- see the header. From here the
    // device is served, so the removal drains instead of wedging.
    BlkConfig cfg2(make_info());
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    DEFER(dev2->shutdown());
    ASSERT_EQ(0, dev2->start(file));
    vdpa_detach(TEST_NAME);
    EXPECT_NE(0, ::access(sp.c_str(), F_OK)) << "the consumer did not go";

    // With the consumer gone and the device detached again, the same call takes both
    // halves -- so what blocked it was the consumer, not the device having become
    // undestroyable.
    ASSERT_EQ(0, dev2->detach(false));
    errno = 0;
    rc = ctl->destroy_orphan(rec);
    e = errno;
    EXPECT_EQ(0, rc) << "errno " << e;
    EXPECT_NE(0, ::access(reg.c_str(), F_OK));
    EXPECT_NE(0, ::access(lp.c_str(), F_OK));
}

TEST_F(VduseTest, destroy_orphan_validates_the_identity) {
    if (skip_reason) return;
    // A real orphan, so the rejections below can be checked against something: what
    // each spelling would have named, had it been let through.
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    ASSERT_EQ(0, dev->detach(false));
    std::string reg = std::string("/dev/vduse/") + TEST_NAME;
    ASSERT_EQ(0, ::access(reg.c_str(), F_OK));

    // Unlike tcmu there is no character mapping here: both the registration path and
    // the tombstone name take the identity VERBATIM. Without the '/' check a name
    // like "a/b" reaches devlock_unlink() as a path with a component in it, and
    // because a missing file is the goal state rather than an error, the call would
    // report SUCCESS about a device it never touched.
    const std::vector<std::pair<std::string, const char*>> bad = {
        {"", "nothing to name"},
        {std::string(300, 'x'), "past the 255-byte device name the uapi allows"},
        {"a/b", "verbatim into a path, so it names a directory that is not this one"},
        {".", "resolves to /dev/vduse itself, which exists"},
        {"..", "resolves to /dev, which exists"},
    };
    int refused = 0;
    for (auto& b : bad) {
        BlkDevInfo d;
        d.identity = b.first;
        errno = 0;
        int rc = ctl->destroy_orphan(d);
        int e = errno;
        EXPECT_EQ(-1, rc) << "identity [" << b.first << "]: " << b.second;
        EXPECT_EQ(EINVAL, e) << "identity [" << b.first << "]: " << b.second;
        if (rc == -1 && e == EINVAL)
            refused++;
    }
    EXPECT_EQ((int)bad.size(), refused) << "not every unusable identity was refused";
    // the two directories those spellings resolve to are untouched, and so is the
    // orphan this case fabricated
    EXPECT_EQ(0, ::access("/dev/vduse", F_OK));
    EXPECT_EQ(0, ::access("/dev", F_OK));
    EXPECT_EQ(0, ::access(reg.c_str(), F_OK)) << "a rejected identity destroyed the real device";
    bool still = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == TEST_NAME) still = true;
    EXPECT_TRUE(still) << "the orphan stopped being reported after a refused destroy";

    // A name that was never a device is ENOENT, not EINVAL and not a silent 0: the
    // caller asked about something and deserves to be told it is not there.
    BlkDevInfo none;
    none.identity = "photon-vduse-never-existed";
    ::unlink((std::string(SUITE_LOCKS) + "/vduse-" + none.identity + ".lock").c_str());
    errno = 0;
    EXPECT_EQ(-1, ctl->destroy_orphan(none));
    EXPECT_EQ(ENOENT, errno);

    // And the correct spelling of the same dev still works, so the rejections above
    // are about the spellings and not about the device having become undestroyable.
    BlkDevInfo rec;
    bool found = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == TEST_NAME) { rec = i; found = true; }
    ASSERT_TRUE(found);
    errno = 0;
    int rc = ctl->destroy_orphan(rec);
    int e = errno;
    EXPECT_EQ(0, rc) << "errno " << e;
    EXPECT_NE(0, ::access(reg.c_str(), F_OK));
}

// The two answers devlock_free() gives when there is NO registration for the name,
// and why they differ. A tombstone nobody holds is our own litter and goes with the
// device it outlived. One a LIVE SERVER holds is not this call's to take even though
// there is nothing in the kernel for it to stand for: that server is between claiming
// the name and CREATE_DEV, and unlinking the file would leave the device it then
// creates with no tombstone -- which list_orphans() skips, so it would stop being
// reported by every later scan.
TEST_F(VduseTest, destroy_orphan_leaves_a_claimed_tombstone_alone) {
    if (skip_reason) return;
    const std::string name = "photon-vduse-no-such-dev";
    std::string reg = "/dev/vduse/" + name;
    std::string lp = std::string(SUITE_LOCKS) + "/vduse-" + name + ".lock";
    ASSERT_NE(0, ::access(reg.c_str(), F_OK)) << "this name unexpectedly exists";

    BlkDevInfo rec;
    rec.identity = name;

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
    // success -- so the refusal above was about the claim and not about the name.
    ::flock(fd, LOCK_UN);
    ::close(fd);
    fd = -1;
    errno = 0;
    int rc = ctl->destroy_orphan(rec);
    int e = errno;
    EXPECT_EQ(0, rc) << "errno " << e;
    EXPECT_NE(0, ::access(lp.c_str(), F_OK)) << "a tombstone outliving its device survived";
}

// The claim's other half, and the one the case above cannot reach: here the
// registration DOES exist, so the call gets past the existence probe and the
// tombstone is the only thing between it and a DESTROY_DEV against a device a live
// server owns. Without the claim that destroy succeeds, and the owner goes on
// serving a registration the kernel no longer has.
TEST_F(VduseTest, destroy_orphan_refuses_a_registration_claimed_by_another) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    // Leaves the registration standing and gives the name back: detach() is what
    // releases the tombstone, so from here somebody else can hold it.
    ASSERT_EQ(0, dev->detach(false));
    std::string reg = std::string("/dev/vduse/") + TEST_NAME;
    std::string lp = std::string(SUITE_LOCKS) + "/vduse-" + TEST_NAME + ".lock";
    ASSERT_EQ(0, ::access(reg.c_str(), F_OK));

    BlkDevInfo rec;
    bool found = false;
    for (auto& i : ctl->list_orphans())
        if (i.identity == TEST_NAME) { rec = i; found = true; }
    ASSERT_TRUE(found) << "the detached device was not reported as an orphan";

    int fd = ::open(lp.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    DEFER({ if (fd >= 0) ::close(fd); });
    ASSERT_EQ(0, ::flock(fd, LOCK_EX | LOCK_NB));

    errno = 0;
    EXPECT_EQ(-1, ctl->destroy_orphan(rec));
    EXPECT_EQ(EBUSY, errno) << "a live server holds the claim on a device that exists";
    EXPECT_EQ(0, ::access(reg.c_str(), F_OK)) << "the registration went under its owner";

    // Let go and the identical call succeeds, so the refusal was about the claim.
    ::flock(fd, LOCK_UN);
    ::close(fd);
    fd = -1;
    errno = 0;
    int rc = ctl->destroy_orphan(rec);
    int e = errno;
    EXPECT_EQ(0, rc) << "errno " << e;
    EXPECT_NE(0, ::access(reg.c_str(), F_OK));
    EXPECT_NE(0, ::access(lp.c_str(), F_OK));
}

TEST_F(VduseTest, daemon_restart_io) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    auto dev1 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev1);
    DEFER(delete dev1);
    ASSERT_EQ(0, dev1->start(file));
    DEFER(vdpa_detach(TEST_NAME));
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());

    // a continuous writer across the daemon handover: its IO blocks in the
    // kernel during the gap and completes after the adoption -- no errors
    test::BackgroundWriter w;
    ASSERT_EQ(0, w.start(node, {IO_OFF, IMG_SIZE - IO_OFF, 64 << 10}));
    DEFER(w.stop());   // safety net; the explicit stop below is what normally runs

    photon::thread_usleep(100 * 1000);
    ASSERT_EQ(0, dev1->detach(false));   // the writer's in-flight IO now blocks
    BlkConfig cfg2(make_info());
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    DEFER(dev2->shutdown());
    DEFER(vdpa_detach(TEST_NAME));   // fires BEFORE dev2->shutdown (the consumer
                                     // goes while the daemon still serves); the
                                     // early-registered duplicate is the safety
                                     // net for ASSERT failures before this point
    ASSERT_EQ(0, dev2->start(file));   // adoption: the backlog resumes
    photon::thread_usleep(200 * 1000);
    EXPECT_EQ(0, w.errors());
    // stop the writer BEFORE the DEFERs fire: it holds the device open, and
    // the consumer detach + shutdown must not race a live writer
    w.stop();
}

// BlkConfig::queues decides how many virtqueues a vduse device serves, and the
// count is observable without trusting our own config: the kernel creates one
// directory per hardware queue under /sys/block/<node>/mq/ -- the node being
// the one the vdpa binding produced -- and it records the negotiated
// VIRTIO_BLK_F_MQ in the virtio device's sysfs. Both are the kernel's own view
// and they come from different wire fields (config-space num_queues vs the
// feature word), so together they are an independent oracle for fill_config's
// num_queues and for the F_MQ gate. They are not one for CREATE_DEV's vq_num:
// the consumer never reads that field back, and a vq_num LARGER than the queue
// count we serve is invisible in both attributes. An under-declared vq_num is
// caught, but through a different channel -- the kernel rejects a VQ_SETUP for
// an index it was never told about with EINVAL, which fails start(). The table
// pins the `nqueues >= 2` gate at its exact boundary (2), mirroring what
// test-vhost-user's queue_count_follows_config pins against its mock frontend.
TEST_F(VduseTest, queue_count_follows_config) {
    if (skip_reason) return;
    struct Case { uint32_t ask; int dirs; int mq; };
    static const Case cases[] = {
        {0,                   1,                    0},   // the default: one queue, no F_MQ
        {1,                   1,                    0},   // one queue must NOT offer F_MQ
        {2,                   2,                    1},   // the exact F_MQ boundary
        {4,                   4,                    1},
        {PEER_MAX_QUEUES + 5, (int)PEER_MAX_QUEUES, 1},   // clamped, not rejected
    };
    // The consumer driver caps the hardware queues it builds at a CPU count.
    // Exactly one measurement of that cap exists, taken on this suite's VM: 64
    // asked, 8 directories on 8 CPUs. That host reports the same CPU set for
    // possible, present and online, so the data point cannot say which of the
    // three the cap is, and an equality derived from it would go red -- against
    // a correct implementation -- on a host where they differ. Nor is the
    // clamp's exact value observable at all while CPUs are fewer than the
    // clamp: 64 and 69 both come back as the CPU count. The four rows at or
    // below four queues depend on none of this (four is below any plausible CPU
    // count) and stay equalities; the over-large row asserts a range instead.
    // What that row really pins is that an over-large request is served rather
    // than rejected and still offers F_MQ. Of its two directory bounds, only
    // the lower one can fire here: a clamp tightened below the CPU count comes
    // back short, while a clamp widened past it is masked by the same cap that
    // hides the exact count.
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    for (const auto& c : cases) {
        BlkConfig cfg(make_info());
        cfg.queues = c.ask;
        auto dev = ctl->new_device(cfg);
        ASSERT_NE(nullptr, dev) << "queues=" << c.ask;
        DEFER(delete dev);
        ASSERT_EQ(0, dev->start(file)) << "queues=" << c.ask;
        DEFER(dev->shutdown());          // fires after the detach
        DEFER(vdpa_detach(TEST_NAME));   // consumer off BEFORE the daemon: an
                                         // unserved device wedges its users in D state
        std::string node = vdpa_attach(TEST_NAME);
        ASSERT_FALSE(node.empty()) << "queues=" << c.ask;

        // count_mq_dirs and virtio_feature_bit take the bare kernel name; the
        // node we got back is a /dev path
        std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;
        int dirs = test::count_mq_dirs(kname);
        int want_dirs = cpus > 0 && c.dirs > cpus ? (int)cpus : c.dirs;
        if (c.ask > PEER_MAX_QUEUES) {
            // The over-large row: served rather than rejected, and no wider
            // than the clamp we publish. The lower bound is cap-robust rather
            // than exact -- online is a subset of present and of possible, so
            // whichever of the three the driver caps at, it cannot build FEWER
            // than min(clamped, online) -- but it is not vacuous: a clamp
            // lowered below the CPU count comes back short and goes red. The
            // exact count stays unobservable for the reason above.
            EXPECT_GE(dirs, want_dirs) << "mq dirs, queues=" << c.ask;
            EXPECT_LE(dirs, c.dirs) << "mq dirs, queues=" << c.ask;
        } else {
            EXPECT_EQ(want_dirs, dirs) << "mq dirs, queues=" << c.ask;
        }
        EXPECT_EQ(c.mq, virtio_feature_bit(kname, FEAT_BIT_BLK_MQ)) << "F_MQ, queues=" << c.ask;
    }
}

// Adoption is the only ring refresh queues 1..n-1 ever get when the daemon
// hands over while the consumer stays bound: the kernel replays no SET_STATUS
// across the handover, so nothing re-arms needs_refresh and start()'s
// per-queue vq_refresh is the whole resync. A queue that resync skips keeps
// its desc/avail/used unresolved while the kernel -- which was told vq_num --
// keeps putting requests on it. The stress phase after the handover is the
// assertion: it drives the device from enough OS threads that the block layer
// spreads requests over every hardware queue, and an IO that lands on an
// unresolved queue never completes.
TEST_F(VduseTest, adoption_resyncs_every_queue) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    cfg.queues = 4;
    auto dev1 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev1);
    DEFER(delete dev1);
    ASSERT_EQ(0, dev1->start(file));
    // Registered BEFORE the attach, not after: vdpa_attach's timeout branch
    // returns "" with the consumer already added, so the ASSERT below is itself
    // inside the window. Firing twice is harmless -- see sweep()'s header.
    DEFER(vdpa_detach(TEST_NAME));
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    // The premise this test's name asserts: four hardware queues really exist.
    // Without it a mutation that pins nqueues to 1 turns this into a
    // single-queue handover that still passes, and the resync it exists to
    // protect goes unobserved. 4 is below any plausible CPU count, so this does
    // not inherit the table test's per-CPU clamp.
    std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;
    ASSERT_EQ(4, test::count_mq_dirs(kname));
    ASSERT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "vduse-mq-fresh"));

    // daemon goes away, consumer STAYS attached: the rings are orphaned in a
    // live state and dev2's start() must pull every queue's state itself
    ASSERT_EQ(0, dev1->detach(false));

    BlkConfig cfg2(make_info());
    cfg2.queues = 4;
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    DEFER(dev2->shutdown());
    DEFER(vdpa_detach(TEST_NAME));   // fires BEFORE dev2->shutdown: the consumer
                                     // must go while the daemon still serves
    ASSERT_EQ(0, dev2->start(file));   // adoption: resync of EVERY queue
    EXPECT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "vduse-mq-adopted"));
    EXPECT_EQ(0, device_io(node, pattern(0x44), true));
}

// The case above adopts with the SAME cfg.queues the registration was created
// with. This one adopts with a deliberately different one, which is what a
// recovery loop that does not know the geometry does -- and what this fixture's
// own sweep() does on every run, since the BlkConfig it builds sets identity and
// size and never sets `queues`, so it adopts every orphan asking for one queue.
//
// create_dev is the only place a queue count is declared and it does not run on an
// adopt, and DEV_SET_CONFIG's one call site publishes capacity only, so without
// start() asking the kernel for the count the daemon would serve ONE virtqueue
// while the registration has four -- and so does the still-attached consumer,
// whose config-space num_queues came from the daemon that created it and which
// nothing re-published. The consumer keeps spreading requests over all four
// hardware queues, so the stress phase after the handover is the assertion: a
// request that lands on a queue the adopter never set up, resolved or started
// never completes, and the phase reports it as hung rather than done.
TEST_F(VduseTest, adoption_serves_the_registered_queue_count) {
    if (skip_reason) return;
    BlkConfig cfg(make_info());
    cfg.queues = 4;
    auto dev1 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev1);
    DEFER(delete dev1);
    ASSERT_EQ(0, dev1->start(file));
    // Registered BEFORE the attach, not after: vdpa_attach's timeout branch
    // returns "" with the consumer already added, so the ASSERT below is itself
    // inside the window. Firing twice is harmless -- see sweep()'s header.
    DEFER(vdpa_detach(TEST_NAME));
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    // The premise the mismatch is measured against: the consumer really built four
    // hardware queues, so there are three for a one-queue adopter to miss. 4 is
    // below any plausible CPU count, so this does not inherit the table test's
    // per-CPU clamp.
    std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;
    ASSERT_EQ(4, test::count_mq_dirs(kname));
    ASSERT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "vduse-mismatch-fresh"));

    // daemon goes away, consumer STAYS attached: four live rings, nobody serving
    ASSERT_EQ(0, dev1->detach(false));

    // Adopt asking for ONE queue -- what sweep() asks for. The registration's own
    // count has to win.
    BlkConfig cfg2(make_info());
    cfg2.queues = 1;
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    DEFER(dev2->shutdown());
    DEFER(vdpa_detach(TEST_NAME));   // fires BEFORE dev2->shutdown: the consumer
                                     // must go while the daemon still serves
    ASSERT_EQ(0, dev2->start(file));
    // The consumer's view did not change across the handover: still four hardware
    // queues to serve, which is what makes the stress below an oracle at all.
    EXPECT_EQ(4, test::count_mq_dirs(kname));
    EXPECT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "vduse-mismatch-adopted"));
    EXPECT_EQ(0, device_io(node, pattern(0x55), true));
}

// A registration wider than this transport can hold is REFUSED, not clamped:
// serving fewer queues than the kernel declares is the defect the case above
// exists to prevent, and quietly serving the first PEER_MAX_QUEUES of a wider one
// reproduces it with a quieter log. Nothing reachable through blk.h can build such
// a registration -- the create path clamps -- so this one comes from the raw uapi,
// and its name is deliberately OUTSIDE sweep()'s NAME_PREFIX: a registration that
// can only ever be refused must not become something the fixture tries to adopt,
// and fails to clear, on every later run.
//
// This is also the only case that walks the count probe to its cap, so it is what
// pins the walk's termination and its bound: an off-by-one that stops the walk one
// index early turns the refusal into exactly the silent clamp above, and no
// narrower registration can show it.
TEST_F(VduseTest, adoption_refuses_a_registration_wider_than_the_transport) {
    if (skip_reason) return;
    static const char WIDE[] = "vduse-wide-test";
    char reg[64];
    snprintf(reg, sizeof(reg), "/dev/vduse/%s", WIDE);
    // Declared first, so it fires last: whatever the case concludes with, the
    // registration must not outlive it. Tolerant of EINVAL because a start() that
    // SUCCEEDS here is the mutation this case exists to catch, and its destructor
    // then destroys the registration itself.
    DEFER({
        int e = raw_vduse_destroy(WIDE);
        EXPECT_TRUE(e == 0 || e == EINVAL) << "leftover registration, errno " << e;
        EXPECT_NE(0, ::access(reg, F_OK)) << "leftover registration " << reg;
    });
    ASSERT_EQ(0, raw_vduse_create(WIDE, PEER_MAX_QUEUES + 1));
    ASSERT_EQ(0, ::access(reg, F_OK));

    BlkConfig cfg(make_info());
    cfg.info.identity = WIDE;
    cfg.queues = 1;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EINVAL, errno);
    // Refused with nothing torn down: the registration is still there for whoever
    // does own it -- rollback destroys only what WE created -- and our handle on
    // the single-opener char dev is closed, so it is unheld again.
    EXPECT_EQ(0, ::access(reg, F_OK));
    int fd = ::open(reg, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    EXPECT_GE(fd, 0);
    if (fd >= 0)
        ::close(fd);
}

// The size half of what an adoption validates, and the only half vduse can validate
// at all: the uapi writes the device config and has no readback of it, so the
// registration cannot be asked what it serves. Our own tombstone is the record.
//
// Why a mismatch is refused rather than reconciled: the consumer reads the capacity
// out of the REGISTRATION's config space while the daemon bounds every request by
// its OWN, so the two disagreeing means one of them is serving a disk the other does
// not agree about. An adopter that is smaller turns every request past its bound
// into an I/O error the consumer has no way to explain; one that is larger serves
// space the consumer was never told it had. Neither is a warning.
TEST_F(VduseTest, adoption_refuses_a_capacity_drift) {
    if (skip_reason) return;
    const std::string reg = std::string("/dev/vduse/") + TEST_NAME;

    BlkConfig cfg(make_info());
    auto dev1 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev1);
    DEFER(delete dev1);
    ASSERT_EQ(0, dev1->start(file));
    // detach, not shutdown: the registration has to survive for there to be anything
    // to adopt. No consumer is attached, so nothing here can wedge.
    ASSERT_EQ(0, dev1->detach(false));

    BlkConfig small(make_info());
    small.info.size = IMG_SIZE / 2;
    auto dev2 = ctl->new_device(small);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    errno = 0;
    EXPECT_EQ(-1, dev2->start(file));
    EXPECT_EQ(EINVAL, errno);
    // Refused with nothing torn down, as the queue-count refusal above is: rollback
    // destroys only what WE created, so the registration is still there for whoever
    // does own it.
    EXPECT_EQ(0, ::access(reg.c_str(), F_OK));
    // And the refusal left the record ALONE. This is the assertion that keeps the
    // check from being self-erasing: a write placed before the comparison would
    // record the refused size, and every later adopter would then agree with a
    // registration whose capacity it had just overwritten in the only place the fact
    // is kept.
    uint64_t rec = 0;
    EXPECT_TRUE(tombstone_capacity(TEST_NAME, &rec));
    EXPECT_EQ(IMG_SIZE, rec);

    // The agreeing size still adopts, which is what makes the refusal above a
    // comparison rather than "an orphan cannot be adopted at all".
    BlkConfig cfg3(make_info());
    auto dev3 = ctl->new_device(cfg3);
    ASSERT_NE(nullptr, dev3);
    DEFER(delete dev3);
    DEFER(dev3->shutdown());
    ASSERT_EQ(0, dev3->start(file));
}

// No record is NOT a mismatch, and this is the case that pins it. The state is
// reachable without any legacy build: acquire_lock plants the tombstone before
// start() knows whether it is creating or adopting, so a start() that fails after
// that -- the queue-count refusal above is one, and rollback destroys only what it
// created -- leaves a live registration behind an empty file. Reading that file as
// "capacity 0" would refuse every such adoption, and since this fixture's sweep()
// recovers orphans by adopting them, it would strand a registration in the kernel on
// every later run.
TEST_F(VduseTest, adoption_without_a_capacity_record_is_not_a_refusal) {
    if (skip_reason) return;
    const std::string lp = std::string(SUITE_LOCKS) + "/vduse-" + TEST_NAME + ".lock";

    BlkConfig cfg(make_info());
    auto dev1 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev1);
    DEFER(delete dev1);
    ASSERT_EQ(0, dev1->start(file));
    ASSERT_EQ(0, dev1->detach(false));
    uint64_t rec = 0;
    ASSERT_TRUE(tombstone_capacity(TEST_NAME, &rec));   // the premise: a record exists
    ASSERT_EQ(0, ::truncate(lp.c_str(), 0));
    ASSERT_FALSE(tombstone_capacity(TEST_NAME, &rec));  // and now it does not

    BlkConfig small(make_info());
    small.info.size = IMG_SIZE / 2;
    auto dev2 = ctl->new_device(small);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    ASSERT_EQ(0, dev2->start(file));
    DEFER(dev2->shutdown());
    // The adoption wrote the record it found missing, so the NEXT adopter of this
    // registration has something to compare against.
    ASSERT_EQ(0, dev2->detach(false));
    EXPECT_TRUE(tombstone_capacity(TEST_NAME, &rec));
    EXPECT_EQ(IMG_SIZE / 2, rec);
}

// list_orphans() reports the recorded capacity, which is what makes it usable as the
// input to a recovery: start() refuses an adoption that disagrees with the record, so
// a loop that had to invent a size would strand the orphans it could otherwise have
// taken. The 0 it reports for a tombstone with no record is a distinct answer from a
// recorded capacity, and the scan still lists the device -- ownership is the flock,
// not the payload.
TEST_F(VduseTest, list_orphans_reports_the_recorded_capacity) {
    if (skip_reason) return;
    const std::string lp = std::string(SUITE_LOCKS) + "/vduse-" + TEST_NAME + ".lock";
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    ASSERT_EQ(0, dev->detach(false));
    DEFER(dev->shutdown());

    // -1 for "not listed", so the second assertion cannot be satisfied by the device
    // dropping out of the scan instead of reporting 0.
    auto orphan_size = [&]() -> int64_t {
        for (auto& i : ctl->list_orphans())
            if (i.identity == TEST_NAME)
                return (int64_t)i.size;
        return -1;
    };
    EXPECT_EQ((int64_t)IMG_SIZE, orphan_size());
    ASSERT_EQ(0, ::truncate(lp.c_str(), 0));
    EXPECT_EQ(0, orphan_size());
}

// resize() has to move the record with the registration. One that lagged it would
// refuse the only config that describes the device as it now stands -- the drift
// check turned into a way of stranding a resized device.
TEST_F(VduseTest, resize_refreshes_the_capacity_record) {
    if (skip_reason) return;
    constexpr uint64_t NEW_SIZE = 96ull << 20;
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, file->ftruncate(NEW_SIZE));
    ASSERT_EQ(0, dev->start(file));
    // No consumer attached: resize() writes the config space and only logs a failed
    // INJECT_CONFIG_IRQ, so what is under test is the record, not the guest.
    ASSERT_EQ(0, dev->resize(NEW_SIZE));
    ASSERT_EQ(0, dev->detach(false));
    uint64_t rec = 0;
    ASSERT_TRUE(tombstone_capacity(TEST_NAME, &rec));
    EXPECT_EQ(NEW_SIZE, rec);

    // The size the registration was CREATED with is now the drift.
    BlkConfig stale(make_info());
    auto dev2 = ctl->new_device(stale);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    errno = 0;
    EXPECT_EQ(-1, dev2->start(file));
    EXPECT_EQ(EINVAL, errno);

    BlkConfig grown(make_info());
    grown.info.size = NEW_SIZE;
    auto dev3 = ctl->new_device(grown);
    ASSERT_NE(nullptr, dev3);
    DEFER(delete dev3);
    DEFER(dev3->shutdown());
    ASSERT_EQ(0, dev3->start(file));
}

// The tombstone is vduse's ONLY ownership test -- the char device answers "is a
// daemon connected", never "is this ours" -- so it is also the scan's scope: a
// device locked in another dir must be invisible to a SUITE_LOCKS controller, as
// it is for ublk. One controller = one scope, so this test builds a second one.
// sweep() probes SUITE_LOCKS only and can therefore never clean up after this
// test; the shutdown DEFER does.
TEST_F(VduseTest, custom_lock_dir) {
    if (skip_reason) return;
    static const char LOCKS[] = "/tmp/photon-blk-vduse-test-locks";
    static const char NAME[]  = "photon-vduse-locks";
    ::system(("rm -rf " + std::string(LOCKS)).c_str());

    // No default directory to fall back on: a null or empty one is a refusal. What
    // this pins is that the two independent scopes below really are two, rather
    // than one shared default that both controllers happen to name.
    errno = 0;
    EXPECT_EQ(nullptr, new_vduse_controller(nullptr));
    EXPECT_EQ(EINVAL, errno);
    errno = 0;
    EXPECT_EQ(nullptr, new_vduse_controller(""));
    EXPECT_EQ(EINVAL, errno);

    // a stale tombstone in SUITE_LOCKS would fool the invisibility check below
    ::unlink((std::string(SUITE_LOCKS) + "/vduse-" + std::string(NAME) + ".lock").c_str());
    std::string node = std::string("/dev/vduse/") + NAME;
    std::string tomb = std::string(LOCKS) + "/vduse-" + NAME + ".lock";

    auto ctl2 = new_vduse_controller(LOCKS);
    ASSERT_NE(nullptr, ctl2);
    DEFER(delete ctl2);

    BlkConfig cfg(make_info());
    cfg.info.identity = NAME;
    auto dev = ctl2->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    DEFER(dev->shutdown());   // idempotent, and covers an ASSERT failing mid-test

    ASSERT_EQ(0, dev->start(file));   // acquire_lock mkdirs the dir itself
    EXPECT_EQ(0, ::access(tomb.c_str(), F_OK));

    ASSERT_EQ(0, dev->detach(false));   // registration up for adoption, lock free

    // visible through the custom dir, invisible through SUITE_LOCKS
    bool found = false;
    for (auto& i : ctl2->list_orphans())
        if (i.identity == NAME) found = true;
    EXPECT_TRUE(found);
    for (auto& i : ctl->list_orphans())
        EXPECT_NE(std::string(NAME), i.identity);

    // re-claim under the same dir and tear down: the registration must be gone
    ASSERT_EQ(0, dev->start(file));
    ASSERT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(node.c_str(), F_OK));
}

// The observable half of multiqueue: the kernel must see four hardware queues AND
// the IO must actually reach more than one of them, with the pool picking the vcpu
// each queue's serving coroutine runs on. The stress driver is what makes
// vq_refresh's per-queue vi.index and VDUSE_VQ_INJECT_IRQ's per-queue index
// load-bearing -- with vi.index hard-written to 0, every queue but the first never
// learns its own ring address and its IO times out here. Which of the four queues
// get traffic is the block layer's choice, so the placement assertion is a lower
// bound: it takes the same spread over hardware queues that the adoption resync
// case above relies on.
TEST_F(VduseTest, multiqueue_io_spreads_over_the_pool) {
    if (skip_reason) return;
    test::TestPool pool(4);
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    BlkConfig cfg(make_info());
    cfg.queues = 4;
    cfg.pool = pool;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());
    DEFER(vdpa_detach(TEST_NAME));   // consumer off BEFORE the daemon: an
                                     // unserved device wedges its users in D state
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;
    EXPECT_EQ(4, test::count_mq_dirs(kname));
    EXPECT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "vduse mq + pool", 8));

    EXPECT_GE(rec.vcpu_count(), 2u);
    EXPECT_FALSE(rec.ran_on(caller));
}

// Four queues and no pool at all: the kernel must still see four hardware queues,
// and all four serving coroutines must stay on the vcpu that called start().
TEST_F(VduseTest, multiqueue_without_a_pool) {
    if (skip_reason) return;
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    BlkConfig cfg(make_info());
    cfg.queues = 4;
    cfg.pool = nullptr;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());
    DEFER(vdpa_detach(TEST_NAME));   // consumer off BEFORE the daemon: an
                                     // unserved device wedges its users in D state
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;
    EXPECT_EQ(4, test::count_mq_dirs(kname));
    EXPECT_EQ(0, test::stress_node_both_modes(node, IMG_SIZE, "vduse mq, no pool", 8));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(caller));
}

// The handover this suite already trusts for ONE queue (daemon_restart_io), on four
// queues over a pool: a consumer attached, a writer with completed iterations behind
// it, then detach(false) and a second daemon adopting and serving again. What it adds
// is the per-queue teardown loop on a multiqueue device -- the loop whose device-level
// `stopping` assignment C1 moved out of it -- with real in-flight requests to wait for
// and a real run_on_home hop per queue between the serving vcpus.
//
// IT DOES NOT DETECT C1, and the reason is worth stating rather than leaving to be
// found. C1's symptom is that the message loop goes quiet while the queues after the
// first are still draining, and the only thing that notices a quiet message loop is
// the kernel, waiting on a message nobody answers. Witnessing it needs a message to
// arrive inside that window and a wait that outlasts msg_timeout -- and a wait that
// outlasts msg_timeout with a consumer attached is the shape that puts a task in D
// state on this host, which no case may produce. So this asserts the weaker thing that
// IS safe: the multiqueue handover completes, and completes inside the kernel's own
// tolerance, so a teardown that spends even one msg_timeout stalled goes red. The
// ordering itself is pinned separately, by a structural check over stop_serving that
// is proven able to fail on each of the shapes it rejects.
//
// detach(false), not detach(true): vq_backlog_drain re-reads the avail index on every
// pass, so while a writer is still submitting the target keeps moving and the wait has
// no end. daemon_restart_io makes the same choice for the same reason. A quiesced
// consumer would make detach(true) bounded, but with nothing left in the ring its drain
// branch exits on the first test, so it would cover the argument being passed and not
// the wait -- not worth a third daemon.
TEST_F(VduseTest, multiqueue_shutdown_hands_over_inflight_io_promptly) {
    if (skip_reason) return;
    test::TestPool pool(4);
    test::RecordingFile rec(file);

    BlkConfig cfg(make_info());
    cfg.queues = 4;
    cfg.pool = pool;
    auto dev1 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev1);
    DEFER(delete dev1);
    ASSERT_EQ(0, dev1->start(&rec));
    // Read after start(), which is the call that writes it: this is the value the
    // device under test is actually running with, not a constant copied from the
    // default. A kernel without the attribute fails the case instead of substituting
    // one, because a bound nobody measured cannot witness a stall.
    uint64_t bound_us = msg_timeout_us(TEST_NAME);
    ASSERT_NE(0u, bound_us);
    DEFER(vdpa_detach(TEST_NAME));   // registered BEFORE the attach: vdpa_attach's
                                     // timeout branch returns "" with the consumer
                                     // already added, so the ASSERT below is itself
                                     // inside the window
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    std::string kname = node.compare(0, 5, "/dev/") == 0 ? node.substr(5) : node;
    ASSERT_EQ(4, test::count_mq_dirs(kname));

    test::BackgroundWriter w;
    ASSERT_EQ(0, w.start(node, {IO_OFF, IMG_SIZE - IO_OFF, 64 << 10}));
    DEFER(w.stop());   // safety net; the explicit stop below is what normally runs
    // A writer that provably reached the device. Without the iteration count this case
    // could pass with the child never having submitted anything, and a teardown with
    // nothing in flight is not the teardown under test.
    ASSERT_TRUE(w.wait_iters(4));
    uint64_t before = w.iters();

    uint64_t t0 = photon::now;
    ASSERT_EQ(0, dev1->detach(false));
    uint64_t elapsed_us = photon::now - t0;
    LOG_INFO("vduse multiqueue handover across 4 queues: ", VALUE(elapsed_us), VALUE(bound_us));
    EXPECT_LT(elapsed_us, bound_us);

    BlkConfig cfg2(make_info());
    cfg2.queues = 4;
    cfg2.pool = pool;
    auto dev2 = ctl->new_device(cfg2);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    DEFER(dev2->shutdown());
    DEFER(vdpa_detach(TEST_NAME));   // fires BEFORE dev2->shutdown: the consumer goes
                                     // while the daemon still serves
    ASSERT_EQ(0, dev2->start(file));   // adoption: the blocked writes resume
    ASSERT_TRUE(w.wait_iters(before + 4));
    EXPECT_EQ(0, w.errors());
    // stop the writer BEFORE the DEFERs fire: it holds the device open, and the
    // consumer detach + shutdown must not race a live writer
    w.stop();
}

// WorkPool's cursor is `vcpu_index++ % size`, so a zero-size pool is a SIGFPE the
// moment anything asks it for a vcpu; migrate_to_pool's short-circuit is the only
// thing in the way. Surviving is half the assertion, and staying on the caller's
// vcpu is the other half.
TEST_F(VduseTest, empty_pool_falls_back_to_the_caller_vcpu) {
    if (skip_reason) return;
    photon::WorkPool empty(0);   // no vcpus, so no engines to match
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    BlkConfig cfg(make_info());
    cfg.queues = 2;
    cfg.pool = &empty;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());
    DEFER(vdpa_detach(TEST_NAME));   // consumer off BEFORE the daemon: an
                                     // unserved device wedges its users in D state
    std::string node = vdpa_attach(TEST_NAME);
    ASSERT_FALSE(node.empty());
    // the fixture helper verifies through `file`, never through `rec`: a
    // verification read runs on THIS vcpu, so routing it through the probe would
    // record the caller as one of the device's own placements
    EXPECT_EQ(0, device_io(node, pattern(0x33), true));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(caller));
}

// check_pool_engines' integration half: the helper is unit-tested on its own, this
// proves the transport actually asks. A pool whose vcpus cannot host the serving
// coroutines is a configuration error, so start() refuses it rather than serve
// pathologically -- here genuinely up front, before any queue is bound or any
// serving coroutine is spawned, though after the registration itself exists.
//
// That last part is why the residue comparison matters more here than elsewhere:
// rollback() destroys the registration only when WE created it, so an ADOPTED
// orphan survives a refusal by design and a case that reached the guard through
// the adoption path would leave a registration behind on every run. The fixture's
// sweep() clears TEST_NAME in SetUp, which is what makes this start() take the
// create path, and the comparison is taken inside the case because that same sweep
// in TearDown would otherwise clean up a leak before any outside check could see
// it. No vdpa attach: start() never got far enough to serve, so there is no
// consumer side to bring up or down.
TEST_F(VduseTest, pool_without_an_event_engine_is_refused) {
    if (skip_reason) return;
    const std::string residue_before = residue();
    photon::WorkPool bad(2);      // ev_engine defaults to 0: no engine at all
    test::RecordingFile rec(file);

    BlkConfig cfg(make_info());
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

// The refusal the review asked about -- a start that fails AFTER the kernel
// registration exists -- reached through a trigger this suite can produce on
// demand instead of the one it named. The named trigger is the open of the char
// device that follows a successful creation failing, and nothing here can make
// that open fail: the routes that would (no permission on /dev/vduse, or the
// descriptor table full) fail the FIRST open in start() as well, since both
// opens sit at the same depth in the same table, so such a case would never
// reach the registration it exists to check. The rejected pool below does reach
// it: start() has created and claimed by then, and rollback() has to undo both.
//
// Both invariants are asserted directly and not only through the listing
// compare, because what the compare can see depends on what earlier cases left
// behind:
//   - the registration this start brought into existence is destroyed again, and
//   - the claim is released, with the tombstone FILE surviving the release --
//     release unlocks and closes, and only destroy_orphan unlinks. Pinning the
//     file's existence is also what makes the listing compare deterministic
//     rather than a function of case order.
//
// The retry half then shows the name is genuinely free again. A successful
// second start does not show that by itself: had the registration leaked,
// start() would have adopted it and succeeded just the same. So the two halves
// carry different facts -- the listing carries "nothing leaked into the kernel",
// the second start carries "nothing is still claimed here".
TEST_F(VduseTest, a_refused_start_leaves_no_registration_and_no_claim) {
    if (skip_reason) return;
    const std::string reg = std::string("/dev/vduse/") + TEST_NAME;
    const std::string lp = std::string(SUITE_LOCKS) + "/vduse-" + TEST_NAME + ".lock";
    const std::string before = residue();
    {
        photon::WorkPool bad(2);      // ev_engine defaults to 0: no engine at all
        BlkConfig cfg(make_info());
        cfg.queues = 2;
        cfg.pool = &bad;
        auto dev = ctl->new_device(cfg);
        ASSERT_NE(nullptr, dev);
        DEFER(delete dev);
        errno = 0;
        EXPECT_EQ(-1, dev->start(file));
        EXPECT_EQ(EINVAL, errno);
    }
    EXPECT_NE(0, ::access(reg.c_str(), F_OK)) << "the refused start left a registration";
    EXPECT_STREQ("not-held", lock_state(lp));
    EXPECT_EQ(before, residue());

    // The config is copied into the device when it is built, so the retry needs a
    // fresh config as well as a fresh object: reusing either one would retry the
    // rejected pool and fail for the reason this case just checked.
    BlkConfig cfg(make_info());
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    DEFER(dev->shutdown());
    ASSERT_EQ(0, dev->start(file));
    EXPECT_EQ(0, ::access(reg.c_str(), F_OK));
    EXPECT_STREQ("HELD", lock_state(lp));      // claimed for as long as it serves
    ASSERT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(reg.c_str(), F_OK));
    EXPECT_STREQ("not-held", lock_state(lp));
    EXPECT_EQ(before, residue());
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
