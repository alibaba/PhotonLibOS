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
// vduse + virtio_vdpa modules and iproute2's `vdpa` tool; otherwise GTEST_SKIPs.
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>

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

static void vdpa_detach(const char* name) {
    std::string cmd = "vdpa dev del ";
    cmd += name;
    cmd += " 2>/dev/null; true";
    test::sh_off_vcpu(cmd);
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

// virtio-blk's multiqueue feature bit (virtio 1.2 §5.2) and the transport's
// queue clamp, spelled out here on purpose -- the suite family's standing rule
// (see test-vhost-user.cpp's PEER_MAX_QUEUES): a suite must not reach into
// blk/utils.h to learn the values it asserts against, because that is how a
// suite becomes self-consistently wrong. If the two ever disagree these tests
// go red, which is the point.
static constexpr uint32_t FEAT_BIT_BLK_MQ = 12;
static constexpr uint32_t PEER_MAX_QUEUES = 64;

class VduseTest : public ::testing::Test {
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
            GTEST_SKIP() << "vduse test requires root";
        ::system("modprobe vduse 2>/dev/null");
        ::system("modprobe virtio_vdpa 2>/dev/null");   // separate: `modprobe a b`
                                                        // passes b as a PARAMETER of a
        if (::access("/dev/vduse/control", F_OK) != 0)
            GTEST_SKIP() << "vduse module not available";
        if (::system("which vdpa >/dev/null 2>&1") != 0)
            GTEST_SKIP() << "iproute2's vdpa tool not available";
        ctl = new_vduse_controller(nullptr);
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

    // remove leftovers of crashed runs, consumer first: detach our vdpa devs
    // (works without a daemon), then adopt+shutdown every orphan registration.
    // Owns its controller rather than using ctl: a GTEST_SKIP in SetUp returns
    // before ctl exists, and TearDown still runs.
    // The adopt is the only way this sweep destroys a registration: rollback()
    // destroys only what it created, and a failed start clears the state the
    // destructor keys on -- so when the adopt fails, the registration survives
    // this sweep and every later one, and each later start() on that name dies
    // the same way.
    void sweep() {
        test::sh_off_vcpu(
            "ls /sys/bus/vdpa/devices/ 2>/dev/null | grep '^"
            + std::string(NAME_PREFIX) +
            "' | while read n; do vdpa dev del \"$n\" 2>/dev/null; done; true");
        auto c = new_vduse_controller(nullptr);   // the default dir, same scope as ctl
        if (!c)
            return;
        DEFER(delete c);
        for (auto& rec : c->list_orphans()) {
            if (rec.identity.rfind(NAME_PREFIX, 0) != 0)
                continue;   // never touch devices we did not create
            BlkConfig cfg;
            cfg.info.identity = rec.identity;
            cfg.info.size = 4 << 20;   // tombstone record: the size is ours to
                                       // choose; adoption does not validate it
            auto d = c->new_device(cfg);
            if (!d)
                continue;
            auto f = lfs_open_dummy();
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
        i.identity = TEST_NAME;
        i.size = IMG_SIZE;
        i.sector_size_shift = 9;
        i.features = FEATURE_FLUSH;   // DISCARD/WRITE_ZEROES not offered in P1
        return i;
    }

    // run blocking device IO off the photon vcpu; returns 0 on success, errno
    // on a syscall failure, or EILSEQ on a data mismatch
    int device_io(const std::string& node, const std::vector<char>& wbuf,
                  bool verify_backend, uint64_t off = IO_OFF, bool read_only = false) {
        test::DeviceIoOpts o;
        o.backend = verify_backend ? file : nullptr;
        o.read_only = read_only;
        return test::device_io(node, wbuf.data(), wbuf.size(), off, o);
    }

    std::vector<char> pattern(uint8_t seed, size_t n = IO_LEN) { return test::pattern(seed, n); }
};

TEST_F(VduseTest, config_validation) {
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

    // a null backend is start()'s to reject: it is not part of the config
    BlkConfig good(make_info());
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

TEST_F(VduseTest, basic_io) {
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

// High-concurrency stress through the vdpa/virtio-blk driver: many O_DIRECT
// threads keep the single virtqueue full, the driver splits the larger blocks
// into multi-descriptor chains, and every completion comes back through the
// daemon's bounce mappings. The self-describing blocks (harness.h) attribute
// any misrouted, torn or lost IO; DISJOINT additionally requires each block to
// carry its reader's own tid+seq.
TEST_F(VduseTest, concurrent_stress) {
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

    vdpa_detach(TEST_NAME);   // consumer goes first, while we still serve
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(("/dev/vduse/" + std::string(TEST_NAME)).c_str(), F_OK));
}

TEST_F(VduseTest, orphan_recovery) {
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

    // a fresh object adopts it by name and resumes from used->idx; the
    // consumer never noticed (its backlog gets served)
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

TEST_F(VduseTest, daemon_restart_io) {
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

// The tombstone is vduse's ONLY ownership test -- the char device answers "is a
// daemon connected", never "is this ours" -- so it is also the scan's scope: a
// device locked in another dir must be invisible to a default-dir controller, as
// it is for ublk. One controller = one scope, so this test builds a second one.
// sweep() probes the default dir only and can therefore never clean up after this
// test; the shutdown DEFER does.
TEST_F(VduseTest, custom_lock_dir) {
    static const char LOCKS[] = "/tmp/photon-blk-vduse-test-locks";
    static const char NAME[]  = "photon-vduse-locks";
    ::system(("rm -rf " + std::string(LOCKS)).c_str());
    // a stale tombstone in the default dir would fool the invisibility check below
    ::unlink(("/run/photon-blk/vduse-" + std::string(NAME) + ".lock").c_str());
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

    // visible through the custom dir, invisible through the default one
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

// WorkPool's cursor is `vcpu_index++ % size`, so a zero-size pool is a SIGFPE the
// moment anything asks it for a vcpu; migrate_to_pool's short-circuit is the only
// thing in the way. Surviving is half the assertion, and staying on the caller's
// vcpu is the other half.
TEST_F(VduseTest, empty_pool_falls_back_to_the_caller_vcpu) {
    photon::WorkPool empty(0, (int)photon::get_event_engine(),
                              (int)photon::get_io_engine());
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
