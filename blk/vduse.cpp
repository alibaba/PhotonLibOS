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
// P1 scope: BlkConfig::queues virtqueues -- 0 means one, over MAX_QUEUES means
// clamped to it, and an ADOPTED registration keeps the count it was created with
// instead -- and VIRTIO_BLK_F_MQ offered exactly when that count is more
// than one, split ring only (no RING_PACKED / INDIRECT -- not offered, so the
// driver must not use them), IN/OUT/FLUSH/GET_ID requests, FEATURE_FLUSH +
// read_only + logical block size; FEATURE_DISCARD/WRITE_ZEROES are accepted in
// cfg.info.features but not offered yet. Serving runs on the caller's vcpu
// unless BlkConfig::pool names one, in which case each queue's loop coroutine
// is migrated into it. All virtio fields are little-endian (VERSION_1) and
// this file, like nbd/tcmu/ublk, assumes an LE host (x86_64/aarch64).

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

// The five structs below are the ones the multiqueue path reads an `index` out of.
// They are hand-copied from <linux/vduse.h> because that header is too new for
// many build hosts (the same reason ublk.cpp copies <linux/ublk_cmd.h>), and a
// copied struct proves nothing on its own: a transposed field or a wrong width
// compiles clean and only shows up as a mis-directed ioctl once a queue index
// other than 0 is in play. These pin what we actually read. They check that WE
// copied correctly -- deliberately NOT a cross-check against the kernel header,
// which would need a C translation unit (vduse.h is C++-clean, unlike tcmu's).
static_assert(sizeof(vduse_dev_config) == 336, "vduse_dev_config size");
static_assert(offsetof(vduse_dev_config, features) == 264, "vduse_dev_config features offset");
static_assert(offsetof(vduse_dev_config, vq_num) == 272, "vduse_dev_config vq_num offset");
static_assert(offsetof(vduse_dev_config, config_size) == 332, "vduse_dev_config config_size offset");

static_assert(sizeof(vduse_vq_config) == 32, "vduse_vq_config size");
static_assert(offsetof(vduse_vq_config, index) == 0, "vduse_vq_config index offset");
static_assert(offsetof(vduse_vq_config, max_size) == 4, "vduse_vq_config max_size offset");

static_assert(sizeof(vduse_vq_info) == 48, "vduse_vq_info size");
static_assert(offsetof(vduse_vq_info, index) == 0, "vduse_vq_info index offset");
static_assert(offsetof(vduse_vq_info, num) == 4, "vduse_vq_info num offset");
static_assert(offsetof(vduse_vq_info, desc_addr) == 8, "vduse_vq_info desc_addr offset");
static_assert(offsetof(vduse_vq_info, driver_addr) == 16, "vduse_vq_info driver_addr offset");
static_assert(offsetof(vduse_vq_info, device_addr) == 24, "vduse_vq_info device_addr offset");
static_assert(offsetof(vduse_vq_info, ready) == 40, "vduse_vq_info ready offset");

static_assert(sizeof(vduse_vq_eventfd) == 8, "vduse_vq_eventfd size");
static_assert(offsetof(vduse_vq_eventfd, index) == 0, "vduse_vq_eventfd index offset");
static_assert(offsetof(vduse_vq_eventfd, fd) == 4, "vduse_vq_eventfd fd offset");

static_assert(sizeof(vduse_vq_state) == 12, "vduse_vq_state size");
static_assert(offsetof(vduse_vq_state, index) == 0, "vduse_vq_state index offset");

// read today, asserted here so the multiqueue change is not the first to depend
// on an unpinned layout
static_assert(sizeof(vduse_iotlb_entry) == 32, "vduse_iotlb_entry size");
static_assert(offsetof(vduse_iotlb_entry, offset) == 0, "vduse_iotlb_entry offset field");
static_assert(offsetof(vduse_iotlb_entry, start) == 8, "vduse_iotlb_entry start offset");
static_assert(offsetof(vduse_iotlb_entry, last) == 16, "vduse_iotlb_entry last offset");
static_assert(offsetof(vduse_iotlb_entry, perm) == 24, "vduse_iotlb_entry perm offset");
static_assert(sizeof(vduse_iova_range) == 16, "vduse_iova_range size");
static_assert(sizeof(vduse_dev_status) == 1, "vduse_dev_status size");
static_assert(sizeof(vduse_config_data) == 8, "vduse_config_data size");

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
    // Bumped by every invalidate. resolve()'s slow path runs OUTSIDE the lock, so
    // a range it is fetching can be invalidated in between, and the re-scan
    // before the insert cannot see that: an invalidated range is no longer in
    // `maps`, so the re-scan reports "absent" both for "nobody fetched it yet"
    // and for "the driver has unmapped it". Without this counter the second case
    // would publish a mapping of pages the driver no longer owns, and every later
    // request for that IOVA would be a cache hit on them. A plain integer, not an
    // atomic: every access is inside `lock`.
    uint64_t gen = 0;
    // A photon::mutex, not a spinlock: resolve()'s slow path blocks in
    // VDUSE_IOTLB_GET_FD and then in mmap, and a spinlock held across a blocking
    // syscall leaves another OS thread's vcpu spinning for the whole kernel round
    // trip. It guards the two vectors and nothing else -- in particular it does
    // NOT keep a mapping alive, which is what `stale` plus the caller's in-flight
    // drain are for (see invalidate).
    photon::mutex lock;

    void* resolve(uint64_t iova, size_t len) {
        uint64_t gen0 = 0;
        {
            SCOPED_LOCK(lock);
            gen0 = gen;
            // Wrap-free, and the cache is the only place that needs it: a wrapped
            // `iova + len - 1` in the ioctl below produces an end below its start,
            // which the ioctl rejects -- but a cache HIT never reaches the ioctl, so
            // the old endpoint form handed out base + (iova - m.start) for a
            // guest-written desc.addr near UINT64_MAX: an arbitrarily negative offset
            // into our own mappings. iova <= m.last makes the subtraction below safe.
            for (auto& m : maps)
                if (iova >= m.start && iova <= m.last && len <= m.last - iova + 1)
                    return m.base + (iova - m.start);
        }
        // The slow path runs OUTSIDE the lock: the ioctl and the mmap both block,
        // and holding the lock across them would queue every other queue's request
        // coroutines behind one kernel round trip.
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
        char* hit = nullptr;
        bool invalidated = false;
        {
            SCOPED_LOCK(lock);
            // Re-scan before inserting: another coroutine can have fetched and
            // published this same range while the ioctl and mmap above ran. Its
            // mapping answers the request just as well, so keep that one and drop
            // ours rather than cache two copies of the same IOVA range.
            for (auto& m : maps)
                if (iova >= m.start && iova <= m.last && len <= m.last - iova + 1) {
                    hit = m.base + (iova - m.start);
                    break;
                }
            // The re-scan above cannot tell "still absent" from "invalidated while
            // we were fetching": an invalidated range leaves `maps`, and ours was
            // never in it. So the generation says it -- coarsely, because it cannot
            // tell our range from some other one that moved, and a false positive
            // costs one request a resolution the driver would have honoured. That
            // is the cheap side of the trade: re-fetching in a loop instead is
            // precise but has no bound, since every round needs a fresh
            // invalidation to be wasted, and a driver that churns the iotlb per
            // request can churn faster than an ioctl and an mmap take. Dropping our
            // mapping and answering from the cache as it now stands gives a hit if
            // somebody re-fetched the range after the invalidation, and a failure
            // otherwise -- which is what an IOVA the driver has unmapped resolves
            // to anyway.
            //
            // "Cheap" is a property of the caller, though, and resolve() has two
            // of them. For a request the trade holds as stated -- and "cheap" still
            // does not mean free: virtio-blk has no retryable status, so the
            // request completes with VIRTIO_BLK_S_IOERR and the guest reports an IO
            // error. What makes this side survivable is that one request fails, not
            // that it recovers by itself.
            // The other caller resolves a whole vring through these, and a nullptr
            // there is not one request lost but a ring that cannot be published at
            // all -- so vq_refresh snapshots this same generation before it
            // resolves and, when it finds the snapshot stale at its EFAULT branch,
            // re-arms the refresh instead of failing it. The cost analysis above
            // only covers the request side; the ring side is covered there.
            invalidated = (gen != gen0);
            if (!hit && !invalidated)
                maps.push_back(Map{e.start, e.last, (char*)base});
        }
        if (hit || invalidated) {
            ::munmap(base, sz);
            return hit;   // nullptr when the range was invalidated under us
        }
        return (char*)base + (iova - e.start);
    }

    // The invalidation counter, for a caller that has to tell "this IOVA is not
    // mapped" from "the address space moved while I was resolving it". Takes the
    // lock because `gen` is a plain integer whose every access is inside it.
    uint64_t generation() {
        SCOPED_LOCK(lock);
        return gen;
    }

    // A range's mapping changed (UPDATE_IOTLB): drop it from the lookup so the
    // next resolve re-fetches, but do NOT munmap yet -- request coroutines may
    // still hold VAs into it. The lock does not change that argument, it only
    // protects the vectors: it cannot tell whether some coroutine is mid-IO
    // through `base`, so the unmap still waits for the in-flight drain that
    // flush_stale's caller performs.
    // start==last==0 is the driver's unmap-all (vq teardown).
    void invalidate(uint64_t start, uint64_t last) {
        SCOPED_LOCK(lock);
        invalidate_locked(start, last);
    }
    void flush_stale() {
        SCOPED_LOCK(lock);
        flush_locked();
    }
    void clear() {
        SCOPED_LOCK(lock);
        invalidate_locked(0, UINT64_MAX);
        flush_locked();
    }
    // Unlocked halves for clear(): photon::mutex is not recursive, so clear()
    // cannot call the lock-taking members above it. `lock` is already held.
    void invalidate_locked(uint64_t start, uint64_t last) {
        gen++;   // resolve()'s in-flight slow path must not publish what this drops
        for (size_t i = maps.size(); i-- > 0; ) {
            auto& m = maps[i];
            if (m.start > last || m.last < start)
                continue;
            stale.push_back(m);
            maps.erase(maps.begin() + i);
        }
    }
    void flush_locked() {
        // The munmap stays inside the lock so that, for every other taker, the
        // cache contents and the set of live mappings change as one step: there is
        // no window where an entry has left `maps` but its pages are still there,
        // or has been munmapped while `stale` still lists it.
        for (auto& m : stale)
            ::munmap(m.base, (size_t)(m.last - m.start + 1));
        stale.clear();
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
    // Field order is padding-driven, do not tidy it: all 8-byte members first,
    // then the 4-byte ones, then the 1-byte flags and the two char[256] buffers.
    // Previously `name` stranded 1 byte before `ctrl_fd`, `sector_shift` +
    // `read_only` 6 before `capacity_sectors`, `dev_status` 7 before `iotlb`, and
    // the odd end of `lock_dir` left 7 of tail.
    BlkConfig cfg;
    fs::IFile* backend = nullptr;

    // The word create_dev advertises -- so it is the kernel's only on the create
    // branch. An adopt never runs create_dev: there this says what a create WOULD
    // have advertised, and `negotiated` below is what the registration holds. Kept
    // in step with nqueues all the same, so that a shutdown followed by another
    // start -- which does create -- cannot publish a count and a feature word that
    // disagree about it.
    uint64_t offer_features = 0;
    uint64_t negotiated = 0;           // DEV_GET_FEATURES after FEATURES_OK
    uint64_t capacity_sectors = 0;     // 512-byte units, the virtio constant

    Iotlb iotlb;
    photon::thread* msg_th = nullptr;

    // Per-queue state: the shared serving engine (ring state, dispatch,
    // completion, drain) plus what only this transport knows about that queue.
    // ONE heap allocation per queue, and the pointer vector is deliberate:
    // VirtQueueServer holds a std::atomic, so it is neither copyable nor movable
    // and cannot live in a vector by value -- resize() needs MoveInsertable.
    // Pointers also make every address here stable for the device's lifetime,
    // which the hooks below depend on: they are bound with `this` == the Vq*, and
    // a reallocation would leave them pointing at freed memory.
    struct VqVduse {
        // Cross-vcpu state: the control plane (the msg loop and teardown, on the
        // caller's vcpu) writes these and this queue's loop reads them on `home`,
        // which BlkConfig::pool can make another OS thread. vq_refresh is the
        // exception -- it runs on the loop's side, where it reads `gen`, consumes
        // reset_pending and re-arms needs_refresh. Relaxed only: nothing else is
        // published through them. `ready` is the one field with two writers of
        // opposite intent -- the control plane clears it, vq_refresh sets it --
        // which is an ordering problem, not a data race, and `gen` is what settles
        // the order: an invalidation bumps the generation before it clears, and a
        // refresh that finds the generation moved while it was resolving withholds
        // its own publish. Stopping the queue around the clear instead is not
        // available here: the kernel blocks the sender of a message until that
        // message is answered, for msg_timeout seconds, so nothing the message
        // loop waits for may be as unbounded as the in-flight requests are.
        std::atomic<bool> ready{false};          // the ring is resolved and
                                                 // dispatch may run
        std::atomic<bool> reset_pending{false};  // a status-0 reset: zero the ring
                                                 // counters at the next refresh
                                                 // (vs adoption resume)
        std::atomic<bool> needs_refresh{false};  // DRIVER_OK seen; the loop resolves
        // The generation of this queue's readiness. A refresh snapshots it
        // before it resolves anything and publishes only if it has not moved.
        std::atomic<uint32_t> gen = 0;
        // The vcpu this queue's loop coroutine runs on, recorded by vq_start
        // immediately after the migration: photon::get_vcpu(thread*) reads the field
        // do_thread_migrate stores under the thread's own lock before it returns, so
        // the migrator already holds the answer. That is why no completion handshake
        // belongs here -- a semaphore exists to tell a waiter that some work
        // finished, and no work has to finish for this value to be known. An
        // accepted migration leaves nothing between the create and this read that
        // yields (thread_create only queues the new coroutine, thread_migrate does
        // not switch for a thread that is not the caller, get_vcpu is an inline
        // field read), so no other coroutine on this vcpu can catch the loop
        // created while `home` is still null. That a null means "no loop to race"
        // is the direction run_on_home's !home branch rests on, and the only one
        // that holds: the converse has a window, because vq_stop_here nulls `th` on
        // the serving vcpu inside the hop while vq_stop nulls `home` here only after
        // that hop returns, and this vcpu yields while it waits for it. A refused
        // migration needs no handling either, though it is not yield-free --
        // thread_migrate logs every refusal and migrate_to_pool warns behind it, so
        // a caller-installed sink writing through a photon IFile does yield in here.
        // The value is still right: do_thread_migrate leaves the field untouched
        // when it says no, and what it left is this vcpu, which is where the loop
        // then really runs -- so a reader that sees the null sits on the loop's own
        // vcpu instead of racing it. With cfg.pool null it is simply the caller's
        // own vcpu.
        //
        // Teardown moves its work there, because the loop and the request coroutines
        // it spawned share it and are the only writers of last_avail, used_idx and
        // the used ring WHILE THE LOOP IS LIVE -- and because the backlog wait
        // dereferences `avail`, which flush_stale on this same vcpu is what munmaps.
        //
        // Control-plane field: written by vq_start, read by the three teardown
        // callers and by msg_loop's answer to the kernel's vq state read. That all
        // of those readers run on the vcpu that called start() is a contract with
        // the caller, not a property of this code: `home` is a plain pointer, and
        // nothing here would notice a detach() or a shutdown() issued from another
        // vcpu. `home` is then what two OS threads would share: vq_start writes it on
        // the vcpu that called start(), while a caller on the other one both reads it
        // and nulls it. msg_loop honours it because start() creates it there and
        // deliberately never migrates it. The loop's own vcpu never touches it.
        // vq_stop clears it once the loop is joined, so the drain that follows
        // runs in place -- which it may, because drain() polls nothing but the
        // atomic in_flight and therefore has no vcpu it must be on.
        //
        // Work stealing is what would break it: photon writes the field again only in
        // its two stealing scans, and those need a per-thread create flag and a
        // per-vcpu init flag that nothing in blk passes. Turned on, `home` would go
        // stale within the coroutine's life -- but so would the design that gives
        // each queue one serving vcpu, since a stolen loop moves away from the
        // request coroutines that inherit its vcpu.
        photon::vcpu_base* home = nullptr;
    };
    struct Vq {
        VduseDeviceImpl* impl = nullptr;
        uint32_t qid = 0;
        VirtQueueServer srv;
        VqVduse x;
        photon::thread* th = nullptr;
    };
    std::vector<Vq*> vqs;
    // How many virtqueues this device serves: BlkConfig::queues as the
    // constructor reads it (0 = one, over MAX_QUEUES = clamped to it), except on
    // an adopt, where it becomes the count the registration already has.
    // `nqueues` sits with the fds rather than down with the bools: here it fills
    // the 4-byte slot they leave.
    uint32_t nqueues = 1;

    int ctrl_fd = -1;
    int dev_fd = -1;
    int lock_fd = -1;           // the tombstone claim inside the controller's lock_dir

    bool own_backend = false;
    bool started = false;
    bool created = false;       // we CREATE_DEV'd it (vs adopted an orphan)
    // A kernel registration exists under `name` and shutdown() may destroy it.
    // `name` itself is permanent identity now (fixed at construction), so it can
    // no longer double as this marker the way it used to.
    bool registered = false;
    // Written and read only by the control plane (msg_loop, start, stop_serving,
    // rollback), all of which stay on the caller's vcpu -- the serving side is
    // told to stop through the atomic VirtQueueServer::stopping plus
    // wake/interrupt/join instead. So this needs no atomic.
    bool stopping = false;
    uint8_t  sector_shift = 9;
    bool     read_only = false;
    uint8_t  dev_status = 0;           // last SET_STATUS

    // log as (const char*), never VALUE(): alog would emit all VDUSE_NAME_MAX bytes
    char name[VDUSE_NAME_MAX] = {};
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

        // Clamped, not rejected, and 0 means "you choose" -- the same reading the
        // ublk and vhost-user transports give this field, so one BlkConfig means
        // the same thing to every transport.
        //
        // Derived HERE, with exactly one exception: start()'s adopt branch raises
        // it to the count the registration already has. Four things have to agree
        // with it -- the vqs slots sized just below, the vq_num create_dev
        // declares, the num_queues fill_config publishes, and the F_MQ bit
        // offer_features takes from it further down -- and the raise cannot
        // desynchronize them: the middle two never run on an adopt, an adopt is
        // the only branch that raises, and the function that raises grows the
        // slots and sets the bit itself.
        nqueues = cfg.queues ? std::min<uint32_t>(cfg.queues, MAX_QUEUES) : 1;

        offer_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_F_ACCESS_PLATFORM) |
                         (1ULL << VIRTIO_RING_F_EVENT_IDX) |
                         (1ULL << VIRTIO_BLK_F_BLK_SIZE);
        if (cfg.info.features & FEATURE_FLUSH)
            offer_features |= (1ULL << VIRTIO_BLK_F_FLUSH);
        if (read_only)
            offer_features |= (1ULL << VIRTIO_BLK_F_RO);
        // virtio 1.2 §5.2.3: offering F_MQ commits us to a truthful
        // virtio_blk_config::num_queues (§5.2.4: that field is only valid when
        // this bit is set) and to honoring the queue index in every vring
        // message. Only offered when there is more than one queue -- an n==1
        // device that offers it makes the frontend build one vq while believing
        // the device is multiqueue.
        if (nqueues >= 2)
            offer_features |= (1ULL << VIRTIO_BLK_F_MQ);
        // FEATURE_DISCARD / FEATURE_WRITE_ZEROES: accepted but not offered yet
        vqs.reserve(nqueues);
        for (uint32_t i = 0; i < nqueues; i++) {
            auto* q = new Vq;
            q->impl = this;
            q->qid = i;
            vqs.push_back(q);
        }
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
        // Only here, never in rollback(): a rolled-back device must stay able to
        // start again, and these are the slots it starts.
        for (auto* q : vqs)
            delete q;
        vqs.clear();
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

    // Nothing in here may wait for the serving side. The kernel blocks whoever
    // sent the message until this answers it, and gives up after msg_timeout
    // seconds -- a device whose replies stop is a device that has to be destroyed
    // and re-created. So the two branches that invalidate a ring only record that
    // they did: `gen` tells a refresh already in flight to withhold its publish,
    // and anything that genuinely has to wait for the requests to finish belongs
    // to teardown, which runs under detach's own unbounded contract.
    void handle_msg(const vduse_dev_request* req) {
        switch (req->type) {
        case VDUSE_SET_STATUS: {
            dev_status = req->s.status;
            LOG_INFO("vduse ` status -> 0x`", name, HEX(dev_status));
            if (dev_status == 0) {          // reset: stop serving, keep the session
                for (uint32_t i = 0; i < nqueues; i++) {
                    auto* q = vqs[i];
                    // Bump BEFORE clearing. A refresh already in flight snapshots
                    // the generation as its first act and re-reads it immediately
                    // before it publishes, so an increment that has landed by then
                    // makes it stay silent and this clear is the last word on
                    // `ready`. Incrementing after the clear would instead leave the
                    // whole resolve -- an ioctl and up to three mmap round trips --
                    // as a window in which a refresh publishes over the clear, and
                    // dispatch then runs on a ring the driver has torn down.
                    q->x.gen.fetch_add(1, std::memory_order_relaxed);
                    q->x.ready.store(false, std::memory_order_relaxed);
                    // the coming negotiation restarts the ring counters at 0
                    q->x.reset_pending.store(true, std::memory_order_relaxed);
                }
            } else if (dev_status & VIRTIO_CONFIG_S_FEATURES_OK) {
                uint64_t f = 0;
                if (::ioctl(dev_fd, VDUSE_DEV_GET_FEATURES, &f) == 0) {
                    negotiated = f;
                    // DEV_GET_FEATURES returns the NEGOTIATED subset and is only
                    // valid once FEATURES_OK is set (<linux/vduse.h>:94-99), which
                    // is exactly where we are.
                    // A store, not an assign: should_notify reads event_idx from
                    // the serving side, this handler writes it from the control
                    // plane. The negotiation is device-wide, so every queue gets
                    // the same word.
                    for (uint32_t i = 0; i < nqueues; i++)
                        vqs[i]->srv.event_idx.store(!!(negotiated & (1ULL << VIRTIO_RING_F_EVENT_IDX)),
                                                    std::memory_order_relaxed);
                }
            }
            if (dev_status & VIRTIO_CONFIG_S_DRIVER_OK) {
                // SET_STATUS carries no index, so every queue is flagged; the
                // refresh that consumes the flag is per queue. Resolved by the vq
                // loop: the msg handler must stay clear of the IOTLB_GET_FD ioctl,
                // whose kernel domain lock can be held by a path that is itself
                // waiting for a msg reply (a probe run deadlocked so for 200s)
                for (uint32_t i = 0; i < nqueues; i++)
                    vqs[i]->x.needs_refresh.store(true, std::memory_order_relaxed);
            }
            reply(req->request_id, VDUSE_REQ_RESULT_OK, 0, 0);
            break;
        }
        case VDUSE_UPDATE_IOTLB:
            LOG_DEBUG("vduse ` iotlb update [`, `]", name, req->iova.start, req->iova.last);
            iotlb.invalidate(req->iova.start, req->iova.last);
            if (req->iova.start == 0 && req->iova.last == 0) {
                // unmap-all: the vring is gone. Same generation bump as the reset
                // above, and invalidating first is what makes both cases come out
                // right: a refresh that has not resolved yet finds the ranges gone
                // from the cache and fails instead of publishing them, and one that
                // had already resolved them is silenced by the bump. The munmap of
                // those ranges is not this handler's problem either -- flush_stale
                // only unmaps once every queue is neither ready nor holding a
                // request.
                for (uint32_t i = 0; i < nqueues; i++) {
                    auto* q = vqs[i];
                    q->x.gen.fetch_add(1, std::memory_order_relaxed);
                    q->x.ready.store(false, std::memory_order_relaxed);
                }
            }
            reply(req->request_id, VDUSE_REQ_RESULT_OK, 0, 0);
            break;
        case VDUSE_GET_VQ_STATE: {
            uint32_t idx = req->vq_state.index;
            if (idx >= nqueues) {
                // Refuse rather than answer: replying OK with some queue's
                // counter -- or with a fabricated 0 -- would hand a driver that
                // believes it has more queues a ring position to resume from
                // for a queue this device does not have.
                LOG_ERROR("vduse ` GET_VQ_STATE for index ` of ` queues", name, idx, nqueues);
                reply(req->request_id, VDUSE_REQ_RESULT_FAILED, idx, 0);
                break;
            }
            // Read on the queue's own vcpu: last_avail is a plain field that
            // dispatch_avail advances there. Hopping is enough -- the kernel asked
            // to READ a counter, not to stop the vq, so stopping and draining for
            // it would put an unbounded wait in front of a reply the kernel times
            // out at msg_timeout. (vhost-user's GET_VRING_BASE does quiesce, but
            // that message's own protocol meaning is "stop this vq and return it".)
            uint16_t last_avail = 0;
            run_on_home(vqs[idx]->x.home, [&] { last_avail = vqs[idx]->srv.last_avail; });
            LOG_DEBUG("vduse ` GET_VQ_STATE vq`, last_avail `", name, idx, last_avail);
            reply(req->request_id, VDUSE_REQ_RESULT_OK, idx, last_avail);
            break;
        }
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
    int vq_refresh(uint32_t idx) {
        auto* q = vqs[idx];
        // First statement, before anything below can yield: the publish at the
        // bottom is withheld if the control plane invalidates this ring while we
        // are resolving it, and "while" has to span the whole resolve. The only
        // yield point in it is the iotlb cache's mutex (the ioctls and the mmaps
        // are blocking syscalls, which park the OS thread rather than handing the
        // vcpu over), but a snapshot taken after that mutex would miss an
        // invalidation that landed inside it.
        uint32_t gen_snapshot = q->x.gen.load(std::memory_order_relaxed);
        // The cache's counter, for the EFAULT branch below. The queue's own is not
        // enough there: a PARTIAL UPDATE_IOTLB bumps the cache's only -- it leaves
        // the queue's untouched, clears no readiness and sets no needs_refresh --
        // and it is an invalidation that can fail a resolve already in flight --
        // a partial update and an unmap-all alike, since both go through
        // invalidate(). Read under the cache's lock, so it can yield; that costs
        // nothing here, because an invalidation the yield lets go first is one this
        // snapshot already includes and the resolves below already see.
        uint64_t iotlb_gen_snapshot = iotlb.generation();
        vduse_vq_info vi;
        memset(&vi, 0, sizeof(vi));
        // the index is ours to supply: we are asking the kernel about this queue,
        // it is not telling us which one a message referred to
        vi.index = idx;
        if (::ioctl(dev_fd, VDUSE_VQ_GET_INFO, &vi) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse VQ_GET_INFO failed, dev `", name);
        // all five of these arrive cleared together after a device reset, and num
        // is not validated on the way in -- so num belongs in this guard as much
        // as the addresses do. It is a modulo divisor in dispatch_avail and in
        // vring_used_append; a 0 here with ready set divides by zero.
        if (!vi.ready || !vi.num || !vi.desc_addr || !vi.driver_addr || !vi.device_addr) {
            q->x.ready.store(false, std::memory_order_relaxed);
            return 0;
        }
        q->srv.num = vi.num;
        q->srv.desc = (vring_desc*)iotlb.resolve(vi.desc_addr, (size_t)vi.num * sizeof(vring_desc));
        q->srv.avail = (vring_avail*)iotlb.resolve(vi.driver_addr, sizeof(uint16_t) * (3 + vi.num));
        q->srv.used = (vring_used*)iotlb.resolve(vi.device_addr,
                        sizeof(uint16_t) * 3 + sizeof(vring_used_elem) * vi.num);
        if (!q->srv.desc || !q->srv.avail || !q->srv.used) {
            q->x.ready.store(false, std::memory_order_relaxed);
            // Two causes land here and only one of them is retryable, so they have
            // to be told apart. The cache's generation guard is coarse -- it cannot
            // tell our range from some other one that moved -- so an invalidation
            // of an unrelated range landing mid-resolve fails this check too, as
            // does an unmap-all racing it. That failure is transient:
            // the address space has settled by the time we get here, so re-arm the
            // flag this refresh was called under and let the next tick resolve
            // against the new state. The re-arm is the whole fix -- vq_tick
            // exchanged the flag away before calling us, and a partial
            // UPDATE_IOTLB brings no later DRIVER_OK to set it again, so without
            // this the queue stays not-ready for good and the device silently
            // serves nothing.
            //
            // The queue's own counter is in the test for a different reason: a bare
            // device reset touches no mapping, so it cannot fail a resolve -- it
            // reaches the withheld publish below instead. What a reset can leave
            // here is a ring whose IOVAs are gone for good, and re-arming that is
            // harmless because the retry re-reads the vq info and returns quietly
            // on `ready == 0`.
            //
            // With NEITHER generation moved, the vring's IOVAs are simply not
            // mapped: retrying cannot help and the driver has to renegotiate, so
            // leave the flag consumed. Leaving it is not optional either. loop()
            // calls hooks.tick every millisecond while a queue is not ready, so
            // re-arming a genuine failure would repeat an ioctl, up to three
            // resolutions and an error log a thousand times a second. This path
            // logs at debug rather than error, and that says "a race is not a
            // fault" -- it does not make the retry quiet, because this build's
            // default level prints debug and resolve() logs its own failures at
            // error regardless. What bounds the noise is that a retry only happens
            // when an invalidation really landed, so the rate is the driver's, not
            // the tick's.
            if (iotlb.generation() != iotlb_gen_snapshot ||
                q->x.gen.load(std::memory_order_relaxed) != gen_snapshot) {
                q->x.needs_refresh.store(true, std::memory_order_relaxed);
                LOG_DEBUG("vduse vq` refresh raced an iotlb invalidation, retrying, dev `", idx, name);
                return 0;
            }
            LOG_ERROR_RETURN(EFAULT, -1, "vduse vring iova resolution failed, dev `", name);
        }
        if (!q->x.ready.load(std::memory_order_relaxed)) {
            // exchange, not load-then-store: the flag is set by the control plane
            // and consumed here, and a plain pair would drop a reset that lands
            // between the two accesses -- the ring counters would then not be
            // zeroed for the new negotiation
            if (q->x.reset_pending.exchange(false)) {
                // a fresh negotiation: the driver restarts its side of the
                // ring at 0, so we must too -- resuming from the stale
                // used->idx would make the driver consume old used entries
                // as completions and lose the new ones (observed: dd hangs)
                q->srv.used_idx = 0;
                q->srv.last_avail = 0;
                // same device object, driver renegotiated: notify_valid may
                // already be true from the previous negotiation, and leaving it
                // would cost the first completion after this reset its
                // unconditional notification
                q->srv.notify_valid.store(false, std::memory_order_relaxed);
            } else {
                // adoption of a live ring (no reset seen): resume where the
                // previous daemon left off; anything fetched-not-completed is
                // re-served (virtio-blk ops are idempotent)
                q->srv.used_idx = vring_used_idx(q->srv.used);
                q->srv.last_avail = q->srv.used_idx;
            }
        }
        // Establish avail_event == last_avail before the loop can sleep on the
        // kickfd. On adoption the two lines above set last_avail from the live
        // ring, which can be any value, while avail_event still holds what the
        // previous daemon published. See SPEC §3.2.
        q->srv.publish_avail_event();
        // Publish only if nothing invalidated the ring while we were resolving it.
        // Re-arm needs_refresh when we do not: vq_tick consumed that flag before
        // calling us, and DRIVER_OK will not come again, so a suppressed publish
        // without this would leave the queue permanently not-ready -- the device
        // silently stops serving.
        //
        // This narrows the publish/clear conflict from the whole resolve down to
        // the two adjacent instructions below; it does not close it. `gen` and
        // `ready` are two objects, so an invalidation whose bump and clear land
        // between the load and the store is still overtaken by the publish, and no
        // memory order fixes that -- seq_cst would order the two accesses, it
        // would not make them atomic together. Closing it formally means packing
        // the token and the readiness bit into one word and settling both with a
        // single compare-exchange. Accepted as it stands, for what the residue
        // costs: a queue reporting ready a little longer into a reset or an
        // unmap-all, so the requests it dispatches fail their resolve and complete
        // to the guest with an error -- wrong completions, inside a window where
        // the guest is already losing the ring, not corruption. Its mirror image
        // is flush_stale declining to munmap while ready is true, which errs
        // conservative. Neither is what this token exists to prevent, which is a
        // queue pinned not-ready and silent. The same two-objects residue is
        // already carried by event_idx, capacity and notify_valid. Task 10
        // re-weighs the packing: putting the loops on a WorkPool is what makes
        // this window reachable across OS threads at all.
        if (q->x.gen.load(std::memory_order_relaxed) == gen_snapshot) {
            q->x.ready.store(true, std::memory_order_relaxed);
            LOG_INFO("vduse ` vq` ready: num ` desc ` avail ` used ` resume at `",
                     name, idx, q->srv.num, HEX(vi.desc_addr), HEX(vi.driver_addr),
                     HEX(vi.device_addr), q->srv.last_avail);
        } else {
            q->x.needs_refresh.store(true, std::memory_order_relaxed);
            LOG_INFO("vduse ` vq` resolved, but invalidated while resolving: deferring the refresh",
                     name, idx);
        }
        return 0;
    }

    // ----- VirtQueueServer hooks: the vduse half of serving -----

    void* vq_translate(uint64_t addr, size_t len) { return iotlb.resolve(addr, len); }

    void vq_notify(uint32_t idx) {   // the used ring advanced and the driver asked for an IRQ
        if (::ioctl(dev_fd, VDUSE_VQ_INJECT_IRQ, &idx) < 0 && errno != EBADF && errno != ENODEV)
            LOG_WARN("vduse INJECT_IRQ failed, dev `, ", name, ERRNO());
    }

    bool vq_may_dispatch(uint32_t idx) {
        return vqs[idx]->x.ready.load(std::memory_order_relaxed);
    }

    // top of every engine loop iteration: resolve a deferred ring refresh, and
    // release mappings invalidated by UPDATE_IOTLB once no request can still
    // hold their VAs (a reset-per-cycle device would otherwise accumulate them)
    void vq_tick(uint32_t idx) {
        auto* q = vqs[idx];
        // exchange, not load-then-store: SET_STATUS sets this from the control
        // plane while this consumes it here, and a plain pair would drop a
        // DRIVER_OK that lands in between -- the vring would then never be
        // resolved and the device would silently serve nothing
        if (q->x.needs_refresh.exchange(false)) {
            if (vq_refresh(idx) < 0)
                LOG_ERROR("vduse vq refresh failed on `, ", name, ERRNO());
        }
        if (q->x.ready.load(std::memory_order_relaxed))
            return;
        // `stale` is device-wide -- one address space, one cache -- while the
        // readiness and the in-flight count that make a munmap safe are per queue.
        // So the flush waits for EVERY queue to be idle, not just this one: with
        // one queue that is the same test this always was, and with more it is the
        // only form that cannot take a mapping away from another queue's request.
        for (auto* o : vqs)
            if (o->x.ready.load(std::memory_order_relaxed) || o->srv.in_flight.load())
                return;
        iotlb.flush_stale();
    }

    // The hooks are bound with the Vq* as their context, so one allocation
    // carries both the engine and everything this transport knows about it.
    static void notify_thunk(void* a) {
        auto* q = (Vq*)a;
        q->impl->vq_notify(q->qid);
    }
    static bool ready_thunk(void* a) {
        auto* q = (Vq*)a;
        return q->impl->vq_may_dispatch(q->qid);
    }
    static void tick_thunk(void* a) {
        auto* q = (Vq*)a;
        q->impl->vq_tick(q->qid);
    }
    static void* translate_thunk(void* a, uint64_t addr, size_t len) {
        auto* q = (Vq*)a;
        return q->impl->vq_translate(addr, len);   // the iotlb is device-wide
    }
    static void* loop_thunk(void* a) {
        auto* q = (Vq*)a;
        q->srv.loop();
        return nullptr;
    }

    void vq_bind(uint32_t idx) {
        auto* q = vqs[idx];
        q->srv.backend = backend;
        // the LBA bound serve_chain enforces
        q->srv.capacity.store(capacity_sectors << 9, std::memory_order_relaxed);
        q->srv.stack_size = resolve_stack_size(cfg.stack_size);
        q->srv.read_only = read_only;
        q->srv.serial = "photon-vduse";
        q->srv.tag = name;
        q->srv.hooks.translate.bind(q, &translate_thunk);
        q->srv.hooks.notify.bind(q, &notify_thunk);
        q->srv.hooks.ready.bind(q, &ready_thunk);
        q->srv.hooks.tick.bind(q, &tick_thunk);
    }

    // ---- running queue-side work where the queue lives ----
    //
    // The loop coroutine and every request coroutine it spawned run on `home`,
    // which is a pool vcpu once BlkConfig::pool is set. Four things have to join
    // them there instead of running here on the control plane's vcpu: interrupting
    // and joining the loop, waiting out the requests it dispatched, the backlog
    // wait that reads last_avail and dereferences `avail` while counting on the
    // loop to keep advancing the former, and answering the kernel's vq state read,
    // which reports that same last_avail. photon::thread_migrate only accepts a
    // READY thread, so a caller cannot move itself -- hand the work to a coroutine
    // that can be moved.
    //
    // The TempDelegate and everything it captures outlive the call for the same
    // reason run_off_vcpu's do: the caller blocks on `done` and then joins.
    struct HomeArg {
        TempDelegate<void> body;
        photon::vcpu_base* home = nullptr;
        photon::semaphore done{0};   // NSDMI, not {}: semaphore's ctor is explicit
    };

    static void* home_thunk(void* a) {
        auto* ha = (HomeArg*)a;
        ha->body.fire();
        ha->done.signal(1);
        return nullptr;
    }

    void run_on_home(photon::vcpu_base* home, TempDelegate<void> body) {
        if (!home || home == photon::get_vcpu()) {
            // !home means this queue has no loop coroutine on another vcpu, and
            // nothing else: vq_start records `home` in the same stretch that creates
            // the loop, and that stretch yields only where a refused migration logs --
            // which leaves the loop on this vcpu. vq_stop clears `home` only after
            // joining that loop. So there is no window in which a loop runs elsewhere
            // and this reads null, which is the window where working in place would
            // race it. That has to hold for all four call sites, and they are not
            // alike. Three are teardown (vq_stop, vq_drain, vq_backlog_drain),
            // reached from stop_serving -- gated on `started`, which start() sets
            // only after the whole vq_start loop -- and from rollback, start()'s own
            // DEFER: for those a null already meant "this queue never started". The
            // fourth is msg_loop answering the kernel's vq state read, and start()
            // spawns msg_loop before it reaches the vq_start loop, so that reader is
            // ordered behind no start() at all. What it needs is weaker than what the
            // three above get -- a null must mean no writer of last_avail on ANOTHER
            // vcpu -- but the need is unconditional, because no `started` gate and no
            // rollback DEFER stand in front of this reader. Not that it has no
            // writer at all -- start()'s vq_refresh writes it for every
            // not-yet-ready queue between creating msg_loop and reaching the
            // vq_start loop, and a refused migration leaves a loop created but not
            // recorded yet. Both writers are on this vcpu, and the body msg_loop
            // passes is a single load with no yield in it, so nothing can interleave
            // with the read: the worst case is answering the kernel with a
            // pre-refresh value.
            body.fire();
            return;
        }
        HomeArg ha{body, home};
        auto th = photon::thread_create(&VduseDeviceImpl::home_thunk, &ha);
        if (!th) {
            // Cannot honour the vcpu rule, but leaving the queue up is worse: the
            // caller is tearing it down and the address space is about to go.
            LOG_ERROR("vduse: cannot create the coroutine for the serving vcpu, running it here");
            body.fire();
            return;
        }
        photon::thread_enable_join(th);
        if (photon::thread_migrate(th, home) < 0)
            LOG_WARN("vduse: cannot move the work back to the serving vcpu, ", ERRNO());
        ha.done.wait(1);
        photon::thread_join((photon::join_handle*)th);
    }

    // `_here` means "already on this queue's home vcpu". Callers use the
    // unsuffixed wrapper, which does the hop -- wrapping it at every call site
    // instead is how one of them gets missed, and a missed one is a cross-vcpu
    // interrupt/join.
    void vq_stop_here(uint32_t idx) {
        auto* q = vqs[idx];
        // Deliberately does NOT touch x.ready: that flag reports whether the ring
        // is resolved, which is not what stopping the coroutine says. Its clears
        // belong to the invalidation points and to stop_serving, which places them
        // around its own backlog wait.
        if (!q->th) return;
        q->srv.run.store(false, std::memory_order_relaxed);
        q->srv.wake();            // out of its kickfd wait
        photon::thread_interrupt(q->th);
        photon::thread_join((photon::join_handle*)q->th);
        q->th = nullptr;
    }

    // Cleared in the wrapper, not in vq_stop_here: that one runs after the hop, on
    // the serving vcpu, and `home` is only ever written on this one. Clearing it
    // here also keeps it alive for vq_backlog_drain, which stop_serving runs before
    // the stop.
    void vq_stop(uint32_t idx) {
        run_on_home(vqs[idx]->x.home, [&] { vq_stop_here(idx); });
        vqs[idx]->x.home = nullptr;
    }

    // The requests hold iovs into the iotlb mappings and complete into the used
    // ring, both of which the caller is about to unmap or re-read.
    void vq_drain(uint32_t idx) {
        run_on_home(vqs[idx]->x.home, [&] { vqs[idx]->srv.drain(); });
    }

    // Wait out what this queue still owes before it is stopped: the requests
    // already dispatched and, when the caller asked for an orderly handover, the
    // avail backlog the loop has not consumed yet. This reads last_avail and
    // dereferences avail WHILE counting on the loop to keep advancing them, so it
    // cannot run anywhere else. The dereference is the vduse-specific half: an
    // unmap-all clears `ready` without nulling the ring pointers, and the loop's
    // own flush_stale is what munmaps the pages `avail` points into. On home the
    // two sit in one coroutine, and the poll tests `ready` before it dereferences,
    // so a flush can never land between the test and the read.
    void vq_backlog_drain(uint32_t idx, bool drain_backlog) {
        run_on_home(vqs[idx]->x.home, [&] {
            auto* q = vqs[idx];
            if (!q->th)
                return;
            while (q->srv.in_flight.load() ||
                   (drain_backlog && q->x.ready.load(std::memory_order_relaxed) &&
                    q->srv.last_avail != vring_avail_idx(q->srv.avail)))
                photon::thread_usleep(1000);
        });
    }

    // Start one queue's loop coroutine, once its ring is resolved. Only start()
    // calls this: nothing in the message loop stops a queue, so nothing there has
    // to put one back either.
    int vq_start(uint32_t idx) {
        auto* q = vqs[idx];
        if (q->th)
            return 0;
        q->srv.stopping.store(false, std::memory_order_relaxed);
        q->srv.run.store(true, std::memory_order_relaxed);
        q->th = photon::thread_create(&loop_thunk, q);
        if (!q->th)
            LOG_ERRNO_RETURN(0, -1, "vduse: cannot create the vq` loop coroutine, dev `", idx, name);
        // enable_join BEFORE the migration -- it writes a flag in the thread's own
        // struct, which another OS thread owns once migrated. ublk's start_serving
        // uses this same order. What the order really buys is that nothing between
        // the create and the migration yields: a yield lets the loop run here first
        // and park in its kickfd wait -- WAITING, not READY -- and the migration
        // then refuses it. The default log path does not yield (alog takes a
        // spinlock); a caller-installed sink writing through a photon IFile would,
        // which is why there is no logging in between.
        photon::thread_enable_join(q->th);
        migrate_to_pool(cfg.pool, q->th);
        // After the migration, and that is not an exception to the rule above: this
        // is an inline field read, so it cannot yield, and the field it reads is
        // written only by do_thread_migrate under the thread's own lock. The thread
        // cannot have gone away either -- enable_join above keeps its struct alive
        // even if the loop exits at once, until vq_stop_here joins it.
        q->x.home = photon::get_vcpu(q->th);   // already final -- see its declaration
        return 0;
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
        bc->num_queues = (uint16_t)nqueues;
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
        cc->vq_num = nqueues;
        cc->vq_align = (uint32_t)sysconf(_SC_PAGESIZE);
        cc->config_size = sizeof(virtio_blk_config);
        fill_config(raw + sizeof(vduse_dev_config), sizeof(virtio_blk_config));
        if (::ioctl(ctrl_fd, VDUSE_CREATE_DEV, cc) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse CREATE_DEV failed, name `", name);
        created = true;
        return 0;
    }

    // The queue count of a registration we did NOT create. create_dev above is
    // the only place a count is ever declared, and DEV_SET_CONFIG's only call
    // site publishes capacity, so on an adopt the kernel's vq_num -- and with it
    // the num_queues the consumer reads out of config space -- still belongs to
    // the previous daemon while `nqueues` is still ours. Nothing reconciles the
    // two by itself, and the disagreement is silent rather than an error: the
    // consumer keeps spreading requests over every queue it was told about, and a
    // request that lands on one we never set up, resolved or started simply never
    // completes.
    //
    // The uapi has no readback of vq_num, so the count is measured instead. On
    // 7.0.0-31-generic the kernel's <linux/vduse.h> declares sixteen ioctls,
    // four of which this file does not copy, and vq_num appears in it only as
    // CREATE_DEV's input.
    // VDUSE_VQ_GET_INFO bounds the index we supply against the registration's own
    // count and answers an index at or beyond it with EINVAL -- measured on
    // 7.0.0-31-generic at vq_num 1, 4, 8, 64, 65, 128 and 1024, with and without
    // VQ_SETUP, so the first index it refuses IS the count. Its `num` field is no
    // use here: it reads 0 for an in-range index until a consumer drives the
    // device, VQ_SETUP's max_size included, so a detector built on num would
    // report "no such queue" for every queue that exists.
    int adopt_queue_count() {
        uint32_t n = nqueues;
        // At most MAX_QUEUES ioctls -- the walk starts at our own count, which is
        // at least 1, so index 0 is never tested -- and one in the usual case: any
        // registration whose count is at or below ours refuses the first probe.
        for (; n <= MAX_QUEUES; n++) {
            vduse_vq_info vi;
            memset(&vi, 0, sizeof(vi));
            vi.index = n;
            if (::ioctl(dev_fd, VDUSE_VQ_GET_INFO, &vi) == 0)
                continue;
            if (errno != EINVAL)
                LOG_ERRNO_RETURN(0, -1, "vduse VQ_GET_INFO failed while counting the registered queues, dev `", name);
            break;
        }
        if (n > MAX_QUEUES)
            // Refused, not clamped: serving fewer queues than the registration
            // declares is the defect this exists to remove, and there are no slots
            // to serve more with. The refusal leaves the registration alive,
            // because rollback destroys only what WE created -- and a count this
            // large cannot have come from our own create_dev, which clamps, so it
            // is somebody else's registration and leaving it alone is the only
            // correct outcome. A recovery loop will not get past it, though:
            // list_orphans decides "ours" from the tombstone alone, acquire_lock
            // plants one before we know whose device this is, and release never
            // unlinks -- so the name stays listed and is refused again on every
            // later run. That is intended, not a leak of ours.
            LOG_ERROR_RETURN(EINVAL, -1, "vduse ` is registered with more than ` virtqueues; refusing to adopt it",
                             name, MAX_QUEUES);
        if (n == nqueues)
            // The count is ours -- or lower, which this walk cannot tell
            // apart: it starts at our own count, so a narrower registration
            // refuses the first probe and looks exactly like agreement.
            // start() still refuses it later. The loops it then runs per
            // index ask the kernel about indices the registration may not
            // have, and vq_refresh's is the bounds check that is measured --
            // so this fails start() there at the latest. The message names
            // that step, not this mismatch.
            return 0;

        // Grow the slots BEFORE publishing the count: every reader of `nqueues`
        // indexes `vqs` with it. The constructor's own pattern, and the pointer
        // vector is what makes doing it here safe at all -- the addresses the
        // already-built slots are known by do not move.
        vqs.reserve(n);
        for (uint32_t i = nqueues; i < n; i++) {
            auto* q = new Vq;
            q->impl = this;
            q->qid = i;
            vqs.push_back(q);
        }
        nqueues = n;
        // Keep the invariant the constructor established -- F_MQ offered exactly
        // when there is more than one queue. It is not published by an adopt, but
        // this word outlives it: a shutdown followed by another start CREATES the
        // registration, and create_dev then declares the raised vq_num beside it.
        offer_features |= (1ULL << VIRTIO_BLK_F_MQ);
        LOG_INFO("vduse ` is registered with ` virtqueues while the config asks for `: serving the registered count",
                 name, n, cfg.queues);
        return 0;
    }

    // VQ_SETUP + kickfd; safe on both fresh and adopted devices (the kernel
    // only records max_size; the driver's negotiated size comes via GET_INFO)
    int setup_vq(uint32_t idx) {
        vduse_vq_config vqc;
        memset(&vqc, 0, sizeof(vqc));
        vqc.index = idx;
        vqc.max_size = (uint16_t)std::min<uint32_t>(cfg.queue_depth ? cfg.queue_depth
                                                                    : DEFAULT_VQ_SIZE,
                                                    MAX_VQ_SIZE);
        if (::ioctl(dev_fd, VDUSE_VQ_SETUP, &vqc) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse VQ_SETUP failed, dev `", name);
        vqs[idx]->srv.kickfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (vqs[idx]->srv.kickfd < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse kickfd creation failed, dev `", name);
        vduse_vq_eventfd ev;
        memset(&ev, 0, sizeof(ev));
        ev.index = idx;
        ev.fd = vqs[idx]->srv.kickfd;
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
            for (auto* q : vqs)
                // stop dispatching NOW; the un-dispatched avail backlog stays in
                // the ring for the next daemon
                q->x.ready.store(false, std::memory_order_relaxed);
        for (uint32_t i = 0; i < nqueues; i++) {
            // The backlog wait comes first and the engine-level `stopping` second,
            // and that order is load-bearing: the wait expects the loop to keep
            // consuming avail entries, and `stopping` is exactly what tells it to
            // leave them alone. Setting the flag first DEADLOCKS -- a hang, not a
            // stranded wait, and nothing times out to break it: `stopping` is the
            // very flag that makes the loop return, this wait counts on that same
            // loop to keep advancing last_avail until it catches the avail idx,
            // and the wait itself has no bound, so detach(true) never returns. The
            // case that turns this red is
            // VhostUserTest.detach_waits_for_the_avail_backlog in
            // blk/test/test-vhost-user.cpp; no vduse case pins it yet.
            //
            // drain the dispatched requests (they complete into the used ring
            // while dev_fd is still open); with drain_backlog the vq loop also
            // keeps fetching until the avail ring is empty (orderly handover)
            vq_backlog_drain(i, drain_backlog);
            // The device-level flag, and deliberately not at the top of this
            // function: it is what makes msg_loop exit, so from here until that
            // loop is joined the kernel's messages go unanswered. Hoisting it
            // above the wait would spend an unbounded wait with nobody replying,
            // which the destructor's fallback cannot afford -- it is the one
            // caller that still has a consumer attached, so the kernel really is
            // waiting on us, for msg_timeout seconds per message. It still has to
            // precede the msg_th interrupt: msg_loop treats an interrupt that
            // finds the flag false as EINTR and loops again, so the join would
            // never return.
            stopping = true;
            // from here the engine leaves in-flight requests uncompleted
            // (handover contract)
            vqs[i]->srv.stopping.store(true, std::memory_order_relaxed);
            vq_stop(i);   // join the vq loop: no further dispatch
        }
        if (msg_th) {
            photon::thread_interrupt(msg_th);
            photon::thread_join((photon::join_handle*)msg_th);
            msg_th = nullptr;
        }
        for (uint32_t i = 0; i < nqueues; i++) {
            // the pre-join drain cannot see a batch the vq loop dispatched in the
            // window before `stopping` took effect: those request coroutines hold
            // VAs into the mappings, so wait for them here -- unmapping under a
            // live request would be a use-after-free
            vq_drain(i);
            vqs[i]->x.ready.store(false, std::memory_order_relaxed);
            if (vqs[i]->srv.kickfd >= 0) {
                vduse_vq_eventfd ev;
                memset(&ev, 0, sizeof(ev));
                ev.index = i;
                ev.fd = VDUSE_EVENTFD_DEASSIGN;
                if (dev_fd >= 0)
                    ::ioctl(dev_fd, VDUSE_VQ_SETUP_KICKFD, &ev);
                ::close(vqs[i]->srv.kickfd);
                vqs[i]->srv.kickfd = -1;
            }
        }
        iotlb.clear();
        for (uint32_t i = 0; i < nqueues; i++) {
            vqs[i]->srv.desc = nullptr;
            vqs[i]->srv.avail = nullptr;
            vqs[i]->srv.used = nullptr;
        }
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
            // create_dev, the only place a queue count is declared, did not run
            // for this registration -- so ask the kernel what it holds instead of
            // assuming cfg.queues describes it.
            if (adopt_queue_count() < 0)
                return -1;
        }
        iotlb.dev_fd = dev_fd;
        registered = true;   // created or adopted: shutdown() may destroy it

        // After the DEFER, so a rejected pool unwinds through the same rollback as
        // every other start() failure, and before anything is bound or spawned.
        if (check_pool_engines(cfg.pool) < 0)
            return -1;
        // A partial failure unwinds through start()'s DEFER, and rollback() is
        // safe on a queue that never finished setup_vq because all five things
        // it does per queue degenerate there. It closes srv.kickfd: guarded on
        // `kickfd >= 0`, and kickfd is initialized to -1. It stops the loop
        // coroutine: vq_stop returns on a null `th`, which only vq_start sets,
        // and vq_start runs after this whole loop. It stores srv.stopping and
        // srv.run: both exist from construction, and nothing reads them before a
        // loop exists. It reaches the queue through run_on_home, in vq_stop and
        // again in vq_drain: x.home is recorded by vq_start, so it is
        // still null here, and the null branch runs the body in place instead of
        // hopping. And it calls srv.drain(): that spins on in_flight, which is
        // initialized to 0 and which a queue that never had a loop to dispatch
        // on never raised.
        for (uint32_t i = 0; i < nqueues; i++)
            if (setup_vq(i) < 0)
                return -1;
        apply_msg_timeout();

        stopping = false;
        dev_status = 0;
        negotiated = 0;
        for (uint32_t i = 0; i < nqueues; i++) {
            vq_bind(i);
            vqs[i]->srv.stopping.store(false, std::memory_order_relaxed);
            vqs[i]->srv.run.store(true, std::memory_order_relaxed);
        }
        msg_th = photon::thread_create11(&VduseDeviceImpl::msg_loop, this);
        if (!msg_th)
            LOG_ERRNO_RETURN(0, -1, "vduse: cannot create the msg loop coroutine, dev `", name);
        photon::thread_enable_join(msg_th);   // control plane: NOT migrated

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
            // ring. A store, not an assign: the consumer reads it from the
            // serving side, this runs on the control plane.
            for (uint32_t i = 0; i < nqueues; i++)
                vqs[i]->srv.event_idx.store(!!(negotiated & (1ULL << VIRTIO_RING_F_EVENT_IDX)),
                                            std::memory_order_relaxed);
        }
        // EVERY queue, not just queue 0: an adopted device receives no further
        // SET_STATUS, so nothing re-arms needs_refresh and this call is the only
        // refresh queues 1..n-1 will ever get -- a queue it skips keeps its ring
        // unresolved while the kernel keeps putting requests on it. It puts them
        // on vq_num queues, which is why nqueues has to equal vq_num on both
        // branches and is established two different ways: create_dev declares
        // vq_num FROM nqueues, and an adopt raises nqueues TO the vq_num the
        // registration already has (adopt_queue_count). The test stays `< 0`: a
        // resolve that raced an iotlb invalidation returns 0 with needs_refresh
        // re-armed and loop()'s first tick retries it; only a genuine failure
        // (ring IOVAs truly unmapped) may fail start().
        for (uint32_t i = 0; i < nqueues; i++)
            if (vq_refresh(i) < 0)
                return -1;

        // LAST, and the position is the point: everything above writes the ring
        // fields, `negotiated`/`event_idx` and the iotlb cache from this vcpu, so
        // it must all be finished before the loop that reads them goes live -- and
        // before BlkConfig::pool can put that loop on another OS thread, where
        // "finished before" stops being implied by "on the same vcpu".
        for (uint32_t i = 0; i < nqueues; i++)
            if (vq_start(i) < 0)
                return -1;

        started = true;
        ok = true;
        // `created` is what makes the feature word readable: on an adopt
        // create_dev never ran, so `offer` is the word a create would have
        // advertised and not what the registration holds.
        LOG_INFO("vduse device started, ", make_named_value("name", (const char*)name), VALUE(cfg.info.size),
                 "created=", (int)created, VALUE(nqueues), "offer=", HEX(offer_features));
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
        // serve_chain's LBA bound must grow with us. A store, not an assign:
        // serve_chain reads it from the serving side while resize() runs on the
        // control plane -- the same reason tcmu's dev_size/num_lbas are atomic.
        // Every queue enforces the same bound, so every queue gets the new one.
        for (uint32_t i = 0; i < nqueues; i++)
            vqs[i]->srv.capacity.store(new_size, std::memory_order_relaxed);
        cfg.info.size = new_size;
        LOG_INFO("vduse device resized, ", make_named_value("name", (const char*)name), VALUE(cur), VALUE(new_size));
        return 0;
    }

    void rollback() {
        // a failed start: stop anything spawned, then remove the registration
        // only if WE created it (an adopted orphan stays recoverable)
        // Set before the msg_th interrupt below: msg_loop treats an interrupt that
        // still finds this false as EINTR and loops again, so the join would not
        // return.
        stopping = true;
        for (uint32_t i = 0; i < nqueues; i++) {
            vqs[i]->srv.stopping.store(true, std::memory_order_relaxed);
            vqs[i]->srv.run.store(false, std::memory_order_relaxed);
            vq_stop(i);
        }
        if (msg_th) {
            photon::thread_interrupt(msg_th);
            photon::thread_join((photon::join_handle*)msg_th);
            msg_th = nullptr;
        }
        for (uint32_t i = 0; i < nqueues; i++)
            // request coroutines may still hold VAs into the mappings
            vq_drain(i);
        iotlb.clear();
        for (uint32_t i = 0; i < nqueues; i++)
            if (vqs[i]->srv.kickfd >= 0) {
                ::close(vqs[i]->srv.kickfd);
                vqs[i]->srv.kickfd = -1;
            }
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
