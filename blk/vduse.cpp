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

// VDUSE transport (the kernel's vduse module; the ABI is fixed by
// <linux/vduse.h>): exports a photon IFile as a virtio-blk device on the vdpa
// bus. Unlike nbd/tcmu/ublk there is no block device of our own -- start()
// creates the VDUSE registration (a char device /dev/vduse/<name> carrying the
// virtio device model), and an EXTERNAL consumer attaches it to a /dev/vdX:
// `vdpa dev add mgmtdev vduse name <name>` (+ the virtio_vdpa driver) locally,
// or QEMU's vhost-vdpa for a guest. The identity is the device name; there is
// no config readback ioctl, so an orphan record carries the name only
// (tombstone, like vhost-user).
//
// Protocol (validated live against kernel 7.0 + the probe/rescue runs):
// - /dev/vduse/control: VDUSE_{GET,SET}_API_VERSION (we use 0), VDUSE_CREATE_DEV
//   (dup name -> EEXIST = the attach signal), VDUSE_DESTROY_DEV (EBUSY while a
//   vdpa consumer is attached or a daemon is connected -- the contractual
//   "initiator holds it").
// - /dev/vduse/<name> is SINGLE-OPENER: a second open() -> EBUSY. The char dev
//   IS the daemon lock -- no flock needed; orphan = the node exists and opens.
// - read(2) delivers kernel requests (SET_STATUS / UPDATE_IOTLB /
//   GET_VQ_STATE), write(2) answers them (response.reserved MUST be zero).
//   The kernel blocks the requesting op for msg_timeout seconds (per-device
//   sysfs /sys/class/vduse/<name>/msg_timeout, default 30 = BlkConfig::timeout)
//   and marks the device PERMANENTLY broken on expiry -- the message loop must
//   be a dedicated coroutine that never blocks on backend IO.
// - Consumer attach drives SET_STATUS 0 -> ACK -> DRIVER -> FEATURES_OK (after
//   which DEV_GET_FEATURES returns the negotiated subset) -> DRIVER_OK (after
//   which VQ_GET_INFO reports ready=1 with the vring IOVAs). No UPDATE_IOTLB
//   arrives for the initial mappings: IOVAs resolve lazily via IOTLB_GET_FD
//   (the ioctl RETURNS the new fd; mmap(fd, entry.offset) covers
//   [entry.start, entry.last]; close(fd) afterwards keeps the mapping).
//   Request data lands in the kernel's bounce region (low 64MB IOVAs); the
//   vring itself is direct-mapped.
// - VQ kicks arrive on the per-vq kickfd (VDUSE_VQ_SETUP_KICKFD); completions
//   go back via the used ring + VDUSE_VQ_INJECT_IRQ. Which completions actually
//   inject is the shared engine's call: without VIRTIO_RING_F_EVENT_IDX it is
//   the driver's avail->flags low bit, with it the used_event index at the end
//   of the avail ring (§2.7.7.2) and the flags bit must be ignored. Symmetrically
//   we publish avail_event at the end of the used ring to tell the driver which
//   kicks we want (§2.7.10.1).
// - Daemon death leaves the registration AND the vdpa binding intact; pending
//   vring requests survive and ANY consumer touching /dev/vdX blocks in D
//   state until a new daemon adopts the char dev and serves the backlog
//   (resync = VQ_GET_INFO + restart from used->idx -- validated by rescue).
//   Therefore: a consumer must be detached (vdpa dev del) before the serving
//   stops, or the device wedges its users.
//
// P1 scope: one virtqueue (no VIRTIO_BLK_F_MQ), split ring only (no
// RING_PACKED / INDIRECT -- not offered, so the driver must not use them),
// IN/OUT/FLUSH/GET_ID requests, FEATURE_FLUSH + read_only + logical block
// size; FEATURE_DISCARD/WRITE_ZEROES are accepted in cfg.info.features but not
// offered yet. Serving runs on the caller's vcpu (BlkConfig::vcpus/queues are
// not honored yet). All virtio fields are little-endian (VERSION_1) and this
// file, like nbd/tcmu/ublk, assumes an LE host (x86_64/aarch64).

#include "blk.h"
#include "utils.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace photon {
namespace blk {

// ----------------------------------------------------------------------------
// VDUSE UAPI subset (verbatim from <linux/vduse.h>)
// ----------------------------------------------------------------------------

#define VDUSE_BASE 0x81

#define VDUSE_API_VERSION   0

#define VDUSE_GET_API_VERSION   _IOR(VDUSE_BASE, 0x00, uint64_t)
#define VDUSE_SET_API_VERSION   _IOW(VDUSE_BASE, 0x01, uint64_t)

#define VDUSE_NAME_MAX 256

struct vduse_dev_config {
    char name[VDUSE_NAME_MAX];
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t features;
    uint32_t vq_num;
    uint32_t vq_align;
    uint32_t ngroups;   // api version >= 1 only
    uint32_t nas;       // api version >= 1 only
    uint32_t reserved[11];
    uint32_t config_size;
    uint8_t config[];
};

#define VDUSE_CREATE_DEV      _IOW(VDUSE_BASE, 0x02, struct vduse_dev_config)
#define VDUSE_DESTROY_DEV     _IOW(VDUSE_BASE, 0x03, char[VDUSE_NAME_MAX])

struct vduse_iotlb_entry {
    uint64_t offset;    // mmap offset on the returned fd
    uint64_t start;
    uint64_t last;
#define VDUSE_ACCESS_RO 0x1
#define VDUSE_ACCESS_WO 0x2
#define VDUSE_ACCESS_RW 0x3
    uint8_t perm;
};
#define VDUSE_IOTLB_GET_FD    _IOWR(VDUSE_BASE, 0x10, struct vduse_iotlb_entry)

#define VDUSE_DEV_GET_FEATURES    _IOR(VDUSE_BASE, 0x11, uint64_t)

struct vduse_config_data {
    uint32_t offset;
    uint32_t length;
    uint8_t buffer[];
};
#define VDUSE_DEV_SET_CONFIG          _IOW(VDUSE_BASE, 0x12, struct vduse_config_data)
#define VDUSE_DEV_INJECT_CONFIG_IRQ   _IO(VDUSE_BASE, 0x13)

struct vduse_vq_config {
    uint32_t index;
    uint16_t max_size;
    uint16_t reserved1;
    uint32_t group;
    uint16_t reserved2[10];
};
#define VDUSE_VQ_SETUP    _IOW(VDUSE_BASE, 0x14, struct vduse_vq_config)

struct vduse_vq_state_split  { uint16_t avail_index; };
struct vduse_vq_state_packed { uint16_t last_avail_counter, last_avail_idx,
                                       last_used_counter, last_used_idx; };

struct vduse_vq_info {
    uint32_t index;
    uint32_t num;
    uint64_t desc_addr;
    uint64_t driver_addr;    // the avail ring
    uint64_t device_addr;    // the used ring
    union {
        struct vduse_vq_state_split split;
        struct vduse_vq_state_packed packed;
    };
    uint8_t ready;
};
#define VDUSE_VQ_GET_INFO   _IOWR(VDUSE_BASE, 0x15, struct vduse_vq_info)

struct vduse_vq_eventfd {
    uint32_t index;
#define VDUSE_EVENTFD_DEASSIGN -1
    int fd;
};
#define VDUSE_VQ_SETUP_KICKFD   _IOW(VDUSE_BASE, 0x16, struct vduse_vq_eventfd)
#define VDUSE_VQ_INJECT_IRQ     _IOW(VDUSE_BASE, 0x17, uint32_t)

enum vduse_req_type {
    VDUSE_GET_VQ_STATE,
    VDUSE_SET_STATUS,
    VDUSE_UPDATE_IOTLB,
    VDUSE_SET_VQ_GROUP_ASID,
};

struct vduse_vq_state {
    uint32_t index;
    union {
        struct vduse_vq_state_split split;
        struct vduse_vq_state_packed packed;
    };
};
struct vduse_dev_status { uint8_t status; };
struct vduse_iova_range { uint64_t start, last; };

struct vduse_dev_request {
    uint32_t type;
    uint32_t request_id;
    uint32_t reserved[4];
    union {
        struct vduse_vq_state vq_state;
        struct vduse_dev_status s;
        struct vduse_iova_range iova;
        uint32_t padding[32];
    };
};

struct vduse_dev_response {
    uint32_t request_id;
#define VDUSE_REQ_RESULT_OK     0x00
#define VDUSE_REQ_RESULT_FAILED 0x01
    uint32_t result;
    uint32_t reserved[4];    // the kernel rejects a nonzero reserved area
    union {
        struct vduse_vq_state vq_state;
        uint32_t padding[32];
    };
};

// ----------------------------------------------------------------------------

static constexpr uint32_t DEFAULT_VQ_SIZE = 256;
static constexpr uint32_t MAX_VQ_SIZE = 1024;

// IOVA -> VA cache over VDUSE_IOTLB_GET_FD. Ranges are few (the vring's direct
// map + the kernel's bounce region); a linear scan is fine.
struct Iotlb {
    int dev_fd = -1;
    struct Map {
        uint64_t start, last;
        char* base;
    };
    std::vector<Map> maps;
    std::vector<Map> stale;   // invalidated but not yet munmapped

    void* resolve(uint64_t iova, size_t len) {
        // Wrap-free, and the cache is the only place that needs it: a wrapped
        // `iova + len - 1` in the ioctl below produces an end below its start,
        // which the ioctl rejects -- but a cache HIT never reaches the ioctl, so
        // the old endpoint form handed out base + (iova - m.start) for a
        // guest-written desc.addr near UINT64_MAX: an arbitrarily negative offset
        // into our own mappings. iova <= m.last makes the subtraction below safe.
        for (auto& m : maps)
            if (iova >= m.start && iova <= m.last && len <= m.last - iova + 1)
                return m.base + (iova - m.start);
        vduse_iotlb_entry e;
        memset(&e, 0, sizeof(e));
        e.start = iova;
        e.last = iova + len - 1;
        int fd = (int)::ioctl(dev_fd, VDUSE_IOTLB_GET_FD, &e);
        if (fd < 0)
            LOG_ERRNO_RETURN(0, nullptr, "vduse IOTLB_GET_FD failed, iova ` len `", iova, len);
        size_t sz = (size_t)(e.last - e.start + 1);
        int prot = PROT_READ;
        if (e.perm & VDUSE_ACCESS_WO) prot = PROT_WRITE;
        if (e.perm & VDUSE_ACCESS_RO) prot |= PROT_READ;
        void* base = ::mmap(nullptr, sz, prot, MAP_SHARED, fd, (off_t)e.offset);
        ::close(fd);   // the mapping keeps its own reference
        if (base == MAP_FAILED)
            LOG_ERRNO_RETURN(0, nullptr, "vduse iotlb mmap failed, iova ` map [`,`] off `",
                             iova, e.start, e.last, e.offset);
        maps.push_back(Map{e.start, e.last, (char*)base});
        return (char*)base + (iova - e.start);
    }

    // A range's mapping changed (UPDATE_IOTLB): drop it from the lookup so the
    // next resolve re-fetches, but do NOT munmap yet -- request coroutines may
    // still hold VAs into it; the unmap waits for the in-flight drain (flush).
    // start==last==0 is the driver's unmap-all (vq teardown).
    void invalidate(uint64_t start, uint64_t last) {
        for (size_t i = maps.size(); i-- > 0; ) {
            auto& m = maps[i];
            if (m.start > last || m.last < start)
                continue;
            stale.push_back(m);
            maps.erase(maps.begin() + i);
        }
    }
    void flush_stale() {
        for (auto& m : stale)
            ::munmap(m.base, (size_t)(m.last - m.start + 1));
        stale.clear();
    }
    void clear() {
        invalidate(0, UINT64_MAX);
        flush_stale();
    }
};

// The tombstone name of a vduse device inside its controller's lock_dir. A device
// name may run to VDUSE_NAME_MAX-1 = 255 chars, which the prefix and suffix would
// push past the 255-byte NAME_MAX, so such a name locks under an FNV-1a digest
// instead. This is the ONLY definition of the mapping, which is what keeps the
// device's acquire_lock() and the controller's list_orphans() in agreement; the
// digest is stable across processes of this build, not across implementations.
static constexpr size_t VDUSE_LOCK_NAME_MAX = 255 - 6 - 5;   // "vduse-" + ".lock"
static constexpr size_t VDUSE_LOCK_BUF = 256;                // fits the longest

static void vduse_lock_name(const char* name, char* buf, size_t n) {
    if (strlen(name) <= VDUSE_LOCK_NAME_MAX) {
        // the precision only bounds what the compiler has to assume; a name that
        // passed the check above is never actually cut
        snprintf(buf, n, "vduse-%.*s.lock", (int)VDUSE_LOCK_NAME_MAX, name);
        return;
    }
    uint64_t h = 14695981039346656037ULL;   // FNV-1a, 64-bit
    for (const char* p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    snprintf(buf, n, "vduse-%016llx.lock", (unsigned long long)h);
}

struct VduseDeviceImpl : IBlkDevice {
    BlkConfig cfg;
    fs::IFile* backend = nullptr;
    bool own_backend = false;
    bool started = false;
    bool created = false;       // we CREATE_DEV'd it (vs adopted an orphan)

    // log as (const char*), never VALUE(): alog would emit all VDUSE_NAME_MAX bytes
    char name[VDUSE_NAME_MAX] = {};
    int ctrl_fd = -1;
    int dev_fd = -1;
    int lock_fd = -1;           // the tombstone claim inside the controller's lock_dir

    uint64_t offer_features = 0;       // what CREATE_DEV advertised
    uint64_t negotiated = 0;           // DEV_GET_FEATURES after FEATURES_OK
    uint8_t  sector_shift = 9;
    bool     read_only = false;
    uint64_t capacity_sectors = 0;     // 512-byte units, the virtio constant
    uint8_t  dev_status = 0;           // last SET_STATUS

    Iotlb iotlb;
    photon::thread* msg_th = nullptr;
    bool stopping = false;
    bool vq_needs_refresh = false;   // DRIVER_OK seen; the vq loop resolves

    // the shared serving engine (ring state, dispatch, completion, drain);
    // P1 drives a single virtqueue
    VirtQueueServer vq;
    struct VqVduse {
        bool ready = false;           // the ring is resolved and dispatch may run
        bool reset_pending = false;   // a status-0 reset: zero the ring counters
                                      // at the next refresh (vs adoption resume)
    } vqx;
    photon::thread* vq_th = nullptr;

    // A kernel registration exists under `name` and shutdown() may destroy it.
    // `name` itself is permanent identity now (fixed at construction), so it can
    // no longer double as this marker the way it used to.
    bool registered = false;

    // The scope directory this device claims its tombstone in, handed over by the
    // VduseController that built it. COPIED, not borrowed: a device may outlive
    // its controller.
    char lock_dir[SCOPE_DIR_BUF] = {};

    VduseDeviceImpl(const BlkConfig& c, const char* ldir) : cfg(c) {
        if (ldir)
            snprintf(lock_dir, sizeof(lock_dir), "%s", ldir);   // bounded: the factory checked
        sector_shift = cfg.info.sector_size_shift;
        read_only = cfg.read_only;
        capacity_sectors = cfg.info.size >> 9;
        snprintf(name, sizeof(name), "%s", cfg.info.identity.c_str());

        offer_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_F_ACCESS_PLATFORM) |
                         (1ULL << VIRTIO_RING_F_EVENT_IDX) |
                         (1ULL << VIRTIO_BLK_F_BLK_SIZE);
        if (cfg.info.features & FEATURE_FLUSH)
            offer_features |= (1ULL << VIRTIO_BLK_F_FLUSH);
        if (read_only)
            offer_features |= (1ULL << VIRTIO_BLK_F_RO);
        // FEATURE_DISCARD / FEATURE_WRITE_ZEROES: accepted but not offered yet
    }

    // pure config validation -- no I/O, no kernel access. The factory runs it
    // before constructing, so a constructed device is always config-valid.
    static int validate(const BlkConfig& c) {
        if (c.info.identity.empty() || c.info.identity.size() >= VDUSE_NAME_MAX)
            LOG_ERROR_RETURN(EINVAL, -1, "vduse identity must be 1..255 chars (the device name)");
        if (c.info.identity.find('/') != std::string::npos)
            LOG_ERROR_RETURN(EINVAL, -1, "vduse identity must not contain '/'");
        if (validate_info(c.info, /*virtio=*/true) < 0)
            return -1;
        return 0;
    }

    const BlkDevInfo& get_info() const override { return cfg.info; }

    // Always nullptr: the /dev/vdX is created by an EXTERNAL consumer (`vdpa dev
    // add mgmtdev vduse`, or QEMU's vhost-vdpa) asynchronously and outside our
    // control, so it is not ours to name. A caller that needs it must discover
    // it -- test-vduse diffs /sys/block across the attach.
    const char* get_device_node() override { return nullptr; }

    ~VduseDeviceImpl() {
        if (started || dev_fd >= 0) {
            if (shutdown() < 0 && started) {
                // never leave a live consumer served by a dying backend:
                // stop serving (the consumer wedges until adoption -- the
                // documented vduse failure mode) instead of corrupting it.
                // shutdown() kept the tombstone because we were still serving;
                // now that we are not, it has to go or recovery is blocked.
                stop_serving(true);
                release_lock();
                started = false;
            }
        }
        if (own_backend)
            delete backend;
        if (ctrl_fd >= 0) { ::close(ctrl_fd); ctrl_fd = -1; }
    }

    // ----- the message loop (must stay prompt: msg_timeout bricks the dev) --

    int reply(uint32_t request_id, uint32_t result, uint32_t vq_index, uint16_t avail_index) {
        vduse_dev_response resp;
        memset(&resp, 0, sizeof(resp));   // reserved must be zero
        resp.request_id = request_id;
        resp.result = result;
        resp.vq_state.index = vq_index;
        resp.vq_state.split.avail_index = avail_index;
        ssize_t w = ::write(dev_fd, &resp, sizeof(resp));
        if (w != (ssize_t)sizeof(resp))
            LOG_ERRNO_RETURN(0, -1, "vduse msg reply failed, rid `", request_id);
        return 0;
    }

    void handle_msg(const vduse_dev_request* req) {
        switch (req->type) {
        case VDUSE_SET_STATUS: {
            dev_status = req->s.status;
            LOG_INFO("vduse ` status -> 0x`", name, HEX(dev_status));
            if (dev_status == 0) {          // reset: stop serving, keep the session
                vqx.ready = false;
                vqx.reset_pending = true;    // the coming negotiation restarts
                                            // the ring counters at 0
            } else if (dev_status & VIRTIO_CONFIG_S_FEATURES_OK) {
                uint64_t f = 0;
                if (::ioctl(dev_fd, VDUSE_DEV_GET_FEATURES, &f) == 0) {
                    negotiated = f;
                    // DEV_GET_FEATURES returns the NEGOTIATED subset and is only
                    // valid once FEATURES_OK is set (<linux/vduse.h>:94-99), which
                    // is exactly where we are.
                    vq.event_idx = !!(negotiated & (1ULL << VIRTIO_RING_F_EVENT_IDX));
                }
            }
            if (dev_status & VIRTIO_CONFIG_S_DRIVER_OK)
                vq_needs_refresh = true;   // resolved by the vq loop: the msg
                                           // handler must stay clear of the
                                           // IOTLB_GET_FD ioctl, whose kernel
                                           // domain lock can be held by a path
                                           // that is itself waiting for a msg
                                           // reply (a probe run deadlocked so
                                           // for 200s)
            reply(req->request_id, VDUSE_REQ_RESULT_OK, 0, 0);
            break;
        }
        case VDUSE_UPDATE_IOTLB:
            LOG_DEBUG("vduse ` iotlb update [`, `]", name, req->iova.start, req->iova.last);
            iotlb.invalidate(req->iova.start, req->iova.last);
            if (req->iova.start == 0 && req->iova.last == 0)
                vqx.ready = false;   // unmap-all: the vring is gone
            reply(req->request_id, VDUSE_REQ_RESULT_OK, 0, 0);
            break;
        case VDUSE_GET_VQ_STATE:
            reply(req->request_id, VDUSE_REQ_RESULT_OK, req->vq_state.index,
                  req->vq_state.index == 0 ? vq.last_avail : 0);
            break;
        default:
            LOG_WARN("vduse ` unknown msg type `, answering OK", name, req->type);
            reply(req->request_id, VDUSE_REQ_RESULT_OK, 0, 0);
            break;
        }
    }

    void msg_loop() {
        vduse_dev_request req;
        while (!stopping) {
            if (photon::wait_for_fd_readable(dev_fd) < 0) {
                if (stopping) break;
                if (errno == EINTR) continue;
                LOG_ERRNO_RETURN(0, , "vduse msg loop wait failed, dev `", name);
            }
            ssize_t r = ::read(dev_fd, &req, sizeof(req));
            if (r == (ssize_t)sizeof(req)) {
                handle_msg(&req);
            } else if (r < 0 && errno == EAGAIN) {
                continue;
            } else if (r <= 0) {
                LOG_ERRNO_RETURN(0, , "vduse msg loop read failed, dev `", name);
            }
        }
    }

    // ----- the virtqueue -----

    // (re)resolve the vring after DRIVER_OK or an adoption resync
    int vq_refresh() {
        vduse_vq_info vi;
        memset(&vi, 0, sizeof(vi));
        vi.index = 0;
        if (::ioctl(dev_fd, VDUSE_VQ_GET_INFO, &vi) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse VQ_GET_INFO failed, dev `", name);
        // all five of these arrive cleared together after a device reset, and num
        // is not validated on the way in -- so num belongs in this guard as much
        // as the addresses do. It is a modulo divisor in dispatch_avail and in
        // vring_used_append; a 0 here with ready set divides by zero.
        if (!vi.ready || !vi.num || !vi.desc_addr || !vi.driver_addr || !vi.device_addr) {
            vqx.ready = false;
            return 0;
        }
        vq.num = vi.num;
        vq.desc = (vring_desc*)iotlb.resolve(vi.desc_addr, (size_t)vi.num * sizeof(vring_desc));
        vq.avail = (vring_avail*)iotlb.resolve(vi.driver_addr, sizeof(uint16_t) * (3 + vi.num));
        vq.used = (vring_used*)iotlb.resolve(vi.device_addr,
                        sizeof(uint16_t) * 3 + sizeof(vring_used_elem) * vi.num);
        if (!vq.desc || !vq.avail || !vq.used) {
            vqx.ready = false;
            LOG_ERROR_RETURN(EFAULT, -1, "vduse vring iova resolution failed, dev `", name);
        }
        if (!vqx.ready) {
            if (vqx.reset_pending) {
                // a fresh negotiation: the driver restarts its side of the
                // ring at 0, so we must too -- resuming from the stale
                // used->idx would make the driver consume old used entries
                // as completions and lose the new ones (observed: dd hangs)
                vqx.reset_pending = false;
                vq.used_idx = 0;
                vq.last_avail = 0;
                // same device object, driver renegotiated: notify_valid may
                // already be true from the previous negotiation, and leaving it
                // would cost the first completion after this reset its
                // unconditional notification
                vq.notify_valid = false;
            } else {
                // adoption of a live ring (no reset seen): resume where the
                // previous daemon left off; anything fetched-not-completed is
                // re-served (virtio-blk ops are idempotent)
                vq.used_idx = vring_used_idx(vq.used);
                vq.last_avail = vq.used_idx;
            }
        }
        // Establish avail_event == last_avail before the loop can sleep on the
        // kickfd. On adoption the two lines above set last_avail from the live
        // ring, which can be any value, while avail_event still holds what the
        // previous daemon published. See SPEC §3.2.
        vq.publish_avail_event();
        vqx.ready = true;
        LOG_INFO("vduse ` vq0 ready: num ` desc ` avail ` used ` resume at `",
                 name, vq.num, HEX(vi.desc_addr), HEX(vi.driver_addr),
                 HEX(vi.device_addr), vq.last_avail);
        return 0;
    }

    // ----- VirtQueueServer hooks: the vduse half of serving -----

    void* vq_translate(uint64_t addr, size_t len) { return iotlb.resolve(addr, len); }

    void vq_notify() {   // the used ring advanced and the driver asked for an IRQ
        uint32_t idx = 0;
        if (::ioctl(dev_fd, VDUSE_VQ_INJECT_IRQ, &idx) < 0 && errno != EBADF && errno != ENODEV)
            LOG_WARN("vduse INJECT_IRQ failed, dev `, ", name, ERRNO());
    }

    bool vq_may_dispatch() { return vqx.ready; }

    // top of every engine loop iteration: resolve a deferred ring refresh, and
    // release mappings invalidated by UPDATE_IOTLB once no request can still
    // hold their VAs (a reset-per-cycle device would otherwise accumulate them)
    void vq_tick() {
        if (vq_needs_refresh) {
            vq_needs_refresh = false;
            if (vq_refresh() < 0)
                LOG_ERROR("vduse vq refresh failed on `, ", name, ERRNO());
        }
        if (!vqx.ready && vq.in_flight.load() == 0)
            iotlb.flush_stale();
    }

    static void* vq_loop_thunk(void* d) {
        ((VduseDeviceImpl*)d)->vq.loop();
        return nullptr;
    }

    void vq_bind() {
        vq.backend = backend;
        vq.capacity = capacity_sectors << 9;   // the LBA bound serve_chain enforces
        vq.stack_size = resolve_stack_size(cfg.stack_size);
        vq.read_only = read_only;
        vq.serial = "photon-vduse";
        vq.tag = name;
        vq.hooks.translate.bind(this, &VduseDeviceImpl::vq_translate);
        vq.hooks.notify.bind(this, &VduseDeviceImpl::vq_notify);
        vq.hooks.ready.bind(this, &VduseDeviceImpl::vq_may_dispatch);
        vq.hooks.tick.bind(this, &VduseDeviceImpl::vq_tick);
    }

    // ----- lifecycle -----

    int ctrl_init() {
        if (ctrl_fd >= 0)
            return 0;
        ctrl_fd = ::open("/dev/vduse/control", O_RDWR | O_CLOEXEC);
        if (ctrl_fd < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to open /dev/vduse/control (vduse module loaded?)");
        uint64_t ver = 0;
        if (::ioctl(ctrl_fd, VDUSE_GET_API_VERSION, &ver) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse GET_API_VERSION failed");
        ver = VDUSE_API_VERSION;   // 0: the classic single-address-space ABI
        if (::ioctl(ctrl_fd, VDUSE_SET_API_VERSION, &ver) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse SET_API_VERSION(0) failed (kernel supports `)", ver);
        return 0;
    }

    // the config-space image for CREATE_DEV / SET_CONFIG
    void fill_config(uint8_t* buf, uint32_t size) {
        memset(buf, 0, size);
        auto* bc = (virtio_blk_config*)buf;
        bc->capacity = capacity_sectors;
        bc->blk_size = 1u << sector_shift;
        bc->num_queues = 1;
    }

    int create_dev() {
        // vduse_dev_config ends in a flexible array (config[]), which C++ will
        // not embed mid-struct: lay the ioctl payload out in a raw buffer --
        // offsetof(config) == sizeof(vduse_dev_config), and the kernel reads
        // sizeof + config_size bytes
        alignas(vduse_dev_config) uint8_t raw[sizeof(vduse_dev_config) + sizeof(virtio_blk_config)];
        memset(raw, 0, sizeof(raw));
        auto* cc = (vduse_dev_config*)raw;
        snprintf(cc->name, sizeof(cc->name), "%s", name);
        cc->vendor_id = 0x1af4;   // Red Hat -- the virtio-blk convention
        cc->device_id = VIRTIO_ID_BLOCK;
        cc->features = offer_features;
        cc->vq_num = 1;
        cc->vq_align = (uint32_t)sysconf(_SC_PAGESIZE);
        cc->config_size = sizeof(virtio_blk_config);
        fill_config(raw + sizeof(vduse_dev_config), sizeof(virtio_blk_config));
        if (::ioctl(ctrl_fd, VDUSE_CREATE_DEV, cc) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse CREATE_DEV failed, name `", name);
        created = true;
        return 0;
    }

    // VQ_SETUP + kickfd; safe on both fresh and adopted devices (the kernel
    // only records max_size; the driver's negotiated size comes via GET_INFO)
    int setup_vq() {
        vduse_vq_config vqc;
        memset(&vqc, 0, sizeof(vqc));
        vqc.index = 0;
        vqc.max_size = (uint16_t)std::min<uint32_t>(cfg.queue_depth ? cfg.queue_depth
                                                                    : DEFAULT_VQ_SIZE,
                                                    MAX_VQ_SIZE);
        if (::ioctl(dev_fd, VDUSE_VQ_SETUP, &vqc) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse VQ_SETUP failed, dev `", name);
        vq.kickfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (vq.kickfd < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse kickfd creation failed, dev `", name);
        vduse_vq_eventfd ev;
        memset(&ev, 0, sizeof(ev));
        ev.index = 0;
        ev.fd = vq.kickfd;
        if (::ioctl(dev_fd, VDUSE_VQ_SETUP_KICKFD, &ev) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse KICKFD failed, dev `", name);
        return 0;
    }

    // BlkConfig::timeout -> the per-device msg_timeout sysfs (best effort:
    // an old kernel without the attribute keeps its default)
    void apply_msg_timeout() {
        if (!cfg.timeout)
            return;
        char path[VDUSE_NAME_MAX + 48];
        snprintf(path, sizeof(path), "/sys/class/vduse/%s/msg_timeout", name);
        int fd = ::open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            LOG_DEBUG("vduse msg_timeout not settable (`), keeping the kernel default", path);
            return;
        }
        DEFER(::close(fd));
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%u", cfg.timeout);
        if (::write(fd, buf, n) != n)
            LOG_WARN("vduse failed to set msg_timeout on `, ", name, ERRNO());
    }

    void stop_serving(bool drain_backlog) {
        if (!drain_backlog)
            vqx.ready = false;   // stop dispatching NOW; the un-dispatched avail
                                // backlog stays in the ring for the next daemon
        if (vq_th) {
            // drain the dispatched requests (they complete into the used ring
            // while dev_fd is still open); with drain_backlog the vq loop also
            // keeps fetching until the avail ring is empty (orderly handover)
            while (vq.in_flight.load() ||
                   (drain_backlog && vqx.ready &&
                    vq.last_avail != vring_avail_idx(vq.avail)))
                photon::thread_usleep(1000);
            stopping = true;
            vq.stopping = true;   // from here the engine leaves in-flight
                                  // requests uncompleted (handover contract)
            vq.run = false;
            vq.wake();            // wake the loop out of its kickfd wait
            photon::thread_interrupt(vq_th);
            photon::thread_join((photon::join_handle*)vq_th);
            vq_th = nullptr;
        }
        if (msg_th) {
            photon::thread_interrupt(msg_th);
            photon::thread_join((photon::join_handle*)msg_th);
            msg_th = nullptr;
        }
        // the pre-join drain cannot see a batch the vq loop dispatched in the
        // window before `stopping` took effect: those request coroutines hold
        // VAs into the mappings, so wait for them here -- unmapping under a
        // live request would be a use-after-free
        vq.drain();
        vqx.ready = false;
        if (vq.kickfd >= 0) {
            vduse_vq_eventfd ev;
            memset(&ev, 0, sizeof(ev));
            ev.index = 0;
            ev.fd = VDUSE_EVENTFD_DEASSIGN;
            if (dev_fd >= 0)
                ::ioctl(dev_fd, VDUSE_VQ_SETUP_KICKFD, &ev);
            ::close(vq.kickfd);
            vq.kickfd = -1;
        }
        iotlb.clear();
        vq.desc = nullptr;
        vq.avail = nullptr;
        vq.used = nullptr;
        if (dev_fd >= 0) {
            ::close(dev_fd);   // connected = false: the registration survives
            dev_fd = -1;
            iotlb.dev_fd = -1;
        }
    }

    // ----- flock: cross-process ownership + the orphan scan's scope -----
    //
    // The single-opener char device answers "is a daemon connected RIGHT NOW".
    // It cannot answer "is this device OURS" once nobody is connected -- which is
    // precisely the orphan case -- so without a tombstone the controller's
    // list_orphans() would report every unheld vduse device on the host, including
    // another tenant's, and a caller could adopt one it does not own. The
    // tombstone is the ownership record, and it scopes the scan to the lock_dir
    // the way the HBA directory scopes tcmu's -- one directory, held by the
    // VduseController that both lists and builds, so the scan and the claim cannot
    // disagree about it.

    int acquire_lock() {
        char ln[VDUSE_LOCK_BUF];
        vduse_lock_name(name, ln, sizeof(ln));
        if (devlock_acquire(lock_dir, ln, &lock_fd) < 0) {
            if (errno == EBUSY)
                LOG_ERROR_RETURN(EBUSY, -1, "vduse device ` is held by another live server", name);
            return -1;   // devlock_acquire logged it
        }
        return 0;
    }
    void release_lock() {
        devlock_release(lock_fd);
        lock_fd = -1;
    }

    int start(fs::IFile* bk, bool ownership) override {
        if (started)
            LOG_ERROR_RETURN(EALREADY, -1, "vduse device already started");
        if (!bk)
            LOG_ERROR_RETURN(EINVAL, -1, "backend IFile is null");

        backend = bk;
        own_backend = ownership;

        bool ok = false;
        DEFER(if (!ok) { int e = errno; rollback(); errno = e; });

        if (ctrl_init() < 0)
            return -1;
        // claim the identity before touching the kernel: a name another photon
        // server already holds fails here, without creating or adopting anything
        if (acquire_lock() < 0)
            return -1;

        // create-or-attach: open first. ENOENT -> create; EBUSY -> a live
        // daemon holds the single-opener char dev; success -> an orphan whose
        // backlog we resume (used->idx restart, validated by the rescue run)
        char path[VDUSE_NAME_MAX + 16];
        snprintf(path, sizeof(path), "/dev/vduse/%s", name);
        dev_fd = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (dev_fd < 0 && errno == EBUSY)
            LOG_ERROR_RETURN(EBUSY, -1, "vduse device ` is held by another live daemon", name);
        if (dev_fd < 0 && errno != ENOENT)
            LOG_ERRNO_RETURN(0, -1, "failed to open ", path);
        if (dev_fd < 0) {
            if (create_dev() < 0)
                return -1;
            dev_fd = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (dev_fd < 0) {
                if (errno != EBUSY)
                    LOG_ERRNO_RETURN(0, -1, "failed to open the created ", path);
                // raced with another daemon that adopted our fresh device
                LOG_ERROR_RETURN(EBUSY, -1, "vduse device ` was taken during creation", name);
            }
        } else {
            LOG_INFO("vduse adopting the existing registration `", name);
        }
        iotlb.dev_fd = dev_fd;
        registered = true;   // created or adopted: shutdown() may destroy it

        if (setup_vq() < 0)
            return -1;
        apply_msg_timeout();

        stopping = false;
        dev_status = 0;
        negotiated = 0;
        vq_bind();
        vq.stopping = false;
        vq.run = true;
        msg_th = photon::thread_create11(&VduseDeviceImpl::msg_loop, this);
        photon::thread_enable_join(msg_th);
        vq_th = photon::thread_create(&VduseDeviceImpl::vq_loop_thunk, this);
        photon::thread_enable_join(vq_th);

        // adoption resync: a consumer may already be attached (DRIVER_OK from
        // the previous daemon's life); the kernel replays nothing, so pull the
        // state ourselves. A fresh device simply reports ready=0 and waits
        // for the SET_STATUS handshake.
        uint64_t f = 0;
        if (::ioctl(dev_fd, VDUSE_DEV_GET_FEATURES, &f) == 0) {
            negotiated = f;
            // An adopted device is already FEATURES_OK from the daemon we took
            // over from, so this is the negotiated subset. A fresh device reaches
            // here too and gets a word that is not yet negotiated; that is
            // harmless because no ring can go live before the FEATURES_OK handler
            // re-derives event_idx, and every consumer of event_idx needs a live
            // ring.
            vq.event_idx = !!(negotiated & (1ULL << VIRTIO_RING_F_EVENT_IDX));
        }
        if (vq_refresh() < 0)
            return -1;

        started = true;
        ok = true;
        LOG_INFO("vduse device started, ", make_named_value("name", (const char*)name), VALUE(cfg.info.size),
                 "created=", (int)created, "features=", HEX(offer_features));
        return 0;
    }

    int detach(bool wait_pending) override {
        if (!started)
            return 0;
        stop_serving(wait_pending);
        release_lock();
        started = false;
        created = false;   // the registration is up for adoption; a failed
                           // re-start must not destroy it. Releasing the
                           // tombstone IS releasing ownership (the ublk
                           // invariant), so shutdown() re-claims it first.
        LOG_INFO("vduse device detached, ", make_named_value("name", (const char*)name), "flush=", (int)wait_pending);
        return 0;
    }

    int shutdown() override {
        if (!started && dev_fd < 0 && !registered)
            return 0;
        // keep_lock carries the ublk invariant: a device we are STILL serving
        // keeps its claim, so an EBUSY teardown does not hand it over to orphan
        // recovery; a claim borrowed only for this teardown is given back on
        // every path, success included.
        bool keep_lock = false;
        DEFER(if (!keep_lock) release_lock());
        if (!started && registered) {
            // detached: detach() released the tombstone, so another server may
            // have adopted the registration since. Re-claim before destroying --
            // DESTROY_DEV would otherwise tear down a device that is not ours
            // anymore. EBUSY = it belongs to that server now.
            if (acquire_lock() < 0) {
                if (errno != EBUSY)
                    return -1;   // already logged; leave the device alone
                LOG_INFO("vduse device ` is owned by another server now; leaving it alone", name);
                registered = false;
                created = false;
                return 0;
            }
            // The char device is the kernel-side half of the same claim, and the
            // only half that catches a foreign daemon which takes no tombstone.
            char path[VDUSE_NAME_MAX + 16];
            snprintf(path, sizeof(path), "/dev/vduse/%s", name);
            int fd = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                if (errno == ENOENT) {   // already destroyed
                    registered = false;
                    created = false;
                    return 0;
                }
                if (errno != EBUSY)
                    LOG_ERRNO_RETURN(0, -1, "failed to re-open ", path);
                LOG_INFO("vduse device ` is served by another daemon now; leaving it alone", name);
                registered = false;
                created = false;
                return 0;
            }
            ::close(fd);   // claim probed; DESTROY below only needs no daemon
                           // connected, which is true the moment we close
        }
        // the contractual EBUSY: a vdpa consumer holds the registration. Check
        // BEFORE stopping serving -- tearing down first would leave the
        // consumer's device unserved (its users wedge in D state) while still
        // failing the destroy. /sys/bus/vdpa/devices/<name> exists exactly
        // while the kernel-side vdev does (the DESTROY_DEV EBUSY condition).
        char sp[VDUSE_NAME_MAX + 40];
        snprintf(sp, sizeof(sp), "/sys/bus/vdpa/devices/%s", name);
        if (registered && ::access(sp, F_OK) == 0) {
            // nothing torn down yet: a started device is still serving and keeps
            // its claim, a borrowed one goes back
            keep_lock = started;
            LOG_ERROR_RETURN(EBUSY, -1,
                "vduse device ` is still attached to a vdpa consumer (vdpa dev del it first)",
                name);
        }
        if (started)
            stop_serving(true);
        if (registered && ctrl_fd >= 0) {
            char nm[VDUSE_NAME_MAX];
            snprintf(nm, sizeof(nm), "%s", name);
            if (::ioctl(ctrl_fd, VDUSE_DESTROY_DEV, nm) < 0) {
                if (errno == EBUSY)
                    LOG_ERROR_RETURN(EBUSY, -1,
                        "vduse device ` is still attached to a vdpa consumer (vdpa dev del it first)",
                        name);
                if (errno != EINVAL)   // EINVAL: the registration is already gone
                    LOG_ERRNO_RETURN(0, -1, "vduse DESTROY_DEV failed, name `", name);
            }
        }
        started = false;
        created = false;
        registered = false;
        return 0;
    }

    int resize(uint64_t new_size) override {
        if (dev_fd < 0)
            LOG_ERROR_RETURN(ENODEV, -1, "vduse resize: not started");
        if (new_size % 512)
            LOG_ERROR_RETURN(EINVAL, -1, "resize size ` is not a multiple of 512", new_size);
        uint64_t cur = capacity_sectors << 9;
        if (new_size == cur)
            return 0;
        if (new_size < cur)
            LOG_ERROR_RETURN(EINVAL, -1, "vduse resize: shrink (` -> `) is rejected", cur, new_size);
        struct alignas(vduse_config_data) { uint8_t raw[sizeof(vduse_config_data) + 8]; } scbuf;
        memset(&scbuf, 0, sizeof(scbuf));
        auto* sc = (vduse_config_data*)scbuf.raw;
        sc->offset = 0;   // capacity is the first config field
        sc->length = 8;
        uint64_t cap = new_size >> 9;
        memcpy(scbuf.raw + sizeof(vduse_config_data), &cap, 8);
        if (::ioctl(dev_fd, VDUSE_DEV_SET_CONFIG, sc) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse SET_CONFIG failed, dev `", name);
        if (::ioctl(dev_fd, VDUSE_DEV_INJECT_CONFIG_IRQ) < 0)
            LOG_WARN("vduse INJECT_CONFIG_IRQ failed on `, ", name, ERRNO());
        capacity_sectors = cap;
        vq.capacity = new_size;   // serve_chain's LBA bound must grow with us
        cfg.info.size = new_size;
        LOG_INFO("vduse device resized, ", make_named_value("name", (const char*)name), VALUE(cur), VALUE(new_size));
        return 0;
    }

    void rollback() {
        // a failed start: stop anything spawned, then remove the registration
        // only if WE created it (an adopted orphan stays recoverable)
        if (vq_th || msg_th) {
            stopping = true;
            vq.stopping = true;
            vq.run = false;
            if (vq_th) {
                vq.wake();
                photon::thread_interrupt(vq_th);
                photon::thread_join((photon::join_handle*)vq_th);
                vq_th = nullptr;
            }
            if (msg_th) {
                photon::thread_interrupt(msg_th);
                photon::thread_join((photon::join_handle*)msg_th);
                msg_th = nullptr;
            }
        }
        vq.drain();   // request coroutines may still hold VAs into the mappings
        iotlb.clear();
        if (vq.kickfd >= 0) { ::close(vq.kickfd); vq.kickfd = -1; }
        if (dev_fd >= 0) { ::close(dev_fd); dev_fd = -1; iotlb.dev_fd = -1; }
        if (created && registered && ctrl_fd >= 0) {
            char nm[VDUSE_NAME_MAX];
            snprintf(nm, sizeof(nm), "%s", name);
            if (::ioctl(ctrl_fd, VDUSE_DESTROY_DEV, nm) < 0 && errno != EINVAL)
                LOG_WARN("vduse rollback DESTROY_DEV failed, name `, ", name, ERRNO());
        }
        created = false;
        registered = false;
        release_lock();
        started = false;
        // a failed start returns the object to its virgin state -- INCLUDING
        // backend ownership: the caller keeps the backend (else the destructor
        // and the caller would both delete it)
        backend = nullptr;
        own_backend = false;
    }
};

struct VduseControllerImpl : VduseController {
    char lock_dir[SCOPE_DIR_BUF] = {};   // "" = /run/photon-blk, normalized by devlock_*

    explicit VduseControllerImpl(const char* ld) {
        if (ld)
            snprintf(lock_dir, sizeof(lock_dir), "%s", ld);   // bounded: the factory checked
    }

    IBlkDevice* new_device(const BlkConfig& cfg) override {
        if (VduseDeviceImpl::validate(cfg) < 0)
            return nullptr;
        return new VduseDeviceImpl(cfg, lock_dir);
    }

    // Orphan = OURS and unheld, which takes two tests. Ownership: a tombstone in
    // our lock_dir whose flock is free -- without it this would list every unheld
    // vduse device on the host, another tenant's included. Liveness: the
    // single-opener char device admits a probing open, which still catches a
    // foreign daemon that takes no tombstone. The vduse uapi has NO config
    // readback, so size is unspecified (0) and features are best-effort
    // (DEV_GET_FEATURES only reflects a still-attached consumer's negotiation):
    // the record is a tombstone like vhost-user's -- recovery re-opens by name and
    // the caller supplies the geometry (start() rewrites the config space via
    // SET_CONFIG only through resize(); a size mismatch against the registered
    // capacity is NOT detectable here).
    std::vector<BlkDevInfo> list_orphans() override {
        std::vector<BlkDevInfo> ret;
        DIR* dd = ::opendir("/dev/vduse");
        if (!dd)
            return ret;
        DEFER(::closedir(dd));
        struct dirent* e;
        while ((e = readdir(dd))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "control"))
                continue;
            char ln[VDUSE_LOCK_BUF];
            vduse_lock_name(e->d_name, ln, sizeof(ln));
            if (devlock_free(lock_dir, ln) != 1)
                continue;   // held, or no tombstone here: not ours to list
            char path[VDUSE_NAME_MAX + 16];
            snprintf(path, sizeof(path), "/dev/vduse/%s", e->d_name);
            int fd = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;   // EBUSY: a live daemon; ENOENT/EPERM: raced away
            BlkDevInfo bi;
            bi.identity = e->d_name;
            bi.size = 0;
            bi.sector_size_shift = 9;
            uint64_t f = 0;
            if (::ioctl(fd, VDUSE_DEV_GET_FEATURES, &f) == 0) {
                if (f & (1ULL << VIRTIO_BLK_F_FLUSH)) bi.features |= FEATURE_FLUSH;
                // RO/DISCARD/WZ are not expressible in BlkDevInfo.features
            }
            ::close(fd);
            ret.push_back(bi);
        }
        return ret;
    }
};

VduseController* new_vduse_controller(const char* lock_dir) {
    if (validate_scope_dir(lock_dir) < 0)
        return nullptr;   // already logged
    return new VduseControllerImpl(lock_dir);
}

}  // namespace blk
}  // namespace photon
