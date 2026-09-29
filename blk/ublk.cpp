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

// UBLK transport (ublk_drv): exports a photon IFile as a kernel block device
// /dev/ublkbN. start() ADD_DEVs on /dev/ublk-control (or re-attaches a
// QUIESCED device left by a dead daemon when USER_RECOVERY is on), SET_PARAMS
// the geometry, mmaps the per-queue descriptor rings of /dev/ublkcN, starts
// one photon coroutine per tag (each parks its own FETCH_REQ), then
// START_DEVs. detach() closes the serving fds, which quiesces the device
// (UBLK_F_USER_RECOVERY_REISSUE requeues in-flight IO for the next start);
// shutdown() additionally TRY_STOP_DEV (EBUSY while the disk is held open) +
// DEL_DEV. resize() = UBLK_U_CMD_UPDATE_SIZE (UBLK_F_UPDATE_SIZE is always
// requested at ADD).
//
// Rings: no libublksrv. ublk has no read/write/ioctl interface -- everything
// is io_uring URING_CMD, issued through photon's iouring engine: a dedicated
// cascading-engine instance per ring (an SQE128 one for the control plane on
// /dev/ublk-control, a 2x-queue_depth one per data-plane queue), each driven
// by a pump coroutine (wait_for_events reaps cqes and resumes the parked
// submitters; eager_submit makes every submit inline, so a park can never
// miss its own completion). The ublk UAPI constants and structs are copied
// verbatim from <linux/ublk_cmd.h> (7.0): the header is C++-clean but too new
// for many build hosts -- tcmu.cpp set the self-contained precedent.
//
// Commit result semantics: the value handed to COMMIT_AND_FETCH_REQ is >= 0 for
// the number of bytes completed, < 0 for an -errno failing the whole request. We
// always commit one or the other and never a short count, so the short-count
// handling (a zero-length READ forced to -EIO, a partial count requeueing the
// remainder) is not a path this transport reaches.
//
// Operating facts the transport is built on. None of them come from a
// specification -- ublk has none; <linux/ublk_cmd.h> fixes the ABI and the rest
// was established by running against the kernel module (7.0):
// - the control ring must be SQE128: ublksrv_ctrl_cmd is 32B (static_asserted
//   below) while a 64B sqe's cmd area is 16B, so the command does not fit; the
//   per-queue rings stay 64B because ublksrv_io_cmd is exactly 16B.
// - a tag's FETCH_REQ parks until a request arrives; the request descriptor
//   (ublksrv_io_desc) is read from the per-queue mmap (PROT_READ; offset
//   qid * UBLK_MAX_QUEUE_DEPTH * sizeof(desc), both constants from the UAPI
//   header). A COMMIT_AND_FETCH_REQ's commit half takes effect when it is
//   issued, before its fetch half parks -- so cancelling the parked cmd never
//   un-commits the request.
// - initial FETCH_REQs are accepted only before the device is ready (EBUSY
//   afterwards), and neither START_DEV nor END_USER_RECOVERY completes until
//   every queue's fetches have arrived -- so all fetches must be submitted
//   before issuing either.
// - a tag belongs to the task that issued its FETCH_REQ and every later command
//   for that tag must come from the same task -- each queue is therefore pinned
//   to exactly one serving vcpu.
// - START_USER_RECOVERY requires the old daemon's /dev/ublkcN released AND the
//   device QUIESCED; the quiesce runs asynchronously after the close, so the
//   EBUSY must be polled rather than assumed gone. END_USER_RECOVERY(pid) brings
//   the device back to LIVE and requeues what was in flight. A thread of the old
//   daemon that is stuck in an uninterruptible sleep defeats the first
//   precondition for good: the fd table belongs to the process, not the thread,
//   so the control device stays referenced until the LAST thread exits -- and
//   the thread that cannot exit is exactly the one waiting on the IO that only
//   a re-attach would complete.
// - request data crosses via the per-tag userspace buffer whose address was
//   handed over in FETCH_REQ: the request's bytes are copied into that buffer at
//   fetch time (a WRITE) and out of it at commit time (a READ).

#include "blk.h"
#include "utils.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/io/fd-events.h>
#include <photon/io/iouring-wrapper.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifndef RWF_DSYNC
#define RWF_DSYNC 0x00000002
#endif

namespace photon {
namespace blk {

// ----------------------------------------------------------------------------
// ublk UAPI subset (verbatim from <linux/ublk_cmd.h> @ 7.0)
// ----------------------------------------------------------------------------

// legacy IO command numbers (pre-ioctl-encoding); used only if the driver
// negotiated UBLK_F_CMD_IOCTL_ENCODE off (ancient kernels)
#define UBLK_IO_FETCH_REQ               0x20
#define UBLK_IO_COMMIT_AND_FETCH_REQ    0x21

#define UBLK_U_CMD_GET_QUEUE_AFFINITY   _IOR('u', 0x01, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_GET_DEV_INFO         _IOR('u', 0x02, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_ADD_DEV              _IOWR('u', 0x04, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_DEL_DEV              _IOWR('u', 0x05, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_START_DEV            _IOWR('u', 0x06, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_STOP_DEV             _IOWR('u', 0x07, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_SET_PARAMS           _IOWR('u', 0x08, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_GET_PARAMS           _IOR('u', 0x09, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_START_USER_RECOVERY  _IOWR('u', 0x10, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_END_USER_RECOVERY    _IOWR('u', 0x11, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_GET_DEV_INFO2        _IOR('u', 0x12, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_UPDATE_SIZE          _IOWR('u', 0x15, ublksrv_ctrl_cmd)
#define UBLK_U_CMD_TRY_STOP_DEV         _IOWR('u', 0x17, ublksrv_ctrl_cmd)

#define UBLK_U_IO_FETCH_REQ             _IOWR('u', 0x20, ublksrv_io_cmd)
#define UBLK_U_IO_COMMIT_AND_FETCH_REQ  _IOWR('u', 0x21, ublksrv_io_cmd)

// only ABORT means that no re-fetch
#define UBLK_IO_RES_OK          0
#define UBLK_IO_RES_ABORT       (-ENODEV)

#define UBLK_MAX_QUEUE_DEPTH    4096
#define UBLK_MAX_NR_QUEUES      (1U << 12)

#define UBLK_F_USER_RECOVERY            (1ULL << 3)
#define UBLK_F_USER_RECOVERY_REISSUE    (1ULL << 4)
#define UBLK_F_CMD_IOCTL_ENCODE         (1ULL << 6)
#define UBLK_F_UPDATE_SIZE              (1ULL << 10)
#define UBLK_F_SAFE_STOP_DEV            (1ULL << 17)
#define UBLK_F_NO_AUTO_PART_SCAN        (1ULL << 18)

#define UBLK_S_DEV_DEAD         0
#define UBLK_S_DEV_LIVE         1
#define UBLK_S_DEV_QUIESCED     2
#define UBLK_S_DEV_FAIL_IO      3

// shipped via sqe->cmd of the control ring
struct ublksrv_ctrl_cmd {
    uint32_t dev_id;
    uint16_t queue_id;      // must be -1 if the cmd isn't for a queue
    uint16_t len;           // cmd specific buffer, can be IN or OUT
    uint64_t addr;
    uint64_t data[1];       // inline data
    uint16_t dev_path_len;
    uint16_t pad;
    uint32_t reserved;
};
static_assert(sizeof(ublksrv_ctrl_cmd) == 32, "ublksrv_ctrl_cmd size");

struct ublksrv_ctrl_dev_info {
    uint16_t nr_hw_queues;
    uint16_t queue_depth;
    uint16_t state;
    uint16_t pad0;
    uint32_t max_io_buf_bytes;
    uint32_t dev_id;
    int32_t  ublksrv_pid;
    uint32_t pad1;
    uint64_t flags;
    uint64_t ublksrv_flags;
    uint32_t owner_uid;     // stored by the kernel
    uint32_t owner_gid;
    uint64_t reserved1;
    uint64_t reserved2;
};
static_assert(sizeof(ublksrv_ctrl_dev_info) == 64, "ublksrv_ctrl_dev_info size");

#define UBLK_IO_OP_READ         0
#define UBLK_IO_OP_WRITE        1
#define UBLK_IO_OP_FLUSH        2
#define UBLK_IO_OP_DISCARD      3
#define UBLK_IO_OP_WRITE_SAME   4
#define UBLK_IO_OP_WRITE_ZEROES 5

#define UBLK_IO_F_FUA           (1U << 13)

// request descriptor, read from the per-queue mmap, indexed by tag
struct ublksrv_io_desc {
    uint32_t op_flags;      // op: bit 0-7, flags: bit 8-31
    uint32_t nr_sectors;
    uint64_t start_sector;
    uint64_t addr;          // buffer address in our vm space (kernel-recorded)
};
static_assert(sizeof(ublksrv_io_desc) == 24, "ublksrv_io_desc size");

// shipped via sqe->cmd of a queue ring (fits the 16B cmd area of a 64B sqe)
struct ublksrv_io_cmd {
    uint16_t q_id;
    uint16_t tag;
    int32_t  result;        // valid for COMMIT only
    uint64_t addr;          // tag's data buffer (evaluated at FETCH)
};
static_assert(sizeof(ublksrv_io_cmd) == 16, "ublksrv_io_cmd size");

#define UBLK_ATTR_READ_ONLY     (1U << 0)
#define UBLK_ATTR_ROTATIONAL    (1U << 1)
#define UBLK_ATTR_VOLATILE_CACHE (1U << 2)
#define UBLK_ATTR_FUA           (1U << 3)

struct ublk_param_basic {
    uint32_t attrs;
    uint8_t  logical_bs_shift;
    uint8_t  physical_bs_shift;
    uint8_t  io_opt_shift;
    uint8_t  io_min_shift;
    uint32_t max_sectors;
    uint32_t chunk_sectors;
    uint64_t dev_sectors;
    uint64_t virt_boundary_mask;
};
struct ublk_param_discard {
    uint32_t discard_alignment;
    uint32_t discard_granularity;
    uint32_t max_discard_sectors;
    uint32_t max_write_zeroes_sectors;
    uint16_t max_discard_segments;
    uint16_t reserved0;
};
struct ublk_param_devt     { uint32_t char_major, char_minor, disk_major, disk_minor; };
struct ublk_param_zoned    { uint32_t max_open_zones, max_active_zones, max_zone_append_sectors; uint8_t reserved[20]; };
struct ublk_param_dma_align{ uint32_t alignment; uint8_t pad[4]; };
struct ublk_param_segment  { uint64_t seg_boundary_mask; uint32_t max_segment_size; uint16_t max_segments; uint8_t pad[2]; };
struct ublk_param_integrity{ uint32_t flags; uint16_t max_integrity_segments; uint8_t interval_exp, metadata_size, pi_offset, csum_type, tag_size, pad[5]; };
struct ublk_params {
    uint32_t len;
#define UBLK_PARAM_TYPE_BASIC    (1U << 0)
#define UBLK_PARAM_TYPE_DISCARD  (1U << 1)
    uint32_t types;
    struct ublk_param_basic      basic;
    struct ublk_param_discard    discard;
    struct ublk_param_devt       devt;
    struct ublk_param_zoned      zoned;
    struct ublk_param_dma_align  dma;
    struct ublk_param_segment    seg;
    struct ublk_param_integrity  integrity;
};
static_assert(sizeof(ublk_params) == 152, "ublk_params size");

// ----------------------------------------------------------------------------
// UblkCtrl: the control channel -- one SQE128 ring on /dev/ublk-control
// issuing synchronous commands (the cqe's res is the command's return value:
// 0 or -errno)
// ----------------------------------------------------------------------------

struct UblkCtrl {
    // Field order is padding-driven, do not tidy it: `fd` used to lead, so `ce` had
    // to skip the 4 bytes behind it and `stopping` then left a 7-byte tail.
    // 24 bytes vs 32.
    photon::CascadingEventEngine* ce = nullptr;
    photon::thread* pump_th = nullptr;
    int fd = -1;
    bool stopping = false;

    ~UblkCtrl() { fini(); }

    // idempotent: start() calls this every round, and a detach->re-attach
    // cycle must not leak the previous ring (init() would overwrite the fds
    // and mappings of a live instance)
    int init() {
        if (fd >= 0)
            return 0;
        fd = ::open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
        if (fd < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to open /dev/ublk-control (ublk_drv loaded?)");
        bool ok = false;
        DEFER(if (!ok) fini());
        iouring_args args;
        args.setup_sqe128 = true;    // ublksrv_ctrl_cmd is 32B, beyond a 64B sqe's cmd area
        args.queue_depth = 8;        // ctrl commands are issued one at a time
        args.register_files = false;
        args.eager_submit = true;    // the pump may already be parked in wait_for_events
                                     // when a sqe is filled; only an inline submit
                                     // guarantees that its cqe can arrive
        ce = new_iouring_cascading_engine(args);
        if (!ce)
            LOG_ERROR_RETURN(ENOMEM, -1, "failed to create the ublk ctrl io_uring engine");
        pump_th = photon::thread_create11(&UblkCtrl::pump, this);
        photon::thread_enable_join(pump_th);
        ok = true;
        return 0;
    }

    // reaps cqes and resumes the parked transacts. Runs on init()'s vcpu;
    // transact() callers (and fini()) must share it -- the SQ is not
    // thread-safe, and the pump joins back to that vcpu
    void pump() {
        while (!stopping)
            ce->wait_for_events(nullptr, 0);
    }

    void fini() {
        if (pump_th) {
            stopping = true;
            photon::thread_interrupt(pump_th);
            photon::thread_join((photon::join_handle*)pump_th);
            pump_th = nullptr;
        }
        if (ce) { delete ce; ce = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
    }

    // one control command round-trip; 0 on success, -1 with errno set
    int transact(uint32_t dev_id, uint32_t cmd_op, void* buf, uint16_t len, uint64_t data0) {
        ublksrv_ctrl_cmd c;
        memset(&c, 0, sizeof(c));
        c.dev_id = dev_id;
        c.queue_id = (uint16_t)-1;
        c.len = len;
        c.addr = (uint64_t)(uintptr_t)buf;
        c.data[0] = data0;
        if (iouring_uring_cmd(fd, cmd_op, &c, sizeof(c), Timeout(), ce) < 0)
            return -1;   // errno = -res, set by iouring_uring_cmd
        return 0;
    }

    // dev_info is IN/OUT: dev_id (U32_MAX = auto-assign) comes back assigned,
    // flags comes back masked to what the driver supports
    int add_dev(ublksrv_ctrl_dev_info* info) {
        return transact(info->dev_id, UBLK_U_CMD_ADD_DEV, info, sizeof(*info), 0);
    }
    int get_info(uint32_t id, ublksrv_ctrl_dev_info* info) {
        memset(info, 0, sizeof(*info));
        return transact(id, UBLK_U_CMD_GET_DEV_INFO, info, sizeof(*info), 0);
    }
    int set_params(uint32_t id, ublk_params* p) {
        p->len = sizeof(*p);
        return transact(id, UBLK_U_CMD_SET_PARAMS, p, sizeof(*p), 0);
    }
    int get_params(uint32_t id, ublk_params* p) {
        memset(p, 0, sizeof(*p));
        p->len = sizeof(*p);
        return transact(id, UBLK_U_CMD_GET_PARAMS, p, sizeof(*p), 0);
    }
    int start_dev(uint32_t id, int32_t pid)  { return transact(id, UBLK_U_CMD_START_DEV, nullptr, 0, (uint64_t)pid); }
    int stop_dev(uint32_t id)                { return transact(id, UBLK_U_CMD_STOP_DEV, nullptr, 0, 0); }
    int try_stop_dev(uint32_t id)            { return transact(id, UBLK_U_CMD_TRY_STOP_DEV, nullptr, 0, 0); }
    int del_dev(uint32_t id)                 { return transact(id, UBLK_U_CMD_DEL_DEV, nullptr, 0, 0); }
    int start_recovery(uint32_t id)          { return transact(id, UBLK_U_CMD_START_USER_RECOVERY, nullptr, 0, 0); }
    int end_recovery(uint32_t id, int32_t pid) { return transact(id, UBLK_U_CMD_END_USER_RECOVERY, nullptr, 0, (uint64_t)pid); }
    int update_size(uint32_t id, uint64_t sectors) { return transact(id, UBLK_U_CMD_UPDATE_SIZE, nullptr, 0, sectors); }
};

// ----------------------------------------------------------------------------
// UblkDeviceImpl: the IBlkDevice. Control channel + /dev/ublkcN + the queues.
// ----------------------------------------------------------------------------

static constexpr uint32_t DEFAULT_QUEUE_DEPTH = 128;
static constexpr uint32_t IO_BUF_BYTES = 512 << 10;   // per-tag data buffer

struct UblkDeviceImpl : IBlkDevice {
    // Field order is padding-driven, do not tidy it: the align-8 members lead, the
    // 24 bytes of int-sized ones then tile exactly three whole 8-byte blocks, and
    // the six 1-byte members close the run ahead of the two char buffers.
    // `own_backend`/`started`/`created` used to sit right after `backend`, and that
    // 3-byte run left 18 bytes of holes ahead of `lock_fd`, `features`, `spin_us`
    // and `queues`. 616 bytes vs 640 (8 of it is UblkCtrl's own shrink).
    UblkController::Config cfg;
    fs::IFile* backend = nullptr;

    int64_t dev_id = -1;          // the kernel device id; -1 = not registered

    // The ONLY writer of dev_id: node_path is derived from it and blk.h promises
    // get_device_node() is stable for the session, so a caller may be holding the
    // pointer while another thread calls again. Updating both here keeps them
    // from drifting and makes get_device_node() a pure read (as nbd's and tcmu's
    // already are) instead of a rewrite of a member buffer on every call.
    void set_dev_id(int64_t id) {
        dev_id = id;
        if (id < 0)
            node_path[0] = '\0';
        else
            snprintf(node_path, sizeof(node_path), "/dev/ublkb%llu", (unsigned long long)id);
    }
    std::atomic<uint64_t> dev_sectors{0};
    uint64_t features = 0;
    uint64_t spin_us = 0;
    uint64_t negotiated_flags = 0;   // dev_info.flags as accepted by the driver

    UblkCtrl ctrl;

    int lock_fd = -1;
    int cdev_fd = -1;
    uint32_t op_fetch = UBLK_U_IO_FETCH_REQ;    // encoded vs legacy, from the
    uint32_t op_commit = UBLK_U_IO_COMMIT_AND_FETCH_REQ;   // negotiated flags
    uint32_t max_io_buf_bytes = IO_BUF_BYTES;
    uint16_t queue_depth = DEFAULT_QUEUE_DEPTH;
    uint16_t nr_queues = 1;

    struct Queue {
        // Field order is padding-driven, do not tidy it: `qid` used to sit between
        // `d` and `ce`, forcing `ce` to skip to offset 16, and `in_flight` between
        // `bufs` and `pump_th` forced a second 4-byte skip. 120 bytes vs 136.
        UblkDeviceImpl* d = nullptr;
        photon::CascadingEventEngine* ce = nullptr;   // the queue's io_uring ring
        const ublksrv_io_desc* cmd_buf = nullptr;   // mmap'd off /dev/ublkcN, PROT_READ
        size_t cmd_buf_sz = 0;
        char* bufs = nullptr;    // queue_depth * max_io_buf_bytes, page-aligned
        photon::thread* pump_th = nullptr;
        // The vcpu this queue's ring, pump and tag coroutines all live on. Recorded
        // from INSIDE the owner coroutine, because that is the only place it can be
        // observed: WorkPool does not expose its vcpus, so which one the cursor
        // picked is knowable only after landing there. queue_teardown migrates back
        // to it -- the io_uring submission queue is not thread-safe, so the pump and
        // every submitter must share a vcpu, and that includes teardown, which
        // interrupts and joins the tags.
        photon::vcpu_base* home = nullptr;
        std::vector<photon::thread*> tag_ths;   // one coroutine per tag
        photon::semaphore fetches_issued;   // queue_setup waits for the initial fetches
        std::atomic<uint32_t> in_flight{0};   // serving (fetched-not-yet-committed)
        uint16_t qid = 0;
        bool stopping = false;       // tags: exit at the next checkpoint
        bool pump_stop = false;      // pump: exit -- set only AFTER all tags are joined

        char* buf(uint16_t tag) { return bufs + (size_t)tag * d->max_io_buf_bytes; }
    };
    std::vector<Queue*> queues;

    bool own_backend = false;
    bool started = false;
    bool created = false;         // we ADD_DEV'd it (vs re-attached a quiesced one)
    bool read_only = false;
    PollPolicy poll = PollPolicy::SLEEP;
    uint8_t sector_shift = 9;

    // The scope directory this device claims its tombstone in, handed over by the
    // UblkController that built it. COPIED, not borrowed: a device may outlive its
    // controller.
    char lock_dir[SCOPE_DIR_BUF] = {};

    UblkDeviceImpl(const UblkController::Config& c, const char* ldir) : cfg(c) {
        if (ldir)
            snprintf(lock_dir, sizeof(lock_dir), "%s", ldir);
        sector_shift = cfg.info.sector_size_shift;
        features = cfg.info.features;
        read_only = cfg.read_only;
        poll = cfg.poll;
        spin_us = cfg.spin_us;
        dev_sectors = cfg.info.size >> sector_shift;
        // a cfg field left 0 means "default", and the config is immutable, so
        // these are derived exactly once -- no per-start() reset needed
        if (cfg.queue_depth)
            queue_depth = (uint16_t)std::min<uint32_t>(cfg.queue_depth, UBLK_MAX_QUEUE_DEPTH);
        if (cfg.queues)
            nr_queues = (uint16_t)std::min<uint32_t>(cfg.queues, MAX_QUEUES);
    }

    // pure config validation -- no I/O, no kernel access. The factory runs it
    // before constructing, so a constructed device is always config-valid.
    static int validate(const UblkController::Config& c) {
        if (validate_info(c.info, /*virtio=*/false) < 0)
            return -1;
        return 0;
    }

    const BlkDevInfo& get_info() const override { return cfg.info; }

    ~UblkDeviceImpl() {
        if (started || dev_id >= 0) {
            if (shutdown() < 0 && started) {
                // e.g. EBUSY: the initiator holds the disk. Never leave a live
                // device served with a backend we're about to delete: stop
                // serving and leave a quiesced, recoverable orphan instead
                stop_serving(false);
                release_lock();
                started = false;
            }
        }
        // A shutdown() that propagated a DEL_DEV failure has already stopped
        // serving, so the branch above skips it -- but it kept the flock (the
        // registration survives), and lock_fd is a plain member that would
        // outlive this object. devlock_release() is a no-op on -1, so the paths
        // that did release it are unaffected.
        release_lock();
        if (own_backend)
            delete backend;
        ctrl.fini();
    }

    // ----- the IO path -----

    // serve the request currently occupying `tag`; returns the value to
    // commit (bytes completed, or -errno). Does NOT commit -- tag_loop owns
    // that decision: a teardown interrupt must leave the request uncommitted
    // so the quiesce REISSUEs it instead of feeding the initiator an EINTR-
    // induced error
    int32_t serve_req(Queue* q, uint16_t tag) {
        const auto iod = &q->cmd_buf[tag];
        uint8_t op = iod->op_flags & 0xff;
        uint64_t len = (uint64_t)iod->nr_sectors << sector_shift;
        uint64_t off = iod->start_sector << sector_shift;
        int32_t res;
        switch (op) {
        case UBLK_IO_OP_READ: {
            if (len > max_io_buf_bytes) {   // the kernel caps via max_sectors; defensive
                res = -EINVAL;
                break;
            }
            iovec v{q->buf(tag), (size_t)len};
            ssize_t r = backend->preadv(&v, 1, off);
            res = (r == (ssize_t)len) ? (int32_t)len : -EIO;
            break;
        }
        case UBLK_IO_OP_WRITE: {
            if (read_only) { res = -EROFS; break; }
            if (len > max_io_buf_bytes) {   // same defensive bound as READ
                res = -EINVAL;
                break;
            }
            iovec v{q->buf(tag), (size_t)len};
            // UBLK_IO_F_* constants are numbered in the raw op_flags space
            // (bit >= 8); the kernel sets them directly, so no >> 8 here
            bool fua = iod->op_flags & UBLK_IO_F_FUA;   // set only if we advertised FUA
            ssize_t w = fua ? backend->pwritev2(&v, 1, off, RWF_DSYNC)
                            : backend->pwritev(&v, 1, off);
            res = (w == (ssize_t)len) ? (int32_t)len : -EIO;
            break;
        }
        case UBLK_IO_OP_FLUSH:
            res = backend->fdatasync() == 0 ? 0 : -EIO;
            break;
        case UBLK_IO_OP_DISCARD:
            if (read_only) { res = -EROFS; break; }
            if (backend->trim(off, len) < 0 && errno != EOPNOTSUPP && errno != ENOSYS) {
                res = -EIO;
                break;
            }
            res = (int32_t)len;
            break;
        case UBLK_IO_OP_WRITE_ZEROES:
            if (read_only) { res = -EROFS; break; }
            res = zero_fill(backend, off, len) == 0 ? (int32_t)len : -EIO;
            break;
        default:
            res = -EOPNOTSUPP;
            break;
        }
        return res;
    }

    // one coroutine per tag: FETCH parks until a request occupies the tag;
    // serve it; COMMIT_AND_FETCH lands the commit kernel-side at issue time
    // and parks again on its fetch half. Teardown interrupts the park (the
    // engine cancels the uring_cmd; the already-landed commit is unaffected).
    void tag_loop(Queue* q, uint16_t tag) {
        ublksrv_io_cmd c;
        memset(&c, 0, sizeof(c));
        c.q_id = q->qid;
        c.tag = tag;
        c.addr = (uint64_t)(uintptr_t)q->buf(tag);
        uint32_t op = op_fetch;
        // no yield between this signal and the eager submit inside the first
        // iouring_uring_cmd below, so once queue_setup's wait() returns, every
        // initial fetch is in flight (START_DEV / END_USER_RECOVERY require it)
        q->fetches_issued.signal(1);
        for (;;) {
            int32_t r = iouring_uring_cmd(cdev_fd, op, &c, sizeof(c), Timeout(), q->ce);
            if (q->stopping)
                break;   // teardown: leave any arrived request to the REISSUE
            if (r != UBLK_IO_RES_OK) {
                if (r > 0 || errno != ENODEV)   // ENODEV = ABORT: the device is going away
                    LOG_ERROR("ublk fetch failed, dev ` qid ` tag ` r `, ", dev_id, q->qid, tag, r, ERRNO());
                break;
            }
            q->in_flight++;
            int32_t result = serve_req(q, tag);
            q->in_flight--;
            if (q->stopping)
                break;   // interrupted mid-serve: uncommitted, REISSUE requeues it
            c.result = result;
            op = op_commit;
        }
    }

    // one pump coroutine per queue: submits nothing (eager_submit) and reaps
    // cqes, resuming the parked tag coroutines. Must share the tags' vcpu, and
    // must outlive them: an interrupted tag's ASYNC_CANCEL choreography needs
    // the pump to reap cqes (possibly in separate batches) until the tag exits
    void pump(Queue* q) {
        uint64_t last_work = photon::now;
        while (!q->pump_stop) {
            bool spin = poll == PollPolicy::SPIN ||
                        (poll == PollPolicy::ADAPTIVE && photon::now - last_work < spin_us);
            ssize_t n = q->ce->wait_for_events(nullptr, 0, spin ? Timeout(0) : Timeout());
            if (n < 0) {
                if (q->pump_stop) break;
                if (errno != EINTR) {
                    LOG_WARN("ublk pump wait failed, ", ERRNO());
                    photon::thread_usleep(1000);
                }
                continue;
            }
            if (n > 0)
                last_work = photon::now;
            else if (spin)
                photon::thread_yield();
        }
    }

    // queue setup/teardown run on the queue's OWNING vcpu (the initial fetches
    // bind the per-tag daemon task)
    int queue_setup(Queue* q) {
        q->cmd_buf_sz = (size_t)queue_depth * sizeof(ublksrv_io_desc);
        q->cmd_buf_sz = (q->cmd_buf_sz + 4095) & ~(size_t)4095;   // page round-up
        off_t off = (off_t)q->qid * UBLK_MAX_QUEUE_DEPTH * sizeof(ublksrv_io_desc);
        void* m = ::mmap(nullptr, q->cmd_buf_sz, PROT_READ, MAP_SHARED | MAP_POPULATE,
                         cdev_fd, off);
        if (m == MAP_FAILED)
            LOG_ERRNO_RETURN(0, -1, "ublk cmd buf mmap failed, dev ` qid `", dev_id, q->qid);
        q->cmd_buf = (const ublksrv_io_desc*)m;
        if (::posix_memalign((void**)&q->bufs, 4096, (size_t)queue_depth * max_io_buf_bytes))
            LOG_ERROR_RETURN(ENOMEM, -1, "ublk io buffers alloc failed, dev ` qid `", dev_id, q->qid);
        iouring_args args;
        args.queue_depth = (uint32_t)queue_depth * 2;   // every tag parks one cmd,
                                                        // plus slack for teardown cancels
        args.register_files = false;
        args.eager_submit = true;   // tags submit before parking; the pump only reaps
        q->ce = new_iouring_cascading_engine(args);
        if (!q->ce)
            LOG_ERROR_RETURN(ENOMEM, -1, "failed to create the io_uring engine of ublk queue `, dev `",
                             q->qid, dev_id);
        q->stopping = false;
        q->pump_stop = false;
        q->in_flight = 0;
        q->pump_th = photon::thread_create11(&UblkDeviceImpl::pump, this, q);
        photon::thread_enable_join(q->pump_th);
        // queue_depth long-lived coroutines per queue, each calling into the
        // caller's backend IFile via serve_req -- so these are the ones photon's
        // 8 MiB default hurts most (see DEFAULT_REQ_STACK in utils.h).
        //
        // The create is checked because fetches_issued.wait() below is a token
        // committed BEFORE these creates: a nullptr return would hang it forever
        // and hand thread_enable_join a null. Only successful ones are pushed,
        // and queue_teardown is null-safe on a partially set-up queue, which is
        // what start()'s rollback runs for every queue on this -1.
        uint32_t stack = resolve_stack_size(cfg.stack_size);
        for (uint32_t tag = 0; tag < queue_depth; tag++) {
            auto th = photon::thread_create11(stack, &UblkDeviceImpl::tag_loop, this, q, (uint16_t)tag);
            if (!th)
                LOG_ERROR_RETURN(ENOMEM, -1, "ublk: cannot create the tag ` coroutine of queue `, dev `",
                                 tag, q->qid, dev_id);
            photon::thread_enable_join(th);
            q->tag_ths.push_back(th);
        }
        q->fetches_issued.wait(queue_depth);   // all initial fetches in flight (see tag_loop)
        return 0;
    }

    void queue_teardown(Queue* q) {
        q->stopping = true;
        // ublk's parked FETCH uring_cmds cannot be cancelled on request (the
        // driver has no cancel hook), so the engine's interrupt->ASYNC_CANCEL
        // dance would strand a tag forever: abandon the ring, turning each
        // interrupt into an immediate -1 return. Tags caught mid-serve exit at
        // their next yield (backend IO interrupted); their uncommitted requests
        // are REISSUEd by the quiesce that follows the cdev close.
        //
        // The pump goes FIRST. An abandoned caller returns without waiting for
        // its completion, so its stack-held ioCtx dies while the kernel still
        // points at it -- and reaping routes every completion unconditionally.
        // The tag joins below yield this vcpu, so a pump still running would
        // write res/done into freed (and pool-recycled) coroutine stack and
        // interrupt a stale photon::thread* whenever a request lands mid-teardown
        // (the device is still LIVE and every tag parks one un-cancellable cmd).
        // After abandon the tags need no cqe to wake them, so stopping the pump
        // early costs nothing -- and it is this ring's ONLY reaper: a cascading
        // engine is registered with no master, so nothing else calls into it.
        if (q->pump_th) {
            q->pump_stop = true;
            photon::thread_interrupt(q->pump_th);
            photon::thread_join((photon::join_handle*)q->pump_th);
            q->pump_th = nullptr;
        }
        if (q->ce)
            iouring_abandon(q->ce);
        for (auto th : q->tag_ths)
            photon::thread_interrupt(th);
        for (auto th : q->tag_ths)
            photon::thread_join((photon::join_handle*)th);
        q->tag_ths.clear();
        if (q->ce) {   // ring exit force-completes the abandoned cmds; their
            delete q->ce;   // cqes die unreaped (no reaper is left)
            q->ce = nullptr;
        }
        if (q->cmd_buf) {
            ::munmap((void*)q->cmd_buf, q->cmd_buf_sz);
            q->cmd_buf = nullptr;
        }
        ::free(q->bufs);
        q->bufs = nullptr;
    }

    uint32_t in_flight_all() {
        uint32_t n = 0;
        for (auto q : queues)
            n += q->in_flight.load();
        return n;
    }

    // ----- serving orchestration -----

    // One owner coroutine per queue, migrated once. The migration has to happen
    // BEFORE the queue's ring exists rather than after: queue_setup creates the
    // io_uring ring, the pump and every tag coroutine, and all of them must share
    // one vcpu because the submission queue is not thread-safe. Migrating the pump
    // afterwards would leave the tags submitting on the caller's vcpu while the
    // pump reaps on another; migrating the tags too is impossible, because by the
    // time queue_setup returns they are all parked in wait_for_events and
    // thread_migrate only accepts a READY thread. So the coroutine that RUNS
    // queue_setup is the thing that gets migrated, and everything it creates
    // inherits its vcpu -- no pinning needed, and the pool's cursor is drawn
    // exactly once per queue.
    struct OwnerArg {
        UblkDeviceImpl* d;
        Queue* q;
        photon::semaphore done{0};   // NSDMI, not {}: semaphore's ctor is explicit
        int err = 0;
    };

    static void* queue_owner(void* a) {
        auto* oa = (OwnerArg*)a;
        oa->q->home = photon::get_vcpu();
        if (oa->d->queue_setup(oa->q) < 0)
            oa->err = errno ? errno : EIO;
        oa->done.signal(1);
        return nullptr;   // the owner leaves; the pump it created stays
    }

    int start_serving() {
        if (check_pool_engines(cfg.pool) < 0)
            return -1;
        for (auto q : queues) {
            OwnerArg oa{this, q};
            auto th = photon::thread_create(&UblkDeviceImpl::queue_owner, &oa);
            if (!th)
                LOG_ERROR_RETURN(ENOMEM, -1, "ublk: cannot create the owner coroutine of queue `, dev `",
                                 q->qid, dev_id);
            photon::thread_enable_join(th);
            migrate_to_pool(cfg.pool, th);
            oa.done.wait(1);
            photon::thread_join((photon::join_handle*)th);
            if (oa.err)
                LOG_ERROR_RETURN(oa.err, -1, "ublk queue ` setup failed, dev `", q->qid, dev_id);
        }
        return 0;
    }

    // queue_teardown interrupts and joins the tag coroutines and stops the pump,
    // all of which live on q->home, so it has to run there too -- the same
    // submission-queue rule that put them on one vcpu in the first place. The
    // migration target is photon's own API rather than WorkPool's: WorkPool cannot
    // name a vcpu it already handed out, but the owner coroutine recorded it.
    struct TeardownArg {
        UblkDeviceImpl* d;
        Queue* q;
        photon::semaphore done{0};   // NSDMI, not {}: semaphore's ctor is explicit
    };

    static void* queue_teardown_thunk(void* a) {
        auto* ta = (TeardownArg*)a;
        ta->d->queue_teardown(ta->q);
        ta->done.signal(1);
        return nullptr;
    }

    void run_queue_teardown(Queue* q) {
        if (!q->home || q->home == photon::get_vcpu()) {
            queue_teardown(q);   // no pool, or the cursor put it right here
            return;
        }
        TeardownArg ta{this, q};
        auto th = photon::thread_create(&UblkDeviceImpl::queue_teardown_thunk, &ta);
        if (!th) {
            // Cannot honour the vcpu rule, but leaving the queue up is worse: the
            // device is being torn down and the ring is about to be unmapped.
            LOG_ERROR("ublk: cannot create the teardown coroutine of queue `, tearing down in place",
                      q->qid);
            queue_teardown(q);
            return;
        }
        photon::thread_enable_join(th);
        if (photon::thread_migrate(th, q->home) < 0) {
            LOG_WARN("ublk: cannot move the teardown of queue ` back to its vcpu, ", q->qid, ERRNO());
        }
        ta.done.wait(1);
        photon::thread_join((photon::join_handle*)th);
    }

    void stop_serving(bool flush) {
        if (flush)
            while (in_flight_all())
                photon::thread_usleep(1000);
        for (auto q : queues)
            run_queue_teardown(q);
        for (auto q : queues)
            delete q;
        queues.clear();
        if (cdev_fd >= 0) {
            ::close(cdev_fd);   // releases UB_STATE_OPEN; the device quiesces
            cdev_fd = -1;
        }
    }

    // ----- flock (the orphan-detection liveness key, shared namespace) -----

    int acquire_lock(uint32_t id) {
        char name[32];
        snprintf(name, sizeof(name), "ublk-%u.lock", id);
        if (devlock_acquire(lock_dir, name, &lock_fd) < 0) {
            if (errno == EBUSY)
                LOG_ERROR_RETURN(EBUSY, -1, "ublk device ` is held by another live server", id);
            return -1;   // devlock_acquire logged it
        }
        return 0;
    }
    void release_lock() {
        devlock_release(lock_fd);
        lock_fd = -1;
    }

    // ----- device nodes -----

    int wait_node(const char* path) {
        for (int i = 0; i < 3000; i++) {
            if (::access(path, F_OK) == 0)
                return 0;
            photon::thread_usleep(1000);
        }
        LOG_ERROR_RETURN(ETIMEDOUT, -1, "device node ` did not appear within 3s", path);
    }

    int open_cdev() {
        char path[64];
        snprintf(path, sizeof(path), "/dev/ublkc%llu", (unsigned long long)dev_id);
        if (wait_node(path) < 0)
            return -1;
        cdev_fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (cdev_fd < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to open ", path);
        return 0;
    }

    // ----- params -----

    // the attrs to register for the CURRENT config; fill_params publishes
    // them, validate_params compares the registered ones against them
    uint32_t want_attrs() const {
        return (read_only ? UBLK_ATTR_READ_ONLY : 0) |
               ((features & FEATURE_FLUSH) ? (UBLK_ATTR_VOLATILE_CACHE | UBLK_ATTR_FUA) : 0);
    }

    void fill_params(ublk_params* p) {
        memset(p, 0, sizeof(*p));
        p->types = UBLK_PARAM_TYPE_BASIC;
        p->basic.attrs = want_attrs();
        p->basic.logical_bs_shift = sector_shift;
        p->basic.physical_bs_shift = sector_shift;
        p->basic.io_opt_shift = sector_shift;
        p->basic.io_min_shift = sector_shift;
        p->basic.max_sectors = max_io_buf_bytes >> sector_shift;
        p->basic.dev_sectors = dev_sectors.load();
        if (features & (FEATURE_DISCARD | FEATURE_WRITE_ZEROES)) {
            p->types |= UBLK_PARAM_TYPE_DISCARD;
            p->discard.discard_granularity = 1u << sector_shift;
            // the commit result is a signed 32-bit BYTE count and a negative
            // result reads as -errno: cap the advertised range so a full-size
            // request still encodes. Any non-negative result completes these ops,
            // so the cap only has to keep the byte count inside int32.
            uint32_t cap = std::min<uint32_t>(1u << 22, (uint32_t)(INT32_MAX >> sector_shift));
            p->discard.max_discard_sectors =
                (features & FEATURE_DISCARD) ? cap : 0;
            p->discard.max_write_zeroes_sectors =
                (features & FEATURE_WRITE_ZEROES) ? cap : 0;
            p->discard.max_discard_segments = 1;   // one contiguous range per request
        }
    }

    // re-attach validates cfg against the existing registration (drift = EINVAL)
    int validate_params() {
        ublk_params p;
        if (ctrl.get_params((uint32_t)dev_id, &p) < 0)
            LOG_ERROR_RETURN(0, -1, "ublk GET_PARAMS failed, dev `", dev_id);
        if (!(p.types & UBLK_PARAM_TYPE_BASIC) ||
            p.basic.dev_sectors != dev_sectors.load() ||
            p.basic.logical_bs_shift != sector_shift ||
            p.basic.attrs != want_attrs())
            LOG_ERROR_RETURN(EINVAL, -1,
                "ublk device ` config drift: registered size ` shift ` attrs `, requested size ` shift ` attrs `",
                dev_id, p.basic.dev_sectors, (int)p.basic.logical_bs_shift,
                (int)p.basic.attrs, dev_sectors.load(), (int)sector_shift, (int)want_attrs());
        return 0;
    }

    // ----- lifecycle -----

    void select_ops() {
        if (!(negotiated_flags & UBLK_F_CMD_IOCTL_ENCODE)) {
            op_fetch = UBLK_IO_FETCH_REQ;   // ancient kernel: legacy opcodes
            op_commit = UBLK_IO_COMMIT_AND_FETCH_REQ;
        }
    }

    void make_queues() {
        for (uint32_t i = 0; i < nr_queues; i++) {
            auto q = new Queue;
            q->d = this;
            q->qid = i;
            queues.push_back(q);
        }
    }

    int create_new(uint32_t want) {
        ublksrv_ctrl_dev_info info;
        memset(&info, 0, sizeof(info));
        info.dev_id = want;   // kernel treats UINT32_MAX as auto-assign
        info.nr_hw_queues = nr_queues;
        info.queue_depth = queue_depth;
        info.max_io_buf_bytes = max_io_buf_bytes;
        info.ublksrv_pid = ::getpid();
        info.flags = UBLK_F_USER_RECOVERY | UBLK_F_USER_RECOVERY_REISSUE |
                     UBLK_F_UPDATE_SIZE | UBLK_F_CMD_IOCTL_ENCODE |
                     UBLK_F_NO_AUTO_PART_SCAN | cfg.flags;
        if (ctrl.add_dev(&info) < 0)
            LOG_ERROR_RETURN(0, -1, "ublk ADD_DEV failed, want `", want);
        set_dev_id(info.dev_id);
        negotiated_flags = info.flags;
        created = true;
        select_ops();
        if (want == UINT32_MAX && acquire_lock((uint32_t)dev_id) < 0)
            return -1;
        if (open_cdev() < 0)
            return -1;
        make_queues();
        if (start_serving() < 0)   // fetches must be in before SET_PARAMS/START
            return -1;
        ublk_params p;
        fill_params(&p);
        if (ctrl.set_params((uint32_t)dev_id, &p) < 0)
            LOG_ERROR_RETURN(0, -1, "ublk SET_PARAMS failed, dev `", dev_id);
        if (ctrl.start_dev((uint32_t)dev_id, ::getpid()) < 0)
            LOG_ERROR_RETURN(0, -1, "ublk START_DEV failed, dev `", dev_id);
        char bpath[64];
        snprintf(bpath, sizeof(bpath), "/dev/ublkb%llu", (unsigned long long)dev_id);
        if (wait_node(bpath) < 0)
            return -1;
        LOG_INFO("ublk device started, ", VALUE(dev_id), VALUE(cfg.info.size),
                 VALUE((int)nr_queues), VALUE((int)queue_depth), bpath);
        return 0;
    }

    int attach_existing() {
        bool ok = false;
        // failing past make_queues() leaves pumps, tag coroutines, rings, the
        // queue buffers and cdev_fd behind, and neither caller can undo it:
        // start() rolls back the whole device (DEL_DEV + release_lock) and
        // shutdown() must go on to TRY_STOP/DEL_DEV a device it never served.
        // So tear the serving side down here. stop_serving() is a no-op on the
        // empty state the earlier failure points leave.
        DEFER(if (!ok) { int e = errno; stop_serving(false); errno = e; });
        // the old daemon's quiesce work runs async after its fds close
        int64_t deadline = cfg.quiesce_timeout_ms;
        while (ctrl.start_recovery((uint32_t)dev_id) < 0) {
            if (errno != EBUSY)
                LOG_ERRNO_RETURN(0, -1, "ublk START_USER_RECOVERY failed, dev `", dev_id);
            if ((deadline -= 1) <= 0)
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "ublk device ` never quiesced", dev_id);
            photon::thread_usleep(1000);
        }
        ublksrv_ctrl_dev_info info;
        if (ctrl.get_info((uint32_t)dev_id, &info) < 0)
            LOG_ERROR_RETURN(0, -1, "ublk GET_DEV_INFO failed, dev `", dev_id);
        // adopt the existing queue geometry (it was fixed at ADD_DEV)
        nr_queues = info.nr_hw_queues;
        queue_depth = info.queue_depth;
        max_io_buf_bytes = info.max_io_buf_bytes;
        negotiated_flags = info.flags;
        select_ops();
        if (validate_params() < 0)
            return -1;
        if (open_cdev() < 0)
            return -1;
        make_queues();
        if (start_serving() < 0)
            return -1;
        // END_USER_RECOVERY waits for all fetches and flips the device LIVE
        if (ctrl.end_recovery((uint32_t)dev_id, ::getpid()) < 0)
            LOG_ERROR_RETURN(0, -1, "ublk END_USER_RECOVERY failed, dev `", dev_id);
        LOG_INFO("ublk device recovered, ", VALUE(dev_id), VALUE(cfg.info.size));
        ok = true;
        return 0;
    }

    static bool parse_u32(const std::string& s, uint32_t* out) {
        if (s.empty()) return false;
        char* end = nullptr;
        unsigned long v = strtoul(s.c_str(), &end, 10);
        if (!end || *end || v > UINT32_MAX) return false;
        *out = (uint32_t)v;
        return true;
    }

    int start(fs::IFile* bk, bool ownership) override {
        if (started)
            LOG_ERROR_RETURN(EALREADY, -1, "ublk device already started");
        if (!bk)
            LOG_ERROR_RETURN(EINVAL, -1, "backend IFile is null");

        backend = bk;
        own_backend = ownership;

        // ublk has no uuid: the dev_id IS the identity. cfg.dev_id wins; a
        // decimal identity is the fallback (so a cfg built from a BlkDevInfo
        // returned by the controller's list_orphans() works). The recovery path
        // below adopts the registered geometry, which is authoritative over the
        // derived one.
        uint32_t want = cfg.dev_id;
        if (want == UINT32_MAX)
            parse_u32(cfg.info.identity, &want);

        bool ok = false;
        DEFER(if (!ok) { int e = errno; rollback(); errno = e; });

        if (ctrl.init() < 0)
            return -1;

        bool recover = false;
        if (want != UINT32_MAX) {
            if (acquire_lock(want) < 0)
                return -1;
            ublksrv_ctrl_dev_info info;
            if (ctrl.get_info(want, &info) == 0) {
                // the quiesce after a detach is async (ublk_ch_release
                // schedules the quiesce work): LIVE right after our OWN
                // detach is transient. We hold the flock, so no live photon
                // daemon exists; a device that stays LIVE past the deadline
                // belongs to a foreign daemon.
                int64_t deadline = cfg.quiesce_timeout_ms;
                while (info.state == UBLK_S_DEV_LIVE) {
                    if ((deadline -= 1) <= 0)
                        LOG_ERROR_RETURN(EBUSY, -1, "ublk device ` has a live daemon", want);
                    photon::thread_usleep(1000);
                    if (ctrl.get_info(want, &info) < 0)
                        LOG_ERROR_RETURN(0, -1, "ublk GET_DEV_INFO failed, dev `", want);
                }
                if (info.state == UBLK_S_DEV_QUIESCED || info.state == UBLK_S_DEV_FAIL_IO) {
                    recover = true;
                } else {
                    // DEAD: a half-created residue (never started); remove and
                    // create fresh
                    if (ctrl.del_dev(want) < 0)
                        LOG_ERROR_RETURN(0, -1, "ublk DEL_DEV of a half-created residue failed, dev `", want);
                }
            } else if (errno != ENODEV) {
                LOG_ERROR_RETURN(0, -1, "ublk GET_DEV_INFO failed, dev `", want);
            }
        }

        if (recover)
            set_dev_id(want);
        int ret = recover ? attach_existing() : create_new(want);
        if (ret < 0)
            return -1;

        started = true;
        ok = true;
        return 0;
    }

    // stop serving, keep the registration: the device quiesces and a later
    // start() (possibly another process) re-attaches via START_USER_RECOVERY
    int detach(bool wait_pending) override {
        if (!started)
            return 0;
        stop_serving(wait_pending);
        release_lock();
        started = false;
        // the flock was the ownership claim: once released, a later start()
        // (this object or another process) must treat the registration as
        // foreign -- a failed re-attach may not DEL_DEV it (rollback keys
        // off `created`), and shutdown() must re-claim the lock first
        created = false;
        // the kernel quiesces asynchronously (quiesce work off the cdev
        // release); wait it out so a follow-on start() / list_orphans()
        // sees the final QUIESCED state instead of racing the transition
        ublksrv_ctrl_dev_info info;
        bool quiesced = false;
        for (uint32_t waited = 0; ; waited++) {
            if (ctrl.get_info((uint32_t)dev_id, &info) == 0 &&
                (info.state == UBLK_S_DEV_QUIESCED || info.state == UBLK_S_DEV_FAIL_IO)) {
                quiesced = true;
                break;
            }
            if (waited >= cfg.quiesce_timeout_ms)
                break;
            photon::thread_usleep(1000);
        }
        if (!quiesced)
            LOG_WARN("ublk device ` did not quiesce within ` ms", dev_id,
                     cfg.quiesce_timeout_ms);
        LOG_INFO("ublk device detached, ", VALUE(dev_id), "flush=", (int)wait_pending);
        return 0;
    }

    int shutdown() override {
        if (!started && dev_id < 0)
            return 0;
        if (!started) {
            // detached: since release_lock(), another server may have adopted
            // the registration. DEL_DEV is unconditional (no EBUSY, no state
            // check) and TRY_STOP only guards against open initiators, so
            // without re-claiming the flock we could destroy a LIVE device
            // that is not ours anymore. EBUSY = it belongs to that server now.
            if (acquire_lock((uint32_t)dev_id) < 0) {
                if (errno != EBUSY)
                    return -1;   // already logged; leave the device alone
                LOG_INFO("ublk device ` is owned by another server now; leaving it alone", dev_id);
                set_dev_id(-1);
                return 0;
            }
            // re-serve before tearing down (tcmu's settled shutdown-after-
            // detach design): the quiesce REISSUEs in-flight IO and traps
            // whoever had it outstanding -- a udev probe caught mid-read
            // sits in D state holding the disk open, so disk_openers() never
            // drops and TRY_STOP spins EBUSY forever; only a live daemon
            // completes that IO. A genuine holder (mounted fs) survives the
            // re-serve and still gets the contractual EBUSY below. If the
            // re-attach fails, fall through and stop the quiesced device
            // directly (best effort).
            if (attach_existing() == 0)
                started = true;
        }
        if (dev_id >= 0) {
            // EBUSY if the initiator holds the disk: propagate with nothing
            // torn down. But transient openers (the kernel's async partition
            // scan, udev's probe) hold the disk briefly right after start --
            // ride them out with a bounded retry; a real holder (mounted fs,
            // open fd) never releases, so EBUSY after the deadline is genuine.
            uint32_t max_retry = cfg.stop_timeout_ms / 10;   // 10ms granularity
            for (uint32_t i = 0; ; i++) {
                if (ctrl.try_stop_dev((uint32_t)dev_id) == 0)
                    break;
                if (errno == EOPNOTSUPP) {   // no SAFE_STOP_DEV: plain stop
                    // Do not swallow this: a device that failed to stop is still
                    // live, so DEL_DEV below would fail with EBUSY and hand the
                    // caller an error that points at the wrong step.
                    if (ctrl.stop_dev((uint32_t)dev_id) < 0 && errno != ENODEV)
                        LOG_ERRNO_RETURN(0, -1, "ublk STOP_DEV failed, dev `", dev_id);
                    break;
                }
                if (errno != EBUSY)
                    break;   // ENODEV (never started) etc: proceed to DEL_DEV
                if (i >= max_retry) {
                    // a started device keeps serving (and keeps the lock);
                    // a detached one was only claimed for this teardown --
                    // give the lock back so orphan recovery is not blocked
                    // while we do nothing
                    if (!started)
                        release_lock();
                    LOG_ERROR_RETURN(EBUSY, -1, "ublk device ` is still in use (held open / mounted)",
                                     dev_id);
                }
                photon::thread_usleep(10 * 1000);
            }
        }
        if (started) {
            stop_serving(true);
            started = false;   // before any early return below can skip it
        }
        if (dev_id >= 0 && ctrl.del_dev((uint32_t)dev_id) < 0 && errno != ENODEV)
            // ENODEV means the registration is already gone, which is the goal.
            // Anything else means it SURVIVES, so blk.h's shutdown() contract
            // ("detach() + destroy the kernel-side registration") is not met:
            // propagate rather than report success, and keep both dev_id and the
            // flock. The lock is the only ownership test there is -- handing it
            // back while /dev/ublkbN still exists would let orphan recovery
            // claim a device the kernel still has. vduse's DESTROY_DEV path does
            // the same.
            LOG_ERRNO_RETURN(0, -1, "ublk DEL_DEV failed, dev `", dev_id);
        release_lock();
        created = false;
        set_dev_id(-1);
        return 0;
    }

    // Grow the device (UBLK_U_CMD_UPDATE_SIZE); shrink is rejected. The kernel
    // revalidates the initiator's cached capacity itself
    // (set_capacity_and_notify). Works from the detached (quiesced) state too.
    int resize(uint64_t new_size) override {
        if (dev_id < 0)
            LOG_ERROR_RETURN(ENODEV, -1, "ublk resize: no registration (virgin or shut down)");
        uint64_t bs = 1ull << sector_shift;
        if (new_size % bs)
            LOG_ERROR_RETURN(EINVAL, -1, "resize size ` is not a multiple of the `-byte sector",
                             new_size, bs);
        uint64_t cur = dev_sectors.load() * bs;
        if (new_size == cur)
            return 0;
        if (new_size < cur)
            LOG_ERROR_RETURN(EINVAL, -1, "ublk resize: shrink (` -> `) is rejected", cur, new_size);
        if (ctrl.update_size((uint32_t)dev_id, new_size >> sector_shift) < 0)
            LOG_ERROR_RETURN(0, -1, "ublk UPDATE_SIZE failed, dev `", dev_id);
        dev_sectors = new_size >> sector_shift;
        cfg.info.size = new_size;
        LOG_INFO("ublk device resized, ", VALUE(dev_id), VALUE(cur), VALUE(new_size));
        return 0;
    }

    // set_dev_id() keeps this in step with dev_id, so it is a pure read: safe to
    // hold the pointer across another call, as blk.h promises
    const char* get_device_node() override {
        return node_path[0] ? node_path : nullptr;
    }

    void rollback() {
        stop_serving(false);   // no-op-safe on empty queues
        if (created && dev_id >= 0)
            ctrl.del_dev((uint32_t)dev_id);   // best effort
        created = false;
        release_lock();
        set_dev_id(-1);
        started = false;
        // virgin state includes backend ownership: a failed start leaves the
        // backend with the caller (else the destructor and the caller's own
        // cleanup would both delete it)
        backend = nullptr;
        own_backend = false;
    }

    char node_path[64] = {};
};

struct UblkControllerImpl : UblkController {
    char lock_dir[SCOPE_DIR_BUF] = {};   // "" = /run/photon-blk, normalized by devlock_*

    explicit UblkControllerImpl(const char* ld) {
        if (ld)
            snprintf(lock_dir, sizeof(lock_dir), "%s", ld);   // bounded: the factory checked
    }

    IBlkDevice* new_device(const UblkController::Config& cfg) override {
        if (UblkDeviceImpl::validate(cfg) < 0)
            return nullptr;
        return new UblkDeviceImpl(cfg, lock_dir);
    }

    // Enumerate orphaned ublk devices: a /dev/ublkcN exists, the device is
    // QUIESCED (its daemon died), and no live server holds its flock. The lock
    // file existing at all means a photon-blk daemon created it (ublksrv devices
    // are not listed). The returned identity is the decimal dev_id -- the recovery
    // key (ublk has no uuid).
    std::vector<BlkDevInfo> list_orphans() override {
        std::vector<BlkDevInfo> ret;
        DIR* dd = ::opendir("/dev");
        if (!dd)
            return ret;
        DEFER(::closedir(dd));
        UblkCtrl ctrl;
        // init() logs its own cause. What it cannot log is the consequence,
        // and the consequence is the whole problem: the empty vector this
        // returns then reads exactly like "this host has no orphans" -- the
        // answer a recovery run decides whether to adopt on. There is no error
        // channel to distinguish them with: the signature returns a vector.
        if (ctrl.init() < 0) {
            LOG_ERROR("ublk: the orphan scan did not run -- an empty result does not mean this host has no orphans");
            return ret;
        }
        DEFER(ctrl.fini());
        struct dirent* e;
        while ((e = readdir(dd))) {
            if (strncmp(e->d_name, "ublkc", 5) != 0 || !isdigit(e->d_name[5]))
                continue;
            uint32_t id = (uint32_t)atoi(e->d_name + 5);
            // probe the flock first (cheap): free => no live server. Read-only
            // open, no O_CREAT: a query must not create files. This walks a
            // kernel-enumerated namespace, so a device with no tombstone of
            // ours is the norm rather than an inconsistency -- it is somebody
            // else's ublk device, and "not ours to list" is the whole answer.
            char name[32];
            snprintf(name, sizeof(name), "ublk-%u.lock", id);
            if (devlock_free(lock_dir, name) != 1)
                continue;
            ublksrv_ctrl_dev_info info;
            if (ctrl.get_info(id, &info) < 0)
                continue;
            if (info.state != UBLK_S_DEV_QUIESCED && info.state != UBLK_S_DEV_FAIL_IO)
                continue;
            ublk_params p;
            if (ctrl.get_params(id, &p) < 0 || !(p.types & UBLK_PARAM_TYPE_BASIC))
                continue;
            BlkDevInfo bi;
            bi.identity = std::to_string(id);
            bi.size = p.basic.dev_sectors << p.basic.logical_bs_shift;
            bi.sector_size_shift = (uint8_t)p.basic.logical_bs_shift;
            bi.features = 0;
            if (p.types & UBLK_PARAM_TYPE_DISCARD) {
                if (p.discard.max_discard_sectors) bi.features |= FEATURE_DISCARD;
                if (p.discard.max_write_zeroes_sectors) bi.features |= FEATURE_WRITE_ZEROES;
            }
            if (p.basic.attrs & (UBLK_ATTR_VOLATILE_CACHE | UBLK_ATTR_FUA))
                bi.features |= FEATURE_FLUSH;
            ret.push_back(bi);
        }
        return ret;
    }

    // Remove one orphan: the kernel-side registration, then the tombstone.
    //
    // The identity is caller-supplied and names something to DELETE, and the dev_id
    // space is host-wide and flat, so it gets a stricter parse than list_orphans()
    // needs. That one reads a kernel-enumerated /dev dirent it has already checked
    // with isdigit(); this one reads whatever the caller wrote, and atoi() turns
    // "garbage" into 0 -- a valid dev_id belonging to somebody else.
    //
    // Registration first, tombstone second, so a failure leaves something a later
    // scan still reports. The other way round would delete the tombstone of a device
    // DEL_DEV then failed on, and list_orphans() skips an entry whose tombstone is
    // missing -- the orphan would vanish from every scan while still being in the
    // kernel.
    int destroy_orphan(const BlkDevInfo& orphan) override {
        const std::string& id = orphan.identity;
        if (id.empty())
            LOG_ERROR_RETURN(EINVAL, -1, "ublk destroy_orphan needs a non-empty dev_id");
        // Digits throughout, which is what rejects the two spellings the numeric
        // parse below would otherwise accept: strtoul skips leading white space and
        // takes an optional sign, both measured, so " 12" and "+12" would name
        // device 12. Leading zeros are allowed -- "0012" is another spelling of the
        // same device, and the lock name and DEL_DEV both go through %u.
        for (char c : id)
            if (!isdigit((unsigned char)c))
                LOG_ERROR_RETURN(EINVAL, -1, "ublk identity ` is not a decimal dev_id", id);
        uint32_t want = 0;
        if (!UblkDeviceImpl::parse_u32(id, &want))
            LOG_ERROR_RETURN(EINVAL, -1, "ublk dev_id ` does not fit in 32 bits", id);
        // UINT32_MAX is Config::dev_id's "let the kernel choose" sentinel, so it is
        // not a device this call may be pointed at.
        if (want == UINT32_MAX)
            LOG_ERROR_RETURN(EINVAL, -1, "ublk dev_id ` is the auto-assign sentinel, not a device", want);
        char name[32];
        snprintf(name, sizeof(name), "ublk-%u.lock", want);

        UblkCtrl ctrl;
        if (ctrl.init() < 0)
            return -1;   // init() logged it
        // fini() deletes the io_uring engine and closes the control fd on the way
        // out, and that teardown overwrites errno -- measured: an EBUSY return
        // reached the caller as errno -1, a value no syscall sets. The io_uring
        // wrapper is the one place in the tree that can produce a negative errno,
        // assigning it from a completion result. start() guards the same hazard
        // around rollback(); this guards it around fini(), so that the errno named
        // in this API's contract is the one the caller actually receives.
        DEFER({ int e = errno; ctrl.fini(); errno = e; });
        ublksrv_ctrl_dev_info info;
        if (ctrl.get_info(want, &info) < 0) {
            // No registration, so there is no orphan to recover. devlock_free()'s
            // three answers still have to be told apart, and only one of them is
            // litter this call may take away:
            //   -1  nothing there at all -- the dev_id names no device this host
            //       ever had, so there is nothing to remove and nothing to report.
            //    0  a LIVE SERVER holds the tombstone, which with no registration
            //       means it is between claiming the name and ADD_DEV. Unlinking it
            //       would leave the device that server then creates with no
            //       tombstone, and list_orphans() skips a registration whose
            //       tombstone is missing -- so it would become unrecoverable by
            //       every later scan. Same hazard the ordering above avoids.
            //    1  free: a tombstone outliving its device is our own litter, and it
            //       goes with it.
            int lf = devlock_free(lock_dir, name);
            if (lf < 0)
                LOG_ERROR_RETURN(ENOENT, -1, "no ublk device ` and no tombstone for it that this call could use", want);
            if (lf == 0)
                LOG_ERROR_RETURN(EBUSY, -1, "no ublk device ` yet, but a live server holds its tombstone", want);
            if (devlock_unlink(lock_dir, name) < 0)
                return -1;   // devlock_unlink logged it
            return 0;
        }
        // CLAIM the flock rather than probe it, and hold it across the DEL_DEV:
        // DEL_DEV is unconditional, so a probe would leave a window in which
        // another daemon adopts the device and this destroys a live one. This is
        // the gate UblkDeviceImpl::shutdown() re-claims the lock for.
        int lock_fd = -1;
        if (devlock_acquire(lock_dir, name, &lock_fd) < 0) {
            if (errno == EBUSY)
                LOG_ERROR_RETURN(EBUSY, -1, "ublk device ` is held by another live server; leaving it alone", want);
            return -1;   // devlock_acquire logged it
        }
        // Declared after the fini() DEFER above, so it unwinds first and the claim is
        // dropped on EVERY exit path rather than only the one at the bottom.
        // shutdown() does the opposite on a failed DEL_DEV and says why: it is a live
        // owner that goes on existing, and dropping the claim while /dev/ublkbN
        // survives would let recovery take a device the kernel still has. This call
        // is the other shape -- a one-shot from a controller that stores no fd -- so
        // "keep it" is a leak, and a leaked claim costs more than the ownership
        // window shutdown() guards: devlock_free() then reports 0 and list_orphans()
        // skips the entry, so the device stops being reported by every later scan
        // while still being in the kernel. Giving the claim back leaves it listed and
        // retryable. Same errno guard as above, because devlock_release() calls
        // flock() and close() and either can set errno.
        DEFER({ int e = errno; devlock_release(lock_fd); errno = e; });
        // UNBOUNDED, and that is the kernel's doing rather than a missing knob: with
        // an initiator still holding /dev/ublkbN, DEL_DEV takes the node away and then
        // waits for the last opener to close before returning success. Measured on a
        // quiesced orphan whose holder issued nothing to it: 227s, then 0 -- and at
        // once, when that holder was killed. It blocks this coroutine, not the vcpu;
        // the wait is a kernel wait on an io_uring worker while the vcpu stays in its
        // event loop. blk.h tells the caller, who is the one that has to decide
        // whether it can afford to wait.
        if (ctrl.del_dev(want) < 0 && errno != ENODEV)
            // ENODEV means the registration is already gone, which is the goal.
            // Anything else means it SURVIVES, so this did not do what it claims.
            LOG_ERRNO_RETURN(0, -1, "ublk DEL_DEV failed, dev `", want);
        // Unlinked while the lock is still held -- the release is deferred to scope
        // exit, after this statement -- so nobody else can have created this file in
        // the meantime, and what goes is provably the tombstone this call opened.
        // Releasing first would let another daemon create it in between and this
        // would then delete theirs.
        if (devlock_unlink(lock_dir, name) < 0)
            return -1;   // devlock_unlink logged it
        return 0;
    }
};

UblkController* new_ublk_controller(const char* lock_dir) {
    if (validate_scope_dir(lock_dir) < 0)
        return nullptr;   // already logged
    return new UblkControllerImpl(lock_dir);
}

}  // namespace blk
}  // namespace photon
