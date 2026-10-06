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
// no config readback ioctl, so an orphan record carries the name plus what our
// own tombstone recorded for it -- the capacity, which is what lets start()
// refuse an adoption whose size drifted (tombstone, like vhost-user).
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
// than one, split ring only (no RING_PACKED -- not offered, so the driver must not
// use it), indirect descriptors implemented in the shared engine but NOT offered
// here, with the three reasons at offer_features, IN/OUT/FLUSH/GET_ID requests, FEATURE_FLUSH +
// read_only + logical block size; FEATURE_DISCARD/WRITE_ZEROES are accepted in
// cfg.info.features but not offered yet. Serving runs on the caller's vcpu
// unless BlkConfig::pool names one, in which case each queue's loop coroutine
// is migrated into it. All virtio fields are little-endian (VERSION_1) and
// this file, like nbd/tcmu/ublk, assumes an LE host (x86_64/aarch64).

#include "blk.h"
#include "utils.h"
#include "vduse-uapi.h"   // the VDUSE uapi copies, shared with the test

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

static constexpr uint32_t DEFAULT_VQ_SIZE = 256;
static constexpr uint32_t MAX_VQ_SIZE = 1024;

// IOVA -> VA cache over VDUSE_IOTLB_GET_FD. Ranges are few (the vring's direct
// map + the kernel's bounce region); a linear scan is fine.

// Does a mapping the kernel reported as `perm` allow the access the caller wants?
// RW satisfies both; the two one-way values satisfy one each. Checked on the way out
// of a fetch and again on every cache HIT, because a hit is served from an mmap whose
// protection was fixed when it was fetched for somebody else's access -- and a write
// into a PROT_READ mapping is a fault in our own process, not an error the guest can
// be told about.
static bool iotlb_perm_allows(uint8_t perm, bool writable) {
    return writable ? (perm & VDUSE_ACCESS_WO) : (perm & VDUSE_ACCESS_RO);
}

// How much invalidated mapping one device may hold before vq_tick stops dispatch to
// release it. The trade is guest pages pinned in our address space against one request
// latency of throughput on this device. Chosen as an order of magnitude rather than
// derived: a steady-state device invalidates almost nothing, because a driver that maps
// its buffers once sends no update, while a driver that replaces ranges continuously
// would otherwise retain every replacement until the device stopped serving. Being wrong
// in either direction costs one quiesce or one retained 64 MiB, and changes no answer.
static constexpr size_t VDUSE_STALE_FLUSH_BYTES = 64ull << 20;

// How long that quiesce gives the queues to run out of requests before it holds the
// mappings and lets a later tick try again. Not derived from anything either: the
// retire that precedes the wait stops every queue admitting work, so the expected wait
// is one request latency and this is a ceiling on a wait that has no other bound, not a
// tuning knob. Being wrong in either direction costs one deferred flush or one longer
// stall of a device that is already not answering.
static constexpr uint64_t VDUSE_QUIESCE_DRAIN_US = 1000ull * 1000;

struct Iotlb {
    int dev_fd = -1;
    struct Map {
        uint64_t start, last;
        char* base;
        uint8_t perm;   // as the kernel reported it: what `base` was mmap'd for,
                        // and therefore what a later hit on this entry may do
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

    void* resolve(uint64_t iova, size_t len, bool writable) {
        // Both arguments are hostile: iova and len come from a descriptor the peer or
        // the guest wrote, or from a ring address the kernel reported. The sum is
        // refused here rather than left to the ioctl. The ioctl does reject an end
        // below its start, so a wrapped `iova + len - 1` fails there too -- but that
        // is a fact about this kernel and not a contract, and one wrap it cannot catch
        // is iova 0 with len 0, where `iova + len - 1` is UINT64_MAX and the request
        // reads as the whole address space. A cache hit never reaches the ioctl at
        // all, so it needs the guard regardless.
        if (!len || len - 1 > UINT64_MAX - iova)
            LOG_ERROR_RETURN(EINVAL, nullptr, "vduse iotlb resolve of an overflowing range, iova ` len `", iova, len);
        uint64_t gen0 = 0;
        {
            SCOPED_LOCK(lock);
            gen0 = gen;
            // The permission is part of the hit, not a property of the range: `base`
            // was mmap'd for whatever access the fetch that cached it asked for, so a
            // hit that ignores it can hand a write a PROT_READ mapping and take the
            // fault in our own process instead of refusing the request.
            for (auto& m : maps)
                if (iova_range_covers(m.start, m.last, iova, len) &&
                    iotlb_perm_allows(m.perm, writable))
                    return m.base + (iova - m.start);
        }
        // The slow path runs OUTSIDE the lock: the ioctl and the mmap both block,
        // and holding the lock across them would queue every other queue's request
        // coroutines behind one kernel round trip.
        vduse_iotlb_entry e;
        memset(&e, 0, sizeof(e));
        e.start = iova;
        e.last = iova + len - 1;   // cannot wrap: refused above
        int fd = (int)::ioctl(dev_fd, VDUSE_IOTLB_GET_FD, &e);
        if (fd < 0)
            LOG_ERRNO_RETURN(0, nullptr, "vduse IOTLB_GET_FD failed, iova ` len `", iova, len);
        DEFER(::close(fd));   // the mapping keeps its own reference
        // The lookup answers with the first mapping that OVERLAPS what was asked for
        // and overwrites start/last/perm/offset with that mapping's own, and nothing
        // on the kernel side checks that the mapping covers the request. So the answer
        // can be narrower than the question, and it can begin above it -- and both are
        // silently fatal downstream rather than merely wrong: `e.start > iova`
        // underflows the offset in the return below into a wild pointer, and a short
        // range hands back a VA whose last bytes lie past the mmap, which the engine
        // then reads or writes through. This is also where a request spanning two
        // mappings is answered, and it is refused rather than split: one descriptor's
        // buffer is one element of the engine's scatter list, so splitting it across
        // mappings would mean inventing a second element the chain walk never saw.
        if (!iova_range_covers(e.start, e.last, iova, len))
            LOG_ERROR_RETURN(EINVAL, nullptr, "vduse iotlb `[`,`] does not cover iova ` len `",
                             e.start, e.last, iova, len);
        // `perm` says which way the driver mapped these pages, and `prot` below is
        // derived from it, so the mmap itself always succeeds -- which is exactly why
        // the direction has to be checked here rather than left to the mapping. A
        // read-only mapping fetched on behalf of a write comes back as a VA the engine
        // will write through, and that surfaces as a fault in our own process instead
        // of as a status the guest can be told about.
        if (!iotlb_perm_allows(e.perm, writable))
            LOG_ERROR_RETURN(EACCES, nullptr, "vduse iotlb `[`,`] perm ` forbids the access asked for, iova ` len ` writable `",
                             e.start, e.last, (int)e.perm, iova, len, (int)writable);
        size_t sz = (size_t)(e.last - e.start + 1);
        int prot = PROT_READ;
        if (e.perm & VDUSE_ACCESS_WO) prot = PROT_WRITE;
        if (e.perm & VDUSE_ACCESS_RO) prot |= PROT_READ;
        void* base = ::mmap(nullptr, sz, prot, MAP_SHARED, fd, (off_t)e.offset);
        if (base == MAP_FAILED)
            LOG_ERRNO_RETURN(0, nullptr, "vduse iotlb mmap failed, iova ` map [`,`] off `",
                             iova, e.start, e.last, e.offset);
        char* hit = nullptr;
        bool invalidated = false;
        {
            SCOPED_LOCK(lock);
            // Re-scan before inserting: another coroutine can have fetched and
            // published this same range while the ioctl and mmap above ran. When its
            // entry allows our access too, it answers the request just as well, so
            // keep that one and drop ours rather than cache two copies of the same
            // IOVA range. When it does not -- it was fetched for the other direction,
            // the driver having mapped those pages one-way -- ours is published
            // alongside it, and the two entries for one range are told apart by the
            // permission every hit now checks. That costs a second mapping of pages we
            // could already reach; it does not give one question two answers.
            for (auto& m : maps)
                if (iova_range_covers(m.start, m.last, iova, len) &&
                    iotlb_perm_allows(m.perm, writable)) {
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
                maps.push_back(Map{e.start, e.last, (char*)base, e.perm});
        }
        if (hit || invalidated) {
            ::munmap(base, sz);
            return hit;   // nullptr when the range was invalidated under us
        }
        // Safe because containment was checked against these same bounds: e.start
        // cannot exceed iova, so this offset cannot go negative.
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
    // The range is whatever the driver replaced, and it is not ours to predict:
    // a full replacement arrives as the whole address space and a partial one as
    // any subrange, including the single byte at IOVA 0.
    //
    // The handler that answers that message does NOT call this: it holds the lock
    // across the drop and the retire together, using invalidate_locked, because an
    // entry is the driver's mapping and not our request. One mapping can cover the
    // whole address space, so an update to a subrange no ring lies in still drops
    // the entry the rings were resolved from -- asking whether the message's range
    // covers a ring answers no, and the VAs that were handed out are gone all the
    // same. What was actually dropped is the only thing that says whose mappings
    // went, and telling the queues has to be part of the same step: see the
    // handler.
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
    // Bytes of mapping invalidated but not yet released, for the caller that has
    // to bound them. Computed the same way flush_locked computes the length it
    // hands munmap, so the counter describes what is actually being held: a
    // range whose `last - start + 1` would wrap is one whose mmap in resolve()
    // asked for zero bytes and failed, so it is not in either vector to count.
    size_t stale_bytes() {
        SCOPED_LOCK(lock);
        size_t n = 0;
        for (auto& m : stale)
            n += (size_t)(m.last - m.start + 1);
        return n;
    }
    // The pair of the counter above: bytes of mapping still IN the lookup, i.e.
    // mmapped and reachable by a resolve() hit. The two together are the whole
    // footprint this cache holds in our address space, and the split is what says
    // which half of it is still answering requests and which is only waiting to be
    // released. Same expression as stale_bytes, so the note there about a wrapping
    // range covers this one too.
    size_t live_bytes() {
        SCOPED_LOCK(lock);
        size_t n = 0;
        for (auto& m : maps)
            n += (size_t)(m.last - m.start + 1);
        return n;
    }
    // Unlocked halves for clear(): photon::mutex is not recursive, so clear()
    // cannot call the lock-taking members above it. `lock` is already held.
    void invalidate_locked(uint64_t start, uint64_t last,
                           std::vector<Map>* dropped = nullptr) {
        gen++;   // resolve()'s in-flight slow path must not publish what this drops
        for (size_t i = maps.size(); i-- > 0; ) {
            auto& m = maps[i];
            if (m.start > last || m.last < start)
                continue;
            // copied before the erase below invalidates the reference into `maps`
            if (dropped)
                dropped->push_back(m);
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
    // Test-and-set for the bounded flush in vq_tick, which every queue's loop
    // calls: without it N loops would each quiesce the device for one cap.
    std::atomic<bool> quiescing{false};

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
        // exception -- it runs on the loop's side, where it reads the generation,
        // consumes reset_pending, re-arms needs_refresh and both reads and sets
        // refreshed_once. Relaxed only: nothing else is published through them.
        //
        // Readiness and its generation are ONE word because they are two halves of
        // one state transition, and two objects cannot be written as one. The
        // control plane invalidates by bumping the generation and clearing
        // readiness; a refresh publishes readiness only if the generation has not
        // moved while it was resolving. Kept apart, an invalidation that lands
        // between the refresh's load of the generation and its store of readiness
        // is overtaken by that store, and the queue then reports ready for a ring
        // that is gone. No memory order fixes that: ordering two accesses is not
        // making them atomic together. So both moves are one compare-exchange
        // each, in the members below, and every caller goes through those.
        //
        // Bit 0 is readiness, bits 1..31 the generation. 31 bits is enough because
        // this token never outlives a single vq_refresh -- it is snapshotted on
        // entry and compared only at that call's two exits -- so a wrap would need
        // 2^31 invalidations inside one resolve. A request, which does outlive the
        // refresh that dispatched it, carries the engine's own 64-bit generation
        // instead; that is a different token answering a different question.
        //
        // Stopping the queue around an invalidation instead is not available here:
        // the kernel blocks the sender of a message until that message is
        // answered, for msg_timeout seconds, so nothing the message loop waits for
        // may be as unbounded as the in-flight requests are.
        // The IOVA ranges this queue's three ring arrays were resolved from, in
        // the order desc, avail, used, as inclusive [start, last] pairs, with
        // {1, 0} -- the empty range, which intersects nothing -- meaning "no ring
        // is published". What they are for: an UPDATE_IOTLB names a range and
        // nothing else about this queue changes, so without them a replacement
        // that covers the ring leaves the serving loop reading descriptors from,
        // and writing completions into, pages the driver has just handed back --
        // pages the guest may already be using for something else.
        //
        // Atomics, and read as a set that can tear, because the writer is the
        // queue's own refresh on its own vcpu while the reader is the message
        // handler on the control plane's. A torn read can only MISS an
        // intersection, and only while a refresh is mid-flight -- which is
        // already covered from the other side: the handler invalidates the cache
        // first, and a refresh whose snapshot of the cache generation is stale
        // has its publish withheld and is re-armed, so it cannot publish a ring
        // resolved from mappings that were replaced underneath it. What a tear
        // therefore costs is one extra refresh, never a stale ring.
        std::atomic<uint64_t> ring_start[3] = {{1}, {1}, {1}};
        std::atomic<uint64_t> ring_last[3] = {{0}, {0}, {0}};
        std::atomic<uint32_t> ready_gen{0};
        std::atomic<bool> reset_pending{false};  // a status-0 reset: zero the ring
                                                 // counters at the next refresh
                                                 // (vs adoption resume)
        std::atomic<bool> needs_refresh{false};  // DRIVER_OK seen; the loop resolves
        // Set by ready_thunk when it returns true (the engine is about to read the
        // avail ring), cleared by tick_thunk at the top of the next loop iteration.
        // Between set and clear, no flush_stale may unmap this queue's ring mapping:
        // redispatch_backlog passes hooks.ready and then reads avail->idx without
        // re-checking readiness, and on a different vcpu a tick that sees zero
        // in_flight could otherwise unmap the pages that reader is about to
        // dereference. The flag is conservative -- it stays set for the whole
        // dispatch, not just the single read -- but that costs nothing because a
        // flush deferred by one tick lands on the next one.
        std::atomic<bool> reading{false};
        // True after the first successful vq_refresh publish. Distinguishes initial
        // adoption (derive last_avail from used.idx, which start() proved equal to the
        // previous daemon's consume cursor -- it refuses an adoption whose dispatched
        // entries were not all completed, so nothing is in flight to lose, and any
        // avail entries past that cursor are un-fetched backlog this daemon serves as
        // fresh work) from a live refresh after UPDATE_IOTLB (preserve the existing
        // last_avail, because used.idx counts completions and may lag behind
        // dispatched-but-uncompleted requests).
        //
        // Atomic for the same reason as every neighbour above: a status-0 reset clears
        // it from the msg loop's vcpu while vq_refresh reads and sets it on `home`,
        // which BlkConfig::pool can make another OS thread. Relaxed, likewise -- it
        // selects a mode and publishes no other data, and the case that needs an
        // ordering is already covered by something stronger: a refresh in flight when
        // the reset lands fails its publish against the generation it snapshotted, so
        // it never reaches the store below and the re-armed refresh is the one that
        // reads the cleared flag.
        std::atomic<bool> refreshed_once{false};

        bool ready() const {
            return ready_gen.load(std::memory_order_relaxed) & 1u;
        }
        uint32_t gen() const {
            return ready_gen.load(std::memory_order_relaxed) >> 1;
        }
        // Drop readiness alone. The ring did not resolve, which is not an
        // invalidation, so the generation must stay where it is: the refresh that
        // retries has to recognise its own snapshot.
        void clear_ready() {
            ready_gen.fetch_and(~1u, std::memory_order_relaxed);
        }
        // Invalidate: bump the generation AND clear readiness as one step. The
        // mask is what makes it both -- adding 2 alone would leave a ready bit set.
        void retire() {
            uint32_t s = ready_gen.load(std::memory_order_relaxed);
            while (!ready_gen.compare_exchange_weak(s, (s + 2u) & ~1u,
                                                    std::memory_order_relaxed))
                ;
        }
        // Publish readiness against the generation this refresh snapshotted. False
        // means that generation moved while the resolve was in flight, so the
        // publish is withheld and the caller re-arms instead. The loop re-tests the
        // GENERATION, not the readiness bit, and that is deliberate: a word already
        // ready at an unchanged generation publishes successfully instead of being
        // mistaken for an invalidation, which would re-arm the refresh forever.
        bool publish_ready(uint32_t g) {
            uint32_t s = ready_gen.load(std::memory_order_relaxed);
            while ((s >> 1) == g) {
                if (ready_gen.compare_exchange_weak(s, s | 1u,
                                                    std::memory_order_relaxed))
                    return true;
            }
            return false;
        }
        // Record the range the i-th ring array was resolved from. The end
        // saturates rather than wraps, and saturation only ever makes the
        // intersection test answer yes for an update it should have ignored --
        // the safe direction, since a false positive costs one refresh while a
        // false negative leaves the loop serving on replaced pages.
        void record_ring(int i, uint64_t start, uint64_t size) {
            uint64_t last = (size && size - 1 <= UINT64_MAX - start)
                          ? start + size - 1 : UINT64_MAX;
            ring_start[i].store(start, std::memory_order_relaxed);
            ring_last[i].store(last, std::memory_order_relaxed);
        }
        void clear_rings() {
            for (int i = 0; i < 3; i++) {
                ring_start[i].store(1, std::memory_order_relaxed);
                ring_last[i].store(0, std::memory_order_relaxed);
            }
        }
        // Does a mapping the update dropped cover any of this queue's rings? The
        // question an UPDATE_IOTLB has to answer per queue, and the reason the
        // answer is not "retire them all": retiring raises the ring generation,
        // and a request whose generation moved declines to complete, so retiring
        // a queue whose rings were untouched drops completions the guest is
        // waiting for.
        //
        // Asked about the dropped mapping and not about the message's own range,
        // because a mapping is the driver's and can be far wider than the range
        // that replaced it: one driver mapping over the whole address space is
        // dropped by an update to any byte of it, and every ring resolved through
        // that mapping loses its VA whether or not the byte lies in the ring.
        bool rings_in(uint64_t start, uint64_t last) const {
            for (int i = 0; i < 3; i++)
                if (iova_ranges_intersect(ring_start[i].load(std::memory_order_relaxed),
                                          ring_last[i].load(std::memory_order_relaxed),
                                          start, last))
                    return true;
            return false;
        }
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
        // callers and by msg_loop's answer to the kernel's vq state read. All of
        // those readers run on the control vcpu blk.h's IBlkDevice names, and `home`
        // is a plain pointer nothing here would notice a breach of: vq_start writes
        // it on the vcpu that called start(), while a caller on another one both
        // reads it and nulls it -- two OS threads, one field, nothing ordering them.
        // msg_loop honours the contract because start() creates it there and
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
        snprintf(lock_dir, sizeof(lock_dir), "%s", ldir);   // the controller's bounded copy
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

        // ACCESS_PLATFORM is not a compatibility bit this transport may drop or make
        // conditional: CREATE_DEV refuses a device that does not offer it, measured on
        // 7.0.0-31-generic with the rest of the payload held constant. The refusal does
        // not point here -- its message names the create, not the features -- so the
        // bit stays unconditional and this note is what keeps it that way.
        // VIRTIO_RING_F_INDIRECT_DESC (bit 28) is NOT offered here, though the engine
        // this transport shares with vhost-user walks tables. Three reasons, none of
        // them "the driver would build a table we cannot serve" -- that risk was
        // measured away: a driver not offered SEG_MAX limits itself to one data segment,
        // so it never builds a long chain either way.
        //   1. the iotlb's invalidation generation is DEVICE-wide, so one unrelated unmap
        //      fails every in-flight slow path, and a table adds one resolve per request
        //      to the count that exposure multiplies.
        //   2. retained mappings are released only once nothing is in flight, and past
        //      their budget the device stops dispatch on EVERY queue to force that
        //      moment -- a per-request table mapping feeds that budget.
        //   3. the rescue tool drains a backlog by walking chains, and it does not walk
        //      tables. Daemon death with an indirect backlog published is exactly the
        //      scenario this transport advertises, so that one is a capability loss
        //      rather than a slowdown.
        // Turning it on is therefore a one-line change here plus a re-run of this suite,
        // not a feature: the three wiring points are already in place.
        offer_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_F_ACCESS_PLATFORM) |
                         (1ULL << VIRTIO_RING_F_EVENT_IDX) |
                         (1ULL << VIRTIO_BLK_F_SEG_MAX) |
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
        // The effective half of the descriptor; blk.h documents each axis. `offered`
        // is what this implementation can serve at all, so it does NOT depend on what
        // was asked for -- that is what makes `features & ~offered` show a caller the
        // requests this transport accepted and cannot honour. The offer_features word
        // above is the negotiation input and stays conditional; this is the capability.
        // Adoption asks the registration for its queue count and nothing else, which is
        // why the value is QueueCountOnly rather than Full -- and that is a ceiling the
        // interface sets, not an omission here. The uapi can WRITE the device config
        // (VDUSE_DEV_SET_CONFIG, which is all the resize path uses it for) but offers no
        // readback of it, so a registration's capacity cannot be recovered from the
        // registration. Identity is not a comparable field either: the registration is
        // opened BY name, so a different name is a different registration rather than a
        // drifted one. What start() does compare against cfg.info.size is our OWN record,
        // the capacity the tombstone carries, and it refuses a disagreement. That is a
        // weaker source than asking the kernel would have been, and it does not raise the
        // grade, because a tombstone with no record in it leaves nothing to compare -- an
        // adopt then succeeds with a capacity other than the one the guest already has,
        // and the guest keeps its own. The descriptor says so instead of leaving it to be
        // discovered.
        cfg.info.offered = FEATURE_FLUSH;
        cfg.info.backlog = BlkBacklog::KernelSide;
        cfg.info.shutdown_refusal = BlkShutdownRefusal::RefusesWhenAttached;
        cfg.info.resize_effect = BlkResizeEffect::BestEffortNotify;
        cfg.info.adoption = BlkAdoption::QueueCountOnly;
        cfg.info.detach_no_wait = false;
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

    // The single writer for the descriptor's copy of the negotiated word, called only
    // where a negotiation was OBSERVED -- the FEATURES_OK handler. The engine's own
    // `negotiated` member is written in one more place, the adoption resync in start(),
    // which deliberately does not come through here: see the comment there.
    // Only VIRTIO_BLK_F_FLUSH maps to a FEATURE_* bit, because that is the only one of
    // the three this transport offers.
    void publish_negotiated(uint64_t f) {
        negotiated = f;
        cfg.info.negotiated = (f & (1ULL << VIRTIO_BLK_F_FLUSH)) ? FEATURE_FLUSH : 0;
    }

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
        release_backend();
        // Only here, never in rollback(): a rolled-back device must stay able to
        // start again, and these are the slots it starts.
        for (auto* q : vqs)
            delete q;
        vqs.clear();
        if (ctrl_fd >= 0) { ::close(ctrl_fd); ctrl_fd = -1; }
    }

    // Delete a backend this object owns and forget it either way, so that neither
    // the destructor nor a later start() can see it.
    void release_backend() {
        if (own_backend)
            delete backend;
        backend = nullptr;
        own_backend = false;
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
                    // One step, so a refresh already in flight cannot be overtaken:
                    // it snapshots the generation as its first act and its publish
                    // is a compare-exchange against that snapshot, so an
                    // invalidation that has landed by then makes the publish fail
                    // and this retire is the last word on readiness. Bumping and
                    // clearing as two stores would instead leave the whole resolve
                    // -- an ioctl and up to three mmap round trips -- as a window
                    // in which a refresh publishes over the clear, and dispatch
                    // then runs on a ring the driver has torn down.
                    q->x.retire();
                    // Bump the engine's generation so in-flight requests decline
                    // to complete into the old used ring. retire() above invalidates
                    // the transport's readiness token, but handle_req tests
                    // srv.generation, which only changes on set_ring()/clear_ring().
                    // Without this bump, a request that passed the generation check
                    // before the reset could still publish its completion after the
                    // driver has torn down the ring. The ring pointers stay valid --
                    // only the generation moves -- so a request mid-execution can
                    // still read them safely; it just won't complete.
                    q->srv.generation.fetch_add(1, std::memory_order_release);
                    // the coming negotiation restarts the ring counters at 0
                    q->x.reset_pending.store(true, std::memory_order_relaxed);
                    // next refresh is a fresh start, not a live one: derive
                    // last_avail from used.idx rather than preserving the old one
                    q->x.refreshed_once.store(false, std::memory_order_relaxed);
                }
            } else if (dev_status & VIRTIO_CONFIG_S_FEATURES_OK) {
                uint64_t f = 0;
                if (::ioctl(dev_fd, VDUSE_DEV_GET_FEATURES, &f) == 0) {
                    publish_negotiated(f);
                    // DEV_GET_FEATURES returns the NEGOTIATED subset and is only
                    // valid once FEATURES_OK is set (<linux/vduse.h>:94-99), which
                    // is exactly where we are.
                    // A store, not an assign: should_notify reads event_idx from
                    // the serving side, this handler writes it from the control
                    // plane. The negotiation is device-wide, so every queue gets
                    // the same word.
                    for (uint32_t i = 0; i < nqueues; i++) {
                        vqs[i]->srv.event_idx.store(!!(negotiated & (1ULL << VIRTIO_RING_F_EVENT_IDX)),
                                                    std::memory_order_relaxed);
                        // Derived from the negotiated word, never from offer_features,
                        // for the reason event_idx beside it gives. On THIS transport the
                        // value is always false today, because bit 28 is not offered --
                        // and it is wired anyway, so that turning the offer on is one
                        // line rather than a hunt for the three places this had to be
                        // written. See the offer_features comment for why it is off.
                        vqs[i]->srv.indirect_desc.store(!!(negotiated & (1ULL << VIRTIO_RING_F_INDIRECT_DESC)),
                                                        std::memory_order_relaxed);
                        // FLUSH absent from the negotiated word leaves the driver
                        // with no command that asks for persistence, so we cannot
                        // lean on one arriving later: a write has to be durable
                        // before its completion goes out. Decided here, where the
                        // word is known; the engine only acts on the decision.
                        vqs[i]->srv.write_through.store(!(negotiated & (1ULL << VIRTIO_BLK_F_FLUSH)),
                                                        std::memory_order_relaxed);
                    }
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
        case VDUSE_UPDATE_IOTLB: {
            uint64_t s = req->iova.start, l = req->iova.last;
            LOG_DEBUG("vduse ` iotlb update [`, `]", name, s, l);
            // Invalidate FIRST, and retire inside the same critical section. Two
            // reasons, and neither is about ordering the message.
            //
            // First, the invalidation bumps the cache generation, which is what
            // makes a refresh already in flight withhold its publish; the test
            // below has to run against ranges that refresh will not be allowed to
            // publish. Both orders retire the same queues, but only this one also
            // silences the refresh that is resolving them.
            //
            // Second, and the reason this holds the lock instead of calling
            // invalidate(): the flush in vq_tick takes this same lock, so doing the
            // drop and the retire as one step is what makes them one step for the
            // flush too. Split them -- drop, release, then retire -- and a tick on
            // another vcpu can flush in between, releasing pages a queue that is
            // still ready then dispatches into. Nothing in the loop yields: it is
            // atomic loads and one compare-exchange per queue.
            std::vector<Iotlb::Map> dropped;
            {
                SCOPED_LOCK(iotlb.lock);
                iotlb.invalidate_locked(s, l, &dropped);
                // Retire every queue whose RINGS a dropped entry covers, and arm the
                // refresh that re-resolves them from whatever the driver put there
                // instead.
                //
                // Not every queue, because retiring raises the ring generation and a
                // request whose generation moved declines to complete: retiring a
                // queue whose rings were untouched drops completions its guest is
                // waiting for. Arming is not optional either -- nothing else sets
                // that flag except a DRIVER_OK, which a replacement brings no later
                // one of, so without the arm the queue stays not-ready for good.
                //
                // Intersection and not containment: the entry a ring was resolved
                // from covers the ring's whole range, so it always intersects it,
                // and a second entry over the same IOVAs carrying the other
                // permission answers yes as well. That over-answer is the safe
                // direction -- it costs one queue one refresh -- while an
                // under-answer leaves a ready queue pointing into released pages.
                for (uint32_t i = 0; i < nqueues; i++) {
                    bool covered = false;
                    for (auto& m : dropped)
                        if (vqs[i]->x.rings_in(m.start, m.last)) {
                            covered = true;
                            break;
                        }
                    if (!covered)
                        continue;
                    vqs[i]->x.retire();
                    vqs[i]->x.needs_refresh.store(true, std::memory_order_relaxed);
                }
            }
            reply(req->request_id, VDUSE_REQ_RESULT_OK, 0, 0);
            break;
        }
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
        uint32_t gen_snapshot = q->x.gen();
        // The cache's counter, for the two places below that re-check it. The
        // queue's own is not enough: an UPDATE_IOTLB that misses this queue's rings
        // bumps the cache's only -- it leaves the queue's untouched, clears no
        // readiness and sets no needs_refresh -- and it is still an invalidation
        // that can fail a resolve already in flight. One that hits them retires
        // this queue as well, which the publish's own compare-exchange catches
        // against the snapshot above; what only the cache's counter can answer is
        // whether the address space moved while the resolves were running. Read
        // under the cache's lock, so it can yield; that costs nothing here, because
        // an invalidation the yield lets go first is one this snapshot already
        // includes and the resolves below already see.
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
        // as the addresses do. dispatch_avail derives its in-flight cap from num
        // and returns before indexing a ring entry whenever in_flight is already
        // at that cap, so a published 0 caps the queue at nothing: no chain is
        // dispatched, none completes, and the queue stalls silently while still
        // marked ready. This guard makes it not-ready instead, and that is the
        // whole of what it buys -- the stall is not a fault anyone would see.
        if (!vi.ready || !vi.num || !vi.desc_addr || !vi.driver_addr || !vi.device_addr) {
            // No ring is published, so no range is advertised either: retiring a
            // queue for an update that cannot touch a ring it is serving would
            // raise its generation and drop completions for nothing. What this
            // leaves behind -- the previous ring's pointers still in `srv`, with
            // readiness cleared and no set_ring to bump the generation -- is only
            // safe because nothing can start a new dereference of them: the two
            // paths that admit work into a ring, loop()'s dispatch and the
            // redispatch a completion triggers, both consult readiness first. The
            // third reader is the completion of a request already dispatched,
            // which deliberately does NOT consult readiness -- a paused ring still
            // owes its completions -- and it is covered by the other half of the
            // argument instead: the flush in vq_tick releases mappings only while
            // every queue's in_flight is zero, and that request is counted in it
            // for as long as it runs.
            q->x.clear_rings();
            q->x.clear_ready();
            return 0;
        }
        // The three directions are not the same, and getting one wrong is not a
        // refused ring: a used ring resolved as read-only is published anyway, and the
        // first completion written into it faults in our own process.
        const uint64_t dsz = (uint64_t)vi.num * sizeof(vring_desc);
        const uint64_t asz = sizeof(uint16_t) * (3 + vi.num);
        const uint64_t usz = sizeof(uint16_t) * 3 + sizeof(vring_used_elem) * vi.num;
        auto* d = (vring_desc*)iotlb.resolve(vi.desc_addr, (size_t)dsz,
                                             false);   // the device reads descriptors
        auto* a = (vring_avail*)iotlb.resolve(vi.driver_addr, (size_t)asz,
                                              false);   // and the avail ring
        auto* u = (vring_used*)iotlb.resolve(vi.device_addr, (size_t)usz,
                        true);   // but writes the used ring
        // set_ring rather than four assignments, on the failure path as much as
        // on the success one: a ring that did not resolve is a ring that
        // changed, and the generation bump is what stops a request still in
        // flight against the previous one from completing into whatever comes
        // next.
        q->srv.set_ring(d, a, u, vi.num);
        if (!d || !a || !u) {
            q->x.clear_rings();
            q->x.clear_ready();
            // Two causes land here and only one of them is retryable, so they have
            // to be told apart. The cache's generation guard is coarse -- it cannot
            // tell our range from some other one that moved -- so an invalidation
            // of an unrelated range landing mid-resolve fails this check too, as
            // does an unmap-all racing it. That failure is transient:
            // the address space has settled by the time we get here, so re-arm the
            // flag this refresh was called under and let the next tick resolve
            // against the new state. The re-arm is the whole fix -- vq_tick
            // exchanged the flag away before calling us, and an UPDATE_IOTLB that
            // missed this queue's rings brings no later DRIVER_OK to set it again
            // (one that hit them sets it itself, in the handler), so without
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
                q->x.gen() != gen_snapshot) {
                q->x.needs_refresh.store(true, std::memory_order_relaxed);
                LOG_DEBUG("vduse vq` refresh raced an iotlb invalidation, retrying, dev `", idx, name);
                return 0;
            }
            LOG_ERROR_RETURN(EFAULT, -1, "vduse vring iova resolution failed, dev `", name);
        }
        if (!q->x.ready()) {
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
                // and indirect_desc goes false for the same reason: a true value left
                // here means a NEW negotiation that never carried bit 28 still walks
                // tables, i.e. the offer-instead-of-negotiated mistake returns on the
                // reset path.
                q->srv.indirect_desc.store(false, std::memory_order_relaxed);
            } else {
                // Adoption of a live ring, or a live refresh after UPDATE_IOTLB.
                // On adoption the cursor was validated in start(), and there are
                // three outcomes rather than two: the ring is fully quiescent (avail
                // idx == used idx == the previous daemon's cursor), or it carries
                // un-fetched backlog (used idx == cursor < avail idx, which is what
                // detach(false) leaves and what a kernel-side backlog exists for) --
                // and in both, used.idx IS the cursor, so the derivation below is
                // correct and the backlog is served as fresh work -- or start()
                // refused, because dispatched-but-uncompleted entries have no
                // recoverable identities. A refused adopt never reaches this line.
                //
                // On a live refresh the queue already has a valid last_avail from
                // before the invalidation. Deriving it from used.idx would rewind
                // the cursor: used.idx counts completions, not consumed avail
                // entries, so if requests A and B were dispatched and only B
                // completed, used.idx is 1 but last_avail is 2. Rewinding to 1
                // replays B, and combined with the generation bump that retired
                // the old ring, A's completion is dropped entirely.
                if (!q->x.refreshed_once.load(std::memory_order_relaxed)) {
                    q->srv.used_idx = vring_used_idx(q->srv.used);
                    q->srv.last_avail = q->srv.used_idx;
                }
                // else: preserve last_avail and used_idx from the previous ring
            }
        }
        // Establish avail_event == last_avail before the loop can sleep on the
        // kickfd. On adoption the two lines above set last_avail from the live
        // ring, which can be any value, while avail_event still holds what the
        // previous daemon published. See SPEC §3.2.
        q->srv.publish_avail_event();
        // Publish the ranges to the control plane BEFORE re-checking, and
        // re-check BEFORE publishing readiness. The order is what closes a window
        // resolve() leaves open: its own generation test covers only its slow
        // path, so a ring answered from the cache is a ring whose entry can move
        // to `stale` afterwards with nothing here noticing. Recording first means
        // an invalidation landing from now on is one the message handler can see
        // and answer with a retire, which makes the publish below fail against its
        // own snapshot; re-checking means one that landed while the resolves ran
        // is caught here instead. Neither half suffices alone -- recording without
        // the check misses an invalidation that landed before the record, and
        // checking without the record misses one that lands after it and
        // intersects only the new ring.
        q->x.record_ring(0, vi.desc_addr, dsz);
        q->x.record_ring(1, vi.driver_addr, asz);
        q->x.record_ring(2, vi.device_addr, usz);
        if (iotlb.generation() != iotlb_gen_snapshot) {
            q->x.clear_rings();
            q->x.clear_ready();
            q->x.needs_refresh.store(true, std::memory_order_relaxed);
            LOG_DEBUG("vduse vq` resolved but the address space moved, deferring the refresh, dev `", idx, name);
            return 0;
        }
        // Publish only if nothing invalidated the ring while we were resolving it.
        // Re-arm needs_refresh when we do not: vq_tick consumed that flag before
        // calling us, and DRIVER_OK will not come again, so a suppressed publish
        // without this would leave the queue permanently not-ready -- the device
        // silently stops serving.
        //
        // The test and the store are one compare-exchange, so nothing can land
        // between them and be overtaken by the publish. That is the whole reason
        // readiness and its generation share a word: as two objects this was a
        // check followed by a store, and an invalidation landing in between was
        // silently overwritten -- no memory order would have helped, because
        // ordering two accesses is not making them atomic together.
        //
        // An invalidation landing AFTER a successful publish needs no handling
        // here: retire() clears readiness itself, which is the right answer,
        // because the ring really was current at the moment it became ready. What
        // a publish can no longer do is survive one.
        if (q->x.publish_ready(gen_snapshot)) {
            q->x.refreshed_once.store(true, std::memory_order_relaxed);
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

    void* vq_translate(uint64_t addr, size_t len, bool writable) {
        return iotlb.resolve(addr, len, writable);
    }

    void vq_notify(uint32_t idx) {   // the used ring advanced and the driver asked for an IRQ
        if (::ioctl(dev_fd, VDUSE_VQ_INJECT_IRQ, &idx) < 0 && errno != EBADF && errno != ENODEV)
            LOG_WARN("vduse INJECT_IRQ failed, dev `, ", name, ERRNO());
    }

    bool vq_may_dispatch(uint32_t idx) {
        return vqs[idx]->x.ready();
    }

    // top of every engine loop iteration: resolve a deferred ring refresh, and
    // release mappings invalidated by UPDATE_IOTLB once no request can still
    // hold their VAs (a driver that replaces ranges continuously would otherwise
    // accumulate them without bound)
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
        // `stale` is device-wide -- one address space, one cache -- while the
        // in-flight count that makes a munmap safe is per queue, so the flush waits
        // for every queue to have no request outstanding.
        //
        // Readiness is no longer part of that test, and dropping it is what makes
        // the flush reachable at all: a device that is serving has ready queues, so
        // the old condition amounted to "never, while working". What made readiness
        // look necessary was a ready queue holding ring pointers into a mapping
        // that had been invalidated. That state is what the message handler now
        // rules out, in one step with the invalidation that could create it: an
        // entry that leaves the lookup retires every queue whose rings it covered,
        // under the same lock this flush takes. So the invariant the flush relies on
        // is that a ready queue's ring mappings are still in the lookup, and the
        // three readers of a ring pointer are covered by two different facts -- the
        // two that admit work into a ring consult readiness, and the third, a
        // completion already running, is counted in the in_flight this waits for.
        // The reading flag covers the window between hooks.ready passing and the
        // avail ring read completing: a reader that passed ready but has not yet
        // touched the ring is not counted in in_flight (it was decremented by the
        // handle_req DEFER before redispatch_backlog ran), so without this check
        // a flush on another vcpu could unmap the pages it is about to read.
        if (!any_in_flight() && !any_reading()) {
            iotlb.flush_stale();
            return;
        }
        // Bounded, not merely opportunistic: a saturating guest has some request
        // outstanding at every tick, so the flush above can be starved for as long
        // as the driver keeps replacing ranges. Past the cap this stops dispatch
        // device-wide, waits the requests out and releases the mappings. It costs
        // one request latency of throughput on this device, it cannot deadlock --
        // readiness is cleared before the wait and stays cleared for the whole of
        // it, so in_flight can only fall, and the wait has a deadline of its own --
        // and it is bounded by the cap rather than by the driver's behaviour.
        if (iotlb.stale_bytes() >= VDUSE_STALE_FLUSH_BYTES)
            quiesce_and_flush();
    }

    bool any_in_flight() {
        for (auto* o : vqs)
            if (o->srv.in_flight.load())
                return true;
        return false;
    }

    // True when any queue's engine is between passing hooks.ready and finishing
    // its avail ring read. A flush that runs while this is true can unmap a ring
    // that a reader on another vcpu is about to dereference through, so the flush
    // must wait. See the `reading` flag's declaration for the full argument.
    bool any_reading() {
        for (auto* o : vqs)
            if (o->x.reading.load(std::memory_order_relaxed))
                return true;
        return false;
    }

    // One tick owns this. Every queue's loop calls vq_tick, so without the
    // test-and-set N of them would each retire the device and each wait it out --
    // N quiesces for one cap, and the later ones would find nothing to release.
    void quiesce_and_flush() {
        bool expected = false;
        if (!quiescing.compare_exchange_strong(expected, true))
            return;
        DEFER(quiescing.store(false));
        size_t held = iotlb.stale_bytes();
        // Retire every queue AND consume its refresh flag, so nothing re-publishes
        // readiness while the wait below is running. Readiness is what both paths
        // into a ring consult -- the loop's dispatch and the redispatch a completion
        // triggers -- so from here no queue admits another request and the counts
        // can only fall, which is the whole reason this terminates.
        //
        // Arming the flag here instead, which is where the first version of this had
        // it, hands the device straight back: every other queue's loop calls its own
        // tick, sees the flag, re-resolves and is ready again within a millisecond,
        // and a guest that keeps the ring full then holds in_flight above zero for
        // as long as it likes. The wait below would never end, this coroutine would
        // never return to its own loop, and `quiescing` would stay set so that no
        // later tick could try again -- one queue permanently out of service and the
        // bound it existed to enforce gone with it.
        for (auto* o : vqs) {
            o->x.retire();
            o->x.needs_refresh.store(false, std::memory_order_relaxed);
        }
        // Bounded rather than VirtQueueServer::drain(), which waits without a
        // deadline. Retiring first makes the deadline a formality in every case this
        // can be reached for, and what it is really for is the interleaving that can
        // still add work -- a renegotiation landing mid-quiesce sets the flag this
        // just consumed -- and a backend that has stopped answering. Giving up is the
        // cheap outcome: the mappings stay where they were, and the cap is still
        // exceeded, so a later tick tries again.
        uint64_t deadline = photon::now + VDUSE_QUIESCE_DRAIN_US;
        for (auto* o : vqs)
            while ((o->srv.in_flight.load() || o->x.reading.load(std::memory_order_relaxed)) &&
                   photon::now < deadline)
                photon::thread_usleep(1000);
        bool drained = !any_in_flight() && !any_reading();
        if (drained)
            iotlb.flush_stale();
        // Every queue was retired, so every queue needs its ring re-resolved -- the
        // ones that drained and the ones that did not. This is also what lets the
        // queue whose loop is running here come back: it is inside its own tick, and
        // will not call vq_refresh until the flag is set.
        for (auto* o : vqs)
            o->x.needs_refresh.store(true, std::memory_order_relaxed);
        if (drained)
            LOG_INFO("vduse ` quiesced every queue to release ` bytes of invalidated mappings", name, held);
        else
            LOG_WARN("vduse ` quiesce still had requests in flight after ` us, holding ` bytes of invalidated mappings",
                     name, VDUSE_QUIESCE_DRAIN_US, held);
    }

    // The hooks are bound with the Vq* as their context, so one allocation
    // carries both the engine and everything this transport knows about it.
    static void notify_thunk(void* a) {
        auto* q = (Vq*)a;
        q->impl->vq_notify(q->qid);
    }
    static bool ready_thunk(void* a) {
        auto* q = (Vq*)a;
        bool r = q->impl->vq_may_dispatch(q->qid);
        if (r)
            q->x.reading.store(true, std::memory_order_relaxed);
        return r;
    }
    static void tick_thunk(void* a) {
        auto* q = (Vq*)a;
        // Clear BEFORE the tick body: the previous iteration's dispatch is done,
        // and this tick may flush stale mappings. The flag must be clear so the
        // flush sees no active reader on this queue.
        q->x.reading.store(false, std::memory_order_relaxed);
        q->impl->vq_tick(q->qid);
    }
    static void* translate_thunk(void* a, uint64_t addr, size_t len, bool writable) {
        auto* q = (Vq*)a;
        return q->impl->vq_translate(addr, len, writable);   // the iotlb is device-wide
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
        q->srv.stack_size = cfg.stack_size;
        q->srv.queue_depth = cfg.queue_depth;
        q->srv.read_only = read_only;
        // The device name, which is the identity and is unique in the transport's own
        // namespace -- two registrations cannot share it. A fixed per-transport string
        // made every device this daemon served report the same serial to its guest.
        // GET_ID truncates to VIRTIO_BLK_ID_BYTES, so names sharing that prefix still
        // collide; the field is 20 bytes wide and nothing here can widen it.
        q->srv.serial = name;
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
        // Deliberately does NOT touch readiness: that bit reports whether the ring
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
                   (drain_backlog && q->x.ready() &&
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
        // §5.2.3, and only meaningful because F_SEG_MAX is offered; see
        // VIRTIO_BLK_SEG_MAX_ADVERTISED for why the value is the entry cap minus two.
        // On this transport bit 28 is NOT offered, so a driver takes this as a DIRECT
        // chain bound: it may now publish that many data descriptors plus the two
        // framing ones, which lands exactly on the engine's MAX_DESC_CHAIN. That is
        // deliberate, and it is what this suite's full regression checks.
        bc->seg_max = VIRTIO_BLK_SEG_MAX_ADVERTISED;
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
    // only records max_size; the driver's negotiated size comes via GET_INFO).
    // So queue_depth reaches this device twice and the two are not redundant:
    // max_size is what the driver is OFFERED, and therefore bounds the ring it
    // may negotiate, while the engine's own cap bounds the requests in flight
    // inside whatever ring actually arrived. The second binds on its own only
    // if that ring came back deeper than the offer.
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
                q->x.clear_ready();
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
            // from here the engine leaves in-flight requests uncompleted
            // (handover contract)
            vqs[i]->srv.stopping.store(true, std::memory_order_relaxed);
            vq_stop(i);   // join the vq loop: no further dispatch
        }
        // The device-level flag, deliberately after EVERY queue has drained and
        // deliberately not at the top of this function: it is what makes msg_loop
        // exit, so from here until that loop is joined the kernel's messages go
        // unanswered. Hoisting it above the waits would spend an unbounded wait
        // with nobody replying, which the destructor's fallback cannot afford -- it
        // is the one caller that still has a consumer attached, so the kernel really
        // is waiting on us, for msg_timeout seconds per message.
        //
        // "Above the waits" includes inside the loop they are in. Per queue this
        // used to be drain, then flag, then stop, so the flag was set on the first
        // iteration and every later queue drained with msg_loop already silent --
        // the same unbounded wait with nobody replying, reached by a loop rather
        // than by a hoist, and only on a multiqueue device. It still has to precede
        // the msg_th interrupt: msg_loop treats an interrupt that finds the flag
        // false as EINTR and loops again, so the join would never return.
        stopping = true;
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
            vqs[i]->x.clear_ready();
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
            // the mappings every ring pointer was translated through are gone;
            // clear_ring drops the size with them, so nothing is left describing
            // a ring that no longer exists
            vqs[i]->srv.clear_ring();
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

    // ----- the tombstone payload: the capacity the registration serves -----
    //
    // The uapi can WRITE the device config and cannot read it back, so the one
    // fact an adopting daemon needs about a registration it did not create -- the
    // capacity the consumer was told -- has no kernel-side source. Our own
    // tombstone is the only place it can be kept, which is why it carries one and
    // why the record is written by this implementation rather than asked of the
    // device. It is the capacity in BYTES, the unit BlkDevInfo::size and resize()
    // both use, and it equals capacity_sectors << 9 because validate_info refuses
    // a size that is not a multiple of 512.
    //
    // Best-effort, and the direction of that is the point: a record that fails to
    // be written costs a LATER adopter the comparison and nothing else, so it
    // degrades to the behaviour every registration had before there were records
    // rather than to a wrong answer. That is why both writers warn instead of
    // failing the start() or the resize() that already did its real work.

    // 1 when the tombstone carries a record, 0 when it does not. A tombstone with
    // no record is REACHABLE and is not an error: acquire_lock plants the file
    // before start() knows whether it is creating or adopting, so a start() that
    // fails after that -- the queue-count refusal is one, and rollback destroys
    // only what WE created -- leaves a live registration behind an empty file.
    int read_capacity_record(uint64_t* out) {
        char ln[VDUSE_LOCK_BUF];
        vduse_lock_name(name, ln, sizeof(ln));
        uint64_t v = 0;
        if (devlock_read_payload(lock_dir, ln, &v, sizeof(v)) != (ssize_t)sizeof(v))
            return 0;
        *out = v;
        return 1;
    }
    void record_capacity() {
        char ln[VDUSE_LOCK_BUF];
        vduse_lock_name(name, ln, sizeof(ln));
        uint64_t bytes = capacity_sectors << 9;
        if (devlock_write_payload(lock_fd, &bytes, sizeof(bytes)) < 0)
            LOG_WARN("failed to record the capacity of vduse `, so a daemon adopting it later may not detect a size drift", name);
    }

    // The size half of what an adopt validates, and the reason it can only be a
    // refusal rather than a reconciliation: the consumer reads the capacity out of
    // the REGISTRATION's config space while this server bounds every request by
    // its OWN, so the two being different means one of them is serving a disk the
    // other does not agree about. Neither direction is benign. Ours smaller turns
    // every request past it into an I/O error the consumer has no way to explain;
    // ours larger is invisible until something asks for space the consumer was
    // never told it had. Rewriting the registration's config to match ours is not
    // the alternative either -- that changes the size of a disk a consumer is
    // already using, which is what resize() exists to do with its own contract.
    //
    // No record means nothing to compare and the adoption proceeds. Reading an
    // empty tombstone as capacity 0 instead would refuse every device whose
    // record never got written; see read_capacity_record.
    int validate_adopted_capacity() {
        uint64_t rec = 0;
        if (!read_capacity_record(&rec))
            return 0;
        uint64_t mine = capacity_sectors << 9;
        if (rec == mine)
            return 0;
        // The registration survives, as it does for adopt_queue_count()'s refusal
        // of a registration wider than this transport: rollback destroys only what
        // WE created, so this leaves it standing for whoever does own it -- and
        // leaves the name listed as an orphan, since acquire_lock planted a
        // tombstone that release never unlinks. A recovery loop that keeps asking
        // for this size is refused again on every run.
        LOG_ERROR_RETURN(EINVAL, -1, "vduse ` is registered with capacity ` while this config asks for `; refusing to adopt it",
                         name, rec, mine);
    }

    int start(fs::IFile* bk, bool ownership) override {
        if (started)
            LOG_ERROR_RETURN(EALREADY, -1, "vduse device already started");
        if (!bk)
            LOG_ERROR_RETURN(EINVAL, -1, "backend IFile is null");

        // detach() retains backend and own_backend (the caller keeps the backend
        // on a failed start, so rollback must not delete it), but a subsequent
        // start() with a NEW backend would overwrite both without releasing the
        // old ownership -- double-free at the next shutdown or destructor.
        if (own_backend && backend) {
            delete backend;
            backend = nullptr;
            own_backend = false;
        }

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
            // Set BEFORE the adoption checks below: they call iotlb.resolve()
            // which needs dev_fd for VDUSE_IOTLB_GET_FD. Without this, resolve()
            // fails with EBADF and the adoption loop treats every ring as "no
            // consumer attached", bypassing the non-quiescent-ring rejection.
            iotlb.dev_fd = dev_fd;
            // Capacity first: it is the cheaper question, and it is the one about
            // whether this registration is the device this config describes at all
            // rather than about how much of it we can serve. Both refuse the
            // adoption, and only this one is answerable without the kernel.
            if (validate_adopted_capacity() < 0)
                return -1;
            // create_dev, the only place a queue count is declared, did not run
            // for this registration -- so ask the kernel what it holds instead of
            // assuming cfg.queues describes it.
            if (adopt_queue_count() < 0)
                return -1;
            // The cursor half of what an adopt validates. Two questions get conflated
            // if this is read carelessly, and conflating them is what made this check
            // refuse adoptions it should have taken.
            //
            // (1) Can the IDENTITIES of dispatched-but-uncompleted entries be
            // recovered? No, and that is a property of the ring rather than of any
            // assumption about the driver. A split ring is a circular buffer of width
            // num: entry i and entry i+num share a slot, so publishing the newer
            // destroys the older one's head. The outstanding set has at most num
            // members but can span num+1 indices, so every width-num window misses at
            // least one -- and the one it misses is exactly the one whose slot was
            // overwritten, so its head exists nowhere in shared memory. Nor does an
            // ioctl give it back: the kernel's own reported avail_index was measured on
            // 2026-10-03 to be 0 after a daemon that had consumed one entry died, while
            // used_idx was 1 and the driver had published 2. Resuming from THAT would
            // re-serve everything and publish duplicate completions for heads the driver
            // had already reclaimed.
            //
            // (2) Can their EXISTENCE be detected? Yes, from one trusted cursor. The
            // engine publishes avail_event == last_avail per consumed head, into the
            // slot past the used ring, so that slot is the previous daemon's consume
            // cursor left behind in shared memory -- the only copy that outlives the
            // process. cursor - used_idx is then exactly the count of entries it took
            // and did not finish: sound because used->idx advances once per completed
            // entry and only dispatched entries are ever completed, so used_idx <= cursor
            // always, and a completion is written before the release store that publishes
            // it.
            //
            // Only (1) forces a refusal, and only when (2)'s count is non-zero. When the
            // count IS zero, every dispatched entry completed, so resuming from used->idx
            // neither loses nor duplicates -- and whatever sits past the cursor is backlog
            // the driver published and nobody fetched, which is exactly what detach(false)
            // leaves behind and what BlkBacklog::KernelSide promises a successor will pick
            // up. Refusing that state made adoption impossible against any consumer still
            // submitting: draining completes what was dispatched, but the driver publishes
            // again at once, so the two indices are not equal for long enough to adopt.
            // Measured 2026-10-05 on a registration the rescue tool then recovered cold:
            // cursor 75, used idx 75, avail idx 76 -- one un-fetched entry, served in a
            // single pass.
            //
            // The feature word is read here, separately from the resync below that reads
            // it for its own purpose: avail_event is maintained only under EVENT_IDX, so
            // without that bit the slot holds whatever preceded it and cannot be trusted,
            // and the fallback is the strict equality test. Two reads of one read-only
            // ioctl rather than one shared variable, because a failed read means two
            // different things -- here it narrows the check, there it leaves the engine
            // at its defaults -- and sharing it would make each site reason about the
            // other's ordering.
            uint64_t feat = 0;
            const bool trust_cursor = ::ioctl(dev_fd, VDUSE_DEV_GET_FEATURES, &feat) == 0 &&
                                      (feat & (1ULL << VIRTIO_RING_F_EVENT_IDX));
            for (uint32_t i = 0; i < nqueues; i++) {
                vduse_vq_info vi;
                memset(&vi, 0, sizeof(vi));
                vi.index = i;
                if (::ioctl(dev_fd, VDUSE_VQ_GET_INFO, &vi) < 0)
                    LOG_ERRNO_RETURN(0, -1, "vduse VQ_GET_INFO failed while checking the adopted ring state, dev `", name);
                if (!vi.ready)
                    continue;   // no ring yet; vq_refresh below will pick it up
                auto* a = (vring_avail*)iotlb.resolve(vi.driver_addr,
                                                      sizeof(uint16_t) * (3 + vi.num), false);
                auto* u = (vring_used*)iotlb.resolve(vi.device_addr,
                                                     sizeof(uint16_t) * 3 + sizeof(vring_used_elem) * vi.num,
                                                     true);
                // No mapping means no consumer is attached: the previous daemon
                // died and its IOTLB entries were reclaimed. A ring with no live
                // backing memory cannot have outstanding work that we would
                // duplicate, so there is nothing to refuse. When a new consumer
                // connects, vq_refresh will re-resolve the ring from scratch.
                if (!a || !u)
                    continue;
                uint16_t aidx = __atomic_load_n(&a->idx, __ATOMIC_ACQUIRE);
                uint16_t uidx = __atomic_load_n(&u->idx, __ATOMIC_ACQUIRE);
                if (!trust_cursor) {
                    // No cursor to consult, so the only state this can vouch for is
                    // one where the two indices agree on their own.
                    if (aidx != uidx)
                        LOG_ERROR_RETURN(EINVAL, -1,
                                         "vduse ` vq` is not quiescent on adopt (avail idx `, used idx `) and EVENT_IDX is not in the device's feature word, so there is no cursor to tell un-fetched backlog from dispatched-but-uncompleted entries; refusing to adopt it",
                                         name, i, aidx, uidx);
                    continue;
                }
                uint16_t cursor = vring_avail_event(u, vi.num);
                // Both are uint16 modular differences, so each is a count only while the
                // cursor sits inside the window [used idx, avail idx]. Outside it the
                // slot was not maintained -- a predecessor that ran without EVENT_IDX, or
                // a ring this engine never served -- and there is no sound reading of it.
                uint16_t dispatched = (uint16_t)(cursor - uidx);
                uint16_t backlog = (uint16_t)(aidx - cursor);
                if (dispatched >= 0x8000 || backlog >= 0x8000)
                    LOG_ERROR_RETURN(EINVAL, -1,
                                     "vduse ` vq` has an untrustworthy avail_event cursor on adopt (cursor `, used idx `, avail idx `): it lies outside the window the engine's invariants allow, so nothing here can say what the previous daemon left outstanding; refusing to adopt it",
                                     name, i, cursor, uidx, aidx);
                if (dispatched)
                    LOG_ERROR_RETURN(EINVAL, -1,
                                     "vduse ` vq` has ` dispatched-but-uncompleted entries on adopt (cursor `, used idx `, avail idx `): their identities cannot be recovered from a split ring, so resuming would both lose and duplicate them; refusing to adopt it",
                                     name, i, dispatched, cursor, uidx, aidx);
                if (backlog)
                    LOG_INFO("vduse adopting ` vq` with ` un-fetched avail entries (cursor `, used idx `, avail idx `); they are served as fresh work",
                             name, i, backlog, cursor, uidx, aidx);
            }
        }
        iotlb.dev_fd = dev_fd;
        registered = true;   // created or adopted: shutdown() may destroy it
        // Both branches land here and both need the same thing recorded: a create
        // writes the capacity create_dev just declared, while an adopt that got
        // this far either matched the record or found none -- so writing ours
        // upgrades a tombstone that carried nothing and repeats a value that
        // already agreed.
        record_capacity();

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
        publish_negotiated(0);
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
            // The engine's member only, NOT publish_negotiated(): the descriptor must
            // not claim a negotiation we did not observe. DEV_GET_FEATURES returns the
            // negotiated subset only once FEATURES_OK is set, and there is no ioctl
            // that tells us whether it is -- so on a fresh device this word is not a
            // negotiated one. Feeding it to the engine is safe and is what the comment
            // below argues; feeding it to a caller through get_info() would be a claim
            // about the guest that nothing here witnessed. An adopted, already-driven
            // device therefore reports negotiated=0 until it re-negotiates, which is
            // under-reporting rather than guessing.
            negotiated = f;
            // An adopted device is already FEATURES_OK from the daemon we took
            // over from, so this is the negotiated subset. A fresh device reaches
            // here too and gets a word that is not yet negotiated; that is
            // harmless because no ring can go live before the FEATURES_OK handler
            // re-derives these, and every consumer of them -- should_notify, and
            // the write path in handle_req -- needs a live ring to be reached at
            // all. A store, not an assign: the consumer reads it from the
            // serving side, this runs on the control plane.
            for (uint32_t i = 0; i < nqueues; i++) {
                vqs[i]->srv.event_idx.store(!!(negotiated & (1ULL << VIRTIO_RING_F_EVENT_IDX)),
                                            std::memory_order_relaxed);
                // Derived from the negotiated word, never from offer_features,
                // for the reason event_idx beside it gives. On THIS transport the
                // value is always false today, because bit 28 is not offered --
                // and it is wired anyway, so that turning the offer on is one
                // line rather than a hunt for the three places this had to be
                // written. See the offer_features comment for why it is off.
                vqs[i]->srv.indirect_desc.store(!!(negotiated & (1ULL << VIRTIO_RING_F_INDIRECT_DESC)),
                                                std::memory_order_relaxed);
                vqs[i]->srv.write_through.store(!(negotiated & (1ULL << VIRTIO_BLK_F_FLUSH)),
                                                std::memory_order_relaxed);
            }
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
        int r = do_shutdown();
        // blk.h's start() contract: an owned backend is deleted on shutdown, not
        // only by the destructor. The object outlives a shutdown() and the next
        // start() overwrites the pointer, so releasing it only at destruction
        // leaks the first backend. Gated on success because do_shutdown()'s
        // failures leave either a live consumer or a surviving registration
        // behind, and either can still be served through this backend -- the
        // destructor deletes it once the object itself goes, after stop_serving().
        if (r == 0)
            release_backend();
        return r;
    }

    int do_shutdown() {
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
        // failing the destroy. /sys/bus/vdpa/devices/<name> stands in for the
        // kernel-side vdev but does not bracket it exactly: a removal that wedges
        // part way leaves the entry gone while the vdev is still bound, and
        // DESTROY_DEV answers EBUSY anyway -- measured. That is one reason the
        // EBUSY on the ioctl below is handled rather than left as unreachable.
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
                // Serving is already stopped and cannot be put back. This ioctl
                // answers EBUSY while a daemon is connected, so reaching it at all
                // required stop_serving above to close dev_fd, join the loops and
                // clear the mappings -- "keep the resources needed to serve" is
                // not an option the ABI offers here. Leaving `started` set on the
                // way out described a device that was serving nothing, held no
                // tombstone (the DEFER releases it on this path) and answered
                // EALREADY to a start() that should have been free to adopt the
                // registration and try again. What is left instead is exactly
                // detach()'s state: not started, not ours to destroy without
                // re-claiming, still `registered` so a later shutdown() re-claims
                // the tombstone, probes the char device and retries the destroy.
                // Every other path through this object already understands that
                // state, and the destructor deliberately does not retry -- for the
                // same reason it does not destroy a registration detach() handed
                // over for adoption.
                started = false;
                created = false;
                if (errno == EBUSY)
                    LOG_ERROR_RETURN(EBUSY, -1,
                        "vduse DESTROY_DEV on ` answered busy with no consumer listed; serving stopped, retry later",
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
        // The record follows the registration rather than lagging it: it is what a
        // later adopter compares against, and a stale one makes the next start()
        // refuse the very size this call just published. resize() only grows, so a
        // record that fails to be written holds less than the truth and the
        // refusal it costs later is the safe direction -- which is why this warns
        // from inside record_capacity() and resize() still returns 0.
        record_capacity();
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
        // `created` alone, and not `created && registered`. create_dev() sets the
        // first and the open that follows it sets the second, so an open failing
        // between them leaves a registration this start brought into existence
        // with no marker saying so -- and the old gate skipped the destroy, then
        // cleared both markers, which is a leak the object has forgotten. We are
        // disconnected by the close above, so our own connection cannot be what
        // makes DESTROY_DEV answer EBUSY: something else holds the device, and it
        // is theirs to keep.
        if (created && ctrl_fd >= 0) {
            char nm[VDUSE_NAME_MAX];
            snprintf(nm, sizeof(nm), "%s", name);
            if (::ioctl(ctrl_fd, VDUSE_DESTROY_DEV, nm) < 0) {
                if (errno == EBUSY)
                    LOG_WARN("vduse rollback left ` registered: another daemon or consumer holds it", name);
                else if (errno != EINVAL)   // EINVAL: the registration is already gone
                    LOG_WARN("vduse rollback DESTROY_DEV failed, name `, ", name, ERRNO());
            }
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
    char lock_dir[SCOPE_DIR_BUF] = {};   // the controller's, copied at construction

    explicit VduseControllerImpl(const char* ld) {
        // the factory required it and bounded it, so this cannot truncate
        snprintf(lock_dir, sizeof(lock_dir), "%s", ld);
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
    // foreign daemon that takes no tombstone.
    //
    // size is the tombstone's payload, and the tombstone is the only source there
    // is: the uapi has no config readback, so a registration's capacity cannot be
    // recovered from the registration itself. A tombstone carrying no record --
    // one a start() left behind when it failed after claiming the name -- reports
    // 0, which reads as "not recorded" and never as a capacity. That value is what
    // a recovery loop feeds back into its cfg, and feeding it back is what makes
    // the loop work: start() refuses an adoption whose recorded capacity
    // disagrees with the config, so a caller that invented a size instead would
    // strand every orphan it could otherwise have recovered.
    //
    // Features stay best-effort: DEV_GET_FEATURES only reflects a still-attached
    // consumer's negotiation.
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
                continue;   // locked, or no tombstone this call could use: either
                            // way it is not ours to list
            char path[VDUSE_NAME_MAX + 16];
            snprintf(path, sizeof(path), "/dev/vduse/%s", e->d_name);
            int fd = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;   // EBUSY: a live daemon; ENOENT/EPERM: raced away
            BlkDevInfo bi;
            bi.identity = e->d_name;
            bi.sector_size_shift = 9;
            uint64_t rec = 0;
            if (devlock_read_payload(lock_dir, ln, &rec, sizeof(rec)) == (ssize_t)sizeof(rec))
                bi.size = rec;   // left at the descriptor's 0 when there is no record
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

    // Remove one orphan: the registration (DESTROY_DEV), then the tombstone.
    //
    // The name is CALLER-SUPPLIED and both the registration path and the tombstone
    // name take it VERBATIM -- there is no character mapping here, unlike tcmu's
    // sanitize() -- so the two checks validate() runs on a name are run again on this
    // one. They are duplicated rather than shared because validate() takes a
    // BlkConfig and goes on to require a geometry, while a record from
    // list_orphans() may carry size 0 -- what a tombstone with no capacity record
    // reports -- so requiring a geometry would make exactly those orphans
    // undestroyable. Nothing here needs one either: the registration is destroyed
    // by name.
    //
    // Registration first, tombstone second, so a failure leaves something a later
    // scan still reports.
    int destroy_orphan(const BlkDevInfo& orphan) override {
        const std::string& id = orphan.identity;
        if (id.empty() || id.size() >= VDUSE_NAME_MAX)
            LOG_ERROR_RETURN(EINVAL, -1, "vduse destroy_orphan needs an identity of 1..255 chars (the device name)");
        if (id.find('/') != std::string::npos)
            LOG_ERROR_RETURN(EINVAL, -1, "vduse identity must not contain '/'");
        // Both resolve: /dev/vduse/. is /dev/vduse and /dev/vduse/.. is /dev, so the
        // existence probe below would succeed against a directory that is no device.
        if (id == "." || id == "..")
            LOG_ERROR_RETURN(EINVAL, -1, "vduse identity ` is a path component, not a device name", id);
        char name[VDUSE_NAME_MAX];
        snprintf(name, sizeof(name), "%s", id.c_str());
        char ln[VDUSE_LOCK_BUF];
        vduse_lock_name(name, ln, sizeof(ln));
        char path[VDUSE_NAME_MAX + 16];
        snprintf(path, sizeof(path), "/dev/vduse/%s", name);

        if (::access(path, F_OK) != 0) {
            // No registration, and devlock_free()'s three answers still have to be
            // told apart. One this call cannot use -- absent, or there and not
            // openable by this caller, which devlock_free does not tell apart -- is
            // reported as no such device, and nothing is removed. A tombstone nobody
            // holds is our own litter and goes with the device it outlived. One whose
            // flock attempt failed -- normally a LIVE SERVER's hold -- is not ours to
            // take: that server is between claiming the name and CREATE_DEV, and
            // removing the file would leave the device it then creates with no
            // tombstone, which list_orphans() skips -- unrecoverable by every later
            // scan.
            int lf = devlock_free(lock_dir, ln);
            if (lf < 0)
                LOG_ERROR_RETURN(ENOENT, -1, "no vduse device ` and no tombstone for it that this call could use", name);
            if (lf == 0)
                LOG_ERROR_RETURN(EBUSY, -1, "no vduse device ` yet, but its tombstone is locked, normally by a live server", name);
            if (devlock_unlink(lock_dir, ln) < 0)
                return -1;   // devlock_unlink logged it
            return 0;
        }
        // CLAIM rather than probe, and hold it across the DESTROY_DEV, so that no
        // other server of this implementation adopts the device in between.
        int lock_fd = -1;
        if (devlock_acquire(lock_dir, ln, &lock_fd) < 0) {
            if (errno == EBUSY)
                LOG_ERROR_RETURN(EBUSY, -1, "vduse device ` is held by another live server; leaving it alone", name);
            return -1;   // devlock_acquire logged it
        }
        // Unwinds before nothing else here -- it is the last DEFER -- so the claim is
        // dropped on every exit path, including the refusals below. A controller
        // stores no fd, so keeping it after a failure would be a leak, and a leaked
        // claim is not a lost descriptor but an unreportable device: devlock_free()
        // reads 0 and list_orphans() skips the entry. devlock_release() calls flock()
        // and close(), either of which can set errno, hence the guard.
        DEFER({ int e = errno; devlock_release(lock_fd); errno = e; });
        // A FOREIGN daemon takes no tombstone of ours, so the claim says nothing
        // about it. The char device admits one opener, which is what lets a probing
        // open answer -- list_orphans() and shutdown() both rely on that.
        int probe = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (probe < 0) {
            if (errno == EBUSY)
                LOG_ERROR_RETURN(EBUSY, -1, "vduse device ` has a daemon connected; leaving it alone", name);
            LOG_ERRNO_RETURN(0, -1, "failed to probe the vduse char device ", path);
        }
        ::close(probe);   // the destroy only needs no daemon connected: true once closed
        // The contractual EBUSY, checked BEFORE destroying rather than left to the
        // ioctl. shutdown() checks it in the same place for the same reason --
        // tearing down first would leave the consumer's device unserved and still
        // fail the destroy. /sys/bus/vdpa/devices/<name> is only a stand-in for the
        // kernel-side vdev and does not bracket it exactly: a removal that wedges
        // part way leaves the entry gone while the vdev is still bound, and measured
        // in that state this check passed, nothing held the char device, and
        // DESTROY_DEV answered EBUSY anyway. So the EBUSY branch on the ioctl below
        // is what catches that, and is not only a guard against a consumer attaching
        // in between.
        char sp[VDUSE_NAME_MAX + 40];
        snprintf(sp, sizeof(sp), "/sys/bus/vdpa/devices/%s", name);
        if (::access(sp, F_OK) == 0)
            LOG_ERROR_RETURN(EBUSY, -1, "vduse device ` is still attached to a vdpa consumer (vdpa dev del it first)", name);
        // Duplicated from VduseDeviceImpl::ctrl_init() rather than shared with it:
        // that one caches into a device member and assigns it BEFORE the handshake,
        // so lifting it would change what a retry after a failed handshake does --
        // device-lifecycle behavior this call has no reason to touch.
        int ctrl = ::open("/dev/vduse/control", O_RDWR | O_CLOEXEC);
        if (ctrl < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to open /dev/vduse/control (vduse module loaded?)");
        DEFER(::close(ctrl));
        uint64_t ver = 0;
        if (::ioctl(ctrl, VDUSE_GET_API_VERSION, &ver) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse GET_API_VERSION failed");
        ver = VDUSE_API_VERSION;
        if (::ioctl(ctrl, VDUSE_SET_API_VERSION, &ver) < 0)
            LOG_ERRNO_RETURN(0, -1, "vduse SET_API_VERSION(0) failed (kernel supports `)", ver);
        if (::ioctl(ctrl, VDUSE_DESTROY_DEV, name) < 0) {
            if (errno == EBUSY)
                LOG_ERROR_RETURN(EBUSY, -1, "vduse device ` is still attached to a vdpa consumer (vdpa dev del it first)", name);
            if (errno != EINVAL)   // EINVAL: the registration is already gone
                LOG_ERRNO_RETURN(0, -1, "vduse DESTROY_DEV failed, name `", name);
        }
        // Unlinked while the claim is still held -- the release is deferred to scope
        // exit, after this statement -- so what goes is provably the tombstone this
        // call opened, not one another daemon created in between.
        if (devlock_unlink(lock_dir, ln) < 0)
            return -1;   // devlock_unlink logged it
        return 0;
    }
};

VduseController* new_vduse_controller(const char* lock_dir) {
    if (validate_scope_dir(lock_dir, "lock") < 0)
        return nullptr;   // already logged
    return new VduseControllerImpl(lock_dir);
}

// ---------------------------------------------------------------------------
// Observation seam over the iotlb cache's mapping accounting.
//
// Iotlb is declared in this file and appears in no header, so no other translation
// unit could read how much mapping the cache holds -- which is why the retained
// mapping budget vq_tick enforces (VDUSE_STALE_FLUSH_BYTES) had nothing a test
// could observe it with. These three close that without moving the type: they are
// read-only, they are declared nowhere but in the suite that calls them, and no
// header gains a declaration, a type or a field.
//
// UINT64_MAX, and not 0, for a handle that is not a vduse device: 0 is the answer
// that reads as "nothing retained", so a wrong-typed handle has to contradict the
// assertion it was passed to rather than agree with it.
//
// The two byte counters take the cache's photon::mutex, so they are for a caller on
// the vcpu -- which is where the cache's own readers are.
// ---------------------------------------------------------------------------
uint64_t vduse_iotlb_live_bytes(IBlkDevice* dev) {
    auto* impl = dynamic_cast<VduseDeviceImpl*>(dev);
    return impl ? impl->iotlb.live_bytes() : UINT64_MAX;
}

uint64_t vduse_iotlb_stale_bytes(IBlkDevice* dev) {
    auto* impl = dynamic_cast<VduseDeviceImpl*>(dev);
    return impl ? impl->iotlb.stale_bytes() : UINT64_MAX;
}

uint64_t vduse_iotlb_flush_budget() {
    return VDUSE_STALE_FLUSH_BYTES;
}

}  // namespace blk
}  // namespace photon
