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

#pragma once

// Internal utilities shared by the blk transports. NOT part of the public blk
// API (no include/photon symlink); the transports include this as "utils.h".
// Three sections:
//
//   1. leaf helpers    -- info validation, the WRITE_ZEROES fallback, the
//                         per-device flock, unix endpoint probing. Portable:
//                         no photon runtime state and no transport knowledge.
//   2. generic netlink -- a minimal genetlink client (nbd's loopback attach,
//                         tcmu's device notifications).
//   3. virtio-blk core -- the wire constants and structs, the split-ring
//                         descriptor-chain walk, and the VirtQueueServer
//                         serving engine shared by the vduse and vhost-user
//                         transports.
//
// Everything is implemented in utils.cpp, which CMake compiles on EVERY
// platform (module blk_utils): section 1 because all-platform nbd.cpp calls it,
// sections 2 and 3 inside #ifdef __linux__ because they are Linux ABI. Their
// DECLARATIONS are portable, so an includer needs no #ifdef around the include
// -- it just must not call sections 2/3 off Linux (nbd.cpp keeps its netlink
// use under __linux__).
//
// Byte order: netlink integers are host byte order; the virtio wire structs are
// little-endian (VIRTIO_F_VERSION_1 is always offered) and assume an LE host,
// the same contract as the rest of blk/.

#include "blk.h"

#include <photon/common/callback.h>     // Delegate / TempDelegate
#include <photon/fs/filesystem.h>       // fs::IFile
#include <photon/thread/workerpool.h>   // WorkPool

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace photon {
namespace blk {

// ===========================================================================
// 1. leaf helpers (all platforms; implemented in utils.cpp)
// ===========================================================================

// ----------------------------------------------------------------------------
// Serving-coroutine stack size
// ----------------------------------------------------------------------------

// photon's DEFAULT_STACK_SIZE is 8 MiB. The per-request coroutines here use a
// few KiB of it: parse, one preadv/pwritev into the backend, complete. What 8
// MiB actually costs is not RSS -- the stacks are posix_memalign'd, so only
// touched pages become resident -- but ADDRESS SPACE and one VMA per stack,
// and how many there are is the PEER's choice: a virtqueue may hold 32768
// entries, and nbd takes one coroutine per connection. At 8 MiB that maps 256
// GiB and runs vm.max_map_count (65530 by default) out long before anything
// else gives. BlkConfig::stack_size overrides this for a backend IFile that
// recurses deeply or keeps large buffers on its own stack.
static constexpr uint32_t DEFAULT_REQ_STACK = 256 * 1024;

// Resolve BlkConfig::stack_size: 0 means the default above, NOT photon's.
inline uint32_t resolve_stack_size(uint32_t configured) {
    return configured ? configured : DEFAULT_REQ_STACK;
}

// ----------------------------------------------------------------------------
// BlkDevInfo validation (the part of every start() that is transport-neutral)
// ----------------------------------------------------------------------------

// `virtio` adds the capacity-unit rule: virtio counts capacity in 512-byte
// sectors whatever the negotiated blk_size is. Returns 0, or -1 with
// errno=EINVAL after logging.
int validate_info(const BlkDevInfo& info, bool virtio);

// ----------------------------------------------------------------------------
// WRITE_ZEROES on a backend without hole-punch
// ----------------------------------------------------------------------------

// zero_range, falling back to chunked zero writes when the backend's fallocate
// lacks ZERO_RANGE (e.g. tmpfs). Returns 0, or -1 with errno after logging.
// Linux-only, because IFile::zero_range itself is (fs/virtual-file.cpp) -- and
// so are the callers (tcmu, ublk).
int zero_fill(fs::IFile* backend, uint64_t off, uint64_t len);

// Take the exclusive lock for `name` inside the lock dir (nullptr/"" =
// /run/photon-blk), creating the dir and the file as needed. *fd_out receives the
// fd to keep for the lifetime of the serving session.
// EBUSY is the ROUTINE "another live server holds this identity" answer and is
// deliberately not logged here: every caller reports it with its own context
// (the device identity, or a silent skip in the passive handler's scan).
int devlock_acquire(const char* dir, const char* name, int* fd_out);

// Release (and close) a lock fd. The FILE stays: it is the tombstone the orphan
// scans look for, and a later daemon re-locks it by name.
void devlock_release(int fd);

// Non-creating probe for the orphan scans: 1 = free (no live server holds it),
// 0 = held, -1 = missing or unopenable (an inconsistent state -- the caller
// skips the device). Read-only and no O_CREAT: a query must not create files.
// Note that a DIRECTORY in place of the file reads as 1: open(O_RDONLY) and
// flock both succeed on a directory fd. It is devlock_acquire that refuses one,
// because O_CREAT|O_RDWR on a directory is EISDIR.
int devlock_free(const char* dir, const char* name);

// The inverse of devlock_release's "the FILE stays": remove the tombstone once
// the registration it stands for is gone too. 0 once nothing is there -- an
// absent file is the goal state, not an error -- or -1 with errno after logging.
// unlink(), never remove(): a DIRECTORY where a tombstone should be is operator
// state, and unlink reports it (EISDIR) rather than recursing into it or
// deleting it on the operator's behalf. A caller removing a registration
// alongside the tombstone therefore has to decide what a refused tombstone
// means for the registration it already took down.
int devlock_unlink(const char* dir, const char* name);

// The bound a controller puts on its scope directory (the lock dir; the socket
// dir for vhost-user) so that what it STORES is what it USES: devlock_acquire and
// devlock_free snprintf into a PATH_MAX path, so an over-long dir would be
// silently truncated into a DIFFERENT directory -- the orphan scan and the
// device's claim would then disagree, which is exactly the divergence a controller
// exists to make impossible. This checks the LENGTH only: nullptr/"" is left to
// each factory, and means DEVLOCK_DIR for the three lock dirs (devlock_* normalizes
// it) but is rejected outright for vhost-user's socket dir, which has no default.
constexpr size_t SCOPE_DIR_BUF = 256;

// Upper bound on the queues one device will drive. Shared by ublk (which always
// had it), vhost-user and vduse (which became multiqueue): all three publish a
// queue count the peer then indexes with, so the bound has to be the same one --
// a per-transport limit would make GET_QUEUE_NUM, virtio_blk_config::num_queues
// and VDUSE_CREATE_DEV's vq_num disagree across transports for the same
// BlkConfig::queues.
static constexpr uint32_t MAX_QUEUES = 64;

// 0 if `dir` is usable, -1 with errno=ENAMETOOLONG after logging if not.
int validate_scope_dir(const char* dir);

// ----------------------------------------------------------------------------
// unix socket endpoint probing
// ----------------------------------------------------------------------------

// Is a LIVE listener behind this unix socket path? 1 = live, 0 = stale or
// absent, -1 = a local error (errno set). blk.h's start() contract needs the
// distinction: a live endpoint is EBUSY (never steal another backend's socket
// path), a stale one may be unlinked and re-bound. Runs on the photon vcpu
// (nonblocking connect + fd wait).
int unix_listener_live(const char* path);

// ----------------------------------------------------------------------------
// blocking syscalls this process's own coroutines must answer
// ----------------------------------------------------------------------------

// Some syscalls block the caller while the kernel runs work that THIS process's
// photon coroutines have to answer (a configfs write that triggers a synchronous
// SCSI scan, say). On the photon vcpu that deadlocks until the kernel times out.
// Run such a syscall on a worker OS thread and yield the vcpu until it finishes;
// the worker's errno is propagated back so the caller's LOG_ERRNO_RETURN reports
// it. TempDelegate is safe here: the worker is joined before returning.
int run_off_vcpu(TempDelegate<int> fn);

// ----------------------------------------------------------------------------
// handing serving coroutines to a caller-supplied photon::WorkPool
// ----------------------------------------------------------------------------

// BlkConfig::pool is the single source of serving vcpus. Both helpers below are
// no-ops on a null pool and on a pool with no vcpus, which is what keeps "serve on
// the caller's own vcpu" -- the behaviour of every caller before this existed --
// the default rather than a special case.

// Move a freshly created, still-READY serving coroutine into `pool`. Call it
// IMMEDIATELY after thread_create with no yield in between: photon::thread_migrate
// rejects a thread that is not READY or that already left this vcpu (EINVAL, and
// it logs, so the mistake is not silent).
//
// The out-of-range index is deliberate. It makes WorkPool fall through to its own
// `vcpu_index++ % size` cursor, which is pool-wide and shared by every caller --
// so several devices on one pool interleave across the vcpus instead of each
// starting at vcpu 0 and colliding there.
//
// Failure to migrate is a WARNING, not an error: a serving coroutine works
// correctly on any vcpu, so a failed migration only loses fan-out.
void migrate_to_pool(photon::WorkPool* pool, photon::thread* th);

// Refuse a pool whose vcpus cannot host blk's serving coroutines, BEFORE anything
// is parked on them. Returns 0, or -1 with errno=EINVAL after logging the
// offending vcpu index.
//
// Two requirements, derived from the CALLER's own vcpu rather than declared in the
// config -- a serving coroutine does to the backend exactly what the caller's
// coroutines would do, so "at least as capable as the vcpu you are calling from"
// is the whole requirement and the caller never has to state it:
//
//   event engine: must be installed, and must be the SAME one. "Installed" is not
//     optional -- WorkPool's constructor defaults to ev_engine = 0 and
//     INIT_EVENT_NONE is 0, so the natural `WorkPool pool(4);` yields vcpus whose
//     master engine is the NullEventEngine, whose wait_for_fd returns -1 WITHOUT
//     setting errno. Every fd wait on such a vcpu then fails at once and throws
//     its timeout away -- including one issued on an explicitly created cascading
//     engine, because every cascading engine in photon blocks by calling back
//     into the master engine. That much is a property of photon and is the same
//     for all five transports.
//
//     What a transport then DOES with a failed wait is its own property, and the
//     five are not alike; each was measured with this guard deleted. vhost-user's
//     virtqueue loop spins and logs on every pass: one run wrote 8.07 GB over
//     42.9 M lines before it filled the filesystem it was logging to, and the
//     frontend's write never completed. ublk breaks too, but quietly -- its
//     per-queue pump cannot reap, so no request is ever fetched, the initiator's
//     write times out, shutdown() returns -1, and the teardown that follows it
//     never completes: the queue-teardown coroutine migrated onto the pool vcpu is
//     starved by a pump that no longer yields. nbd neither spins nor logs:
//     start() returns 0, the endpoint listens, and only the client handshake
//     fails. tcmu still WORKS -- a write, an fsync and a read-back through its
//     LUN all succeed -- and only stops being cheap: at PollPolicy::SLEEP its
//     pump logs and sleeps 1 ms per pass, near 1 kHz, while at PollPolicy::SPIN
//     it yields instead, logs nothing, and holds a core. vduse with no consumer
//     attached logs nothing either, because its loop's readiness gate is false
//     and so it never reaches the wait.
//
//     Since the engine never writes errno, every gate on this path that tests one
//     is reading a value left over from an unrelated syscall: the shared
//     virtqueue loop's, each pump's, and the cascading engine's own decision to
//     report a failed wait on the master engine as no events rather than as an
//     error whenever the stale errno happens to be ETIMEDOUT. Every cascading
//     engine in photon reports that way -- io_uring is only the one blk uses --
//     and that report is what turns ublk's throttled loud poll into an
//     unthrottled silent one.
//
//     Which value a gate happens to read differs between transports and between
//     runs of the same transport; as one illustration, a pass through the shared
//     virtqueue loop's gate in the vhost-user run above read 11. One of those
//     gates read -1, which is not an errno at all. So whether a given failure is
//     loud, silent or throttled is an accident of what errno happened to hold, not
//     a property of the pool, and no errno printed by those messages is an
//     observation.
//
//     "Same" is not optional either -- a backend file opened with the iouring
//     engine casts the CURRENT vcpu's master engine to iouringEngine*, so landing
//     on an epoll vcpu is a wrong-type cast. Note that asking init() for several
//     event engines does not install several: it keeps the first that works, which
//     is why this compares get_event_engine() (the winner) and not the request mask.
//   io engines: the pool vcpu's mask must COVER the caller's. libaio's context is
//     thread-local and is only set by libaio_wrapper_init(), which init() calls
//     only when the flag is present; on a vcpu without it the context is null and
//     the first libaio-backed IO dereferences null.
int check_pool_engines(photon::WorkPool* pool);

// ===========================================================================
// 2. minimal generic-netlink client (Linux; see utils.cpp)
//
// Netlink integers are host byte order (no bswap here). The kernel replies to
// EVERY command: NLMSG_ERROR carries -errno (0 = ack), and success replies
// carry genetlink attributes (e.g. NBD_ATTR_INDEX after CONNECT).
// ===========================================================================

// append one attribute at buf+off; returns the PADDED LENGTH of the attr
// just written (chain with `off += nla_append(...)`), 0 on overflow
size_t nla_append(char* buf, size_t off, size_t cap, uint16_t type,
                  const void* data, size_t len);
size_t nla_append_u32(char* buf, size_t off, size_t cap, uint16_t type, uint32_t v);
size_t nla_append_u64(char* buf, size_t off, size_t cap, uint16_t type, uint64_t v);
size_t nla_append_str(char* buf, size_t off, size_t cap, uint16_t type, const char* s);

// find a top-level attribute; returns payload and sets *len
const void* nla_find(const char* attrs, size_t len, uint16_t type, size_t* plen);

// iterate top-level attrs; fn(type, payload, len) -> false stops the walk
void nla_for_each(const char* attrs, size_t len,
                  TempDelegate<bool, uint16_t, const void*, size_t> fn);

struct GenlSock {
    int sk = -1;
    uint32_t seq = 0;

    GenlSock();
    ~GenlSock();

    // one request -> one relevant reply; sends attrs (which start right after
    // the genlmsghdr), waits for the reply matching this socket's sequence
    // number, and copies the family reply's attribute region (the bytes after
    // the genlmsghdr) into out/cap. Returns the attr length (>0) for a family
    // reply, 0 for a bare ack (NLMSG_ERROR error==0) or NLMSG_DONE, or -errno.
    ssize_t transact(uint16_t family, uint8_t cmd, const char* attrs, size_t attrs_len,
                     char* out, size_t cap);

    // one-shot query on top of transact: extracts the wanted u32 attr from the
    // reply; a bare ack / no-attr-wanted returns 0 with want_out untouched.
    int request(uint16_t family, uint8_t cmd, const char* attrs, size_t attrs_len,
                uint16_t want_type, uint32_t* want_out);

    int resolve_family(const char* name);

    // join a multicast group (a resolve_mcast_group id) so async notifications
    // (tcmu ADDED/REMOVED/RECONFIG) are delivered to this socket
    int subscribe(uint32_t group_id);

    // resolve a multicast group id by family + group name
    int resolve_mcast_group(const char* family_name, const char* group_name);

    // enlarge the receive buffer and ask the kernel not to drop on overrun, so
    // a burst of multicast notifications is not lost (the overlaybd idiom)
    int tune_for_notifications();

    // drain pending datagrams (nonblocking; the socket is O_NONBLOCK from the
    // ctor) and dispatch each genetlink message as cb(family, cmd, attrs,
    // attrs_len); NLMSG_ERROR/NLMSG_DONE are not notifications. Returns the
    // count dispatched, or -errno on a recv failure.
    int recv_notifications(TempDelegate<void, uint16_t, uint8_t, const char*, size_t> cb);
};

// ===========================================================================
// 3. virtio-blk device-model core (Linux; see utils.cpp)
//
// Covers the virtio/vring wire constants and structs (copied verbatim from the
// kernel headers, as every transport here does), the split-ring descriptor
// chain walk + request dispatch against a photon IFile, the ring-access
// helpers and the VirtQueueServer serving engine.
//
// What the transports keep: registration/negotiation, ring setup, address
// translation (injected as a delegate: vduse resolves IOVAs via the VDUSE
// IOTLB, vhost-user translates guest GPAs via its memfd table) and completion
// notification (VDUSE_VQ_INJECT_IRQ vs the call eventfd).
//
// One obligation every consumer of this core shares: the integers inside a
// descriptor or a ring entry were written by the peer or the guest, so any
// one of them that bounds a memory access, an iovec, an array index or a
// shift has to be range-checked before it is used. What is checked today,
// each line naming a symbol you can grep for:
//
//   head, de->next  descriptor indices. Both are peer-written and both are
//                   tested against ring_num before desc[] is indexed.
//                   MAX_DESC_CHAIN bounds how many steps the walk takes, not
//                   where they land, so it is not this guard; chain_end is
//                   what refuses a chain that never terminates -- longer
//                   than MAX_DESC_CHAIN, or circular.
//   de->addr,       an address goes to the translate delegate together with
//   de->len         its length, and translate must fail unless the whole
//                   [addr, addr + len) is mapped with no wrap in the sum.
//                   A zero len asks for one byte, so a zero-length desc
//                   gets that answer instead of a vacuous success.
//   sizeof(*hdr)    the first device-readable desc must be large enough to
//                   hold a virtio_blk_outhdr before it is cast to one.
//   ndata           the payload iovec array is MAX_DESC_CHAIN deep and the
//                   count is tested before every store into it.
//   VRING_DESC_F_INDIRECT
//                   refused outright: an indirect table carries a second
//                   peer-supplied length, and this core does not walk it.
//   hdr->sector,    the sector bound is compared in SECTORS, because sector
//   want            is a full 64 bits and (sector << 9) can wrap back into
//                   the range it was just tested against; only once that
//                   holds is the sector's byte offset subtracted from
//                   capacity, and the byte sum compared against the
//                   remainder.
//   hdr->type       dispatched by a switch whose default is UNSUPP, so an
//                   unknown type never reaches a length it would consume.
//   avail->idx      a free-running peer-written counter. dispatch_avail caps
//                   itself at in_flight >= num and reaches the avail ring
//                   through last_avail % num, so one kick cannot fan out
//                   more coroutines than the ring is deep.
//   num             the modulo divisor for both rings, the in-flight cap,
//                   and the index of the event slot one element past the end
//                   of each ring -- which is why the transports size the
//                   avail region as 3 + num uint16s and the used region
//                   as 3 uint16s plus num vring_used_elems. Not re-validated
//                   here: the transport publishes it, and both reject the
//                   0 that would stall the queue silently: this core's cap
//                   reads 0 >= 0, and dispatch returns before any modulo.
//   capacity        ours, not the peer's, but load-bearing for the sector
//                   check above: it is always a multiple of 512, which is
//                   what keeps that subtraction from underflowing.
// ===========================================================================

// ----------------------------------------------------------------------------
// virtio constants (verbatim from <linux/virtio_config.h>, <linux/virtio_blk.h>)
// ----------------------------------------------------------------------------

#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#define VIRTIO_CONFIG_S_DRIVER      2
#define VIRTIO_CONFIG_S_DRIVER_OK   4
#define VIRTIO_CONFIG_S_FEATURES_OK 8
#define VIRTIO_CONFIG_S_FAILED      0x80

#define VIRTIO_F_VERSION_1       32
#define VIRTIO_F_ACCESS_PLATFORM 33   // mandatory for VDUSE; not offered for vhost-user

#define VIRTIO_ID_BLOCK 0x02

#define VIRTIO_BLK_F_RO       5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH    9

// virtio 1.2 §5.2.3. Offering it commits us to publishing a truthful
// virtio_blk_config::num_queues (§5.2.4: that field is only valid when this bit is
// set), and to honoring the queue index the peer puts in every vring message --
// which is exactly what the two virtio transports did not do before.
#define VIRTIO_BLK_F_MQ      12

#define VIRTIO_BLK_T_IN     0
#define VIRTIO_BLK_T_OUT    1
#define VIRTIO_BLK_T_FLUSH  4
#define VIRTIO_BLK_T_GET_ID 8

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

// ----------------------------------------------------------------------------
// split vring (verbatim from <linux/virtio_ring.h>)
// ----------------------------------------------------------------------------

struct vring_desc {
    uint64_t addr;    // guest/IOVA address -- the transport translates it
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2
#define VRING_DESC_F_INDIRECT 4

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};
#define VRING_AVAIL_F_NO_INTERRUPT 1

struct vring_used_elem {
    uint32_t id;     // descriptor chain head
    uint32_t len;    // bytes written into device-writable buffers
};
struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
};
#define VRING_USED_F_NO_NOTIFY 1

// virtio 1.2 §2.7.7.2 / §2.7.10.1: with this negotiated, both sides ignore the
// low bit of their ring's flags and suppress notifications by index instead --
// the driver publishes used_event at the END of the avail ring, we publish
// avail_event at the END of the used ring (<linux/virtio_ring.h>:82-85, :193-194).
#define VIRTIO_RING_F_EVENT_IDX 29

// ----------------------------------------------------------------------------
// virtio-blk wire structs (verbatim from <linux/virtio_blk.h>)
// ----------------------------------------------------------------------------

// packed because the kernel's is, and complete because the kernel's is: wce and
// unused sit between topology and num_queues, so omitting them shifts every
// field after topology two bytes early (num_queues at 32 instead of 34) and
// leaves sizeof at 64 instead of 60. Both are pinned below against literal
// offsets -- a struct copied from a header proves nothing on its own, which is
// how the vhost-user message header stayed wrong while its tests were green.
struct __attribute__((packed)) virtio_blk_config {
    uint64_t capacity;      // in 512-byte sectors, regardless of blk_size
    uint32_t size_max;
    uint32_t seg_max;
    struct { uint16_t cylinders; uint8_t heads; uint8_t sectors; } geometry;
    uint32_t blk_size;
    struct { uint8_t physical_block_exp; uint8_t alignment_offset;
             uint16_t min_io_size; uint32_t opt_io_size; } topology;
    uint8_t wce;            // write-back mode; read only if F_CONFIG_WCE is offered
    uint8_t unused;
    uint16_t num_queues;    // read only if F_MQ is offered
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t  write_zeroes_may_unmap;
    uint8_t  unused1[3];
};
static_assert(offsetof(virtio_blk_config, blk_size) == 20, "virtio_blk_config geometry must be 4 bytes");
static_assert(offsetof(virtio_blk_config, topology) == 24, "virtio_blk_config topology offset");
static_assert(offsetof(virtio_blk_config, wce) == 32, "virtio_blk_config wce offset");
static_assert(offsetof(virtio_blk_config, num_queues) == 34, "virtio_blk_config num_queues offset");
static_assert(offsetof(virtio_blk_config, max_discard_sectors) == 36, "virtio_blk_config discard offset");
static_assert(offsetof(virtio_blk_config, write_zeroes_may_unmap) == 56, "virtio_blk_config write-zeroes offset");
static_assert(sizeof(virtio_blk_config) == 60, "virtio_blk_config size");

struct virtio_blk_outhdr {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;        // in 512-byte units
};

// ----------------------------------------------------------------------------

// translates a descriptor's buffer address (GPA / IOVA -- only the transport
// knows which) to a local VA of `len` bytes; nullptr when unmappable. Used
// synchronously within virtio_blk_serve_chain.
using VirtioBlkTranslate = TempDelegate<void*, uint64_t, size_t>;

// Walk the descriptor chain starting at `head`, dispatch the virtio-blk
// request to `backend`, and write the chain's status byte. Returns the virtio
// status (VIRTIO_BLK_S_*); *written receives the total bytes filled into
// device-writable buffers (request data + the status byte) -- the value the
// used-ring element's len wants. `tag` prefixes the error logs (the device
// identity); `serial` answers VIRTIO_BLK_T_GET_ID.
//
// `ring_num` is the ring's descriptor count. `head` and every `next` the chain
// visits are guest-written, so each is checked against it: without that a chain
// can index arbitrarily far past the mapped ring.
//
// `capacity` is the backend's size in bytes (a multiple of 512 -- validate_info
// with virtio=true enforces that). A READ or WRITE whose LBA range passes it is
// refused: a short read is harmless, but a write past EOF on a regular-file
// backend EXTENDS it, so an unbounded guest could grow the image at will.
//
// Chain layout (virtio 1.0, no indirect -- callers should not offer
// VRING_F_INDIRECT_DESC): first device-readable desc = virtio_blk_outhdr;
// further readable descs = the WRITE payload; writable descs = the READ
// destination(s); the last desc, if writable and 1 byte, is the status.
uint8_t virtio_blk_serve_chain(fs::IFile* backend, bool read_only,
                               const char* serial, const char* tag,
                               const vring_desc* desc, uint16_t head,
                               uint32_t ring_num, uint64_t capacity,
                               VirtioBlkTranslate translate, uint32_t* written);

// ---- split-ring access helpers (the barrier discipline in one place) ----

uint16_t vring_avail_idx(const vring_avail* avail);   // acquire load
uint16_t vring_used_idx(const vring_used* used);      // acquire load
bool vring_need_irq(const vring_avail* avail);        // !AVAIL_F_NO_INTERRUPT

// append one used element and publish it (release store); returns the new
// used index. `num` is the ring size and MUST be nonzero (it is a modulo
// divisor); it is uint32_t to match VirtQueueServer::num -- a uint16_t here
// silently truncated a frontend-supplied 65536 to 0 and divided by it.
// Yield-free: safe from concurrent coroutines on one vcpu.
uint16_t vring_used_append(vring_used* used, uint16_t used_idx, uint32_t num,
                           uint16_t id, uint32_t len);

// ---- event index (VIRTIO_RING_F_EVENT_IDX), verbatim from
// ---- <linux/virtio_ring.h>:193-194 and :219-227 ----

// used_event: the used index at which the driver wants its next interrupt. It
// lives in the uint16 slot right past the avail ring's `num` entries, which is
// why both transports size the avail region as (3 + num) uint16s.
uint16_t vring_used_event(const vring_avail* avail, uint32_t num);   // acquire load

// avail_event: the avail index at which we want the driver's next kick. Same
// trick on the other ring -- the uint16 right past the used ring's `num`
// elements. Written as a setter rather than the kernel's `uint16_t&` macro so
// the release store lives inside it and no caller can forget it.
void vring_set_avail_event(vring_used* used, uint32_t num, uint16_t v);   // release store

// The suppression predicate. uint16 modular subtraction makes it wrap-safe,
// which is the entire reason it is written in this shape. With new == old + 1 it
// reduces to old == event_idx, i.e. exactly the equality rule of §2.7.7.2 /
// §2.7.10.1; the general form also stays correct when several used elements are
// appended at once (the window widens and anything crossing event_idx + 1
// notifies), which the equality form would miss.
bool vring_need_event(uint16_t event_idx, uint16_t new_idx, uint16_t old);

// ----------------------------------------------------------------------------
// VirtQueueServer: the serving half of one split virtqueue
//
// The kick-driven loop, the dispatch that gives every available chain its own
// coroutine, the used-ring completion and the drain discipline teardown relies
// on -- all of it identical between the vduse and vhost-user transports (a
// teardown bug found in one was found in the other, which is why it lives
// here). What genuinely differs is injected as hooks.
//
// Threading: one serving vcpu PER QUEUE. loop() runs as a photon coroutine and
// the request coroutines it spawns inherit its vcpu, so they interleave with it
// and the used-ring append stays yield-free -- no locking is needed for anything
// only they touch. What IS cross-vcpu is the control plane: BlkConfig::pool lets
// a caller run this queue's loop on a pool vcpu while the transport's message
// loop stays on the caller's, so every field the two sides share is atomic --
// event_idx, capacity and notify_valid (control plane writes, serving side
// reads), run and stopping (teardown writes, loop reads), in_flight (the request
// coroutines bump it, teardown polls it). The ones published as a group
// (desc/avail/used/num, the kick/call fds) and the ones only the serving side
// advances (last_avail, used_idx) cannot be made atomic -- a request coroutine
// holds those pointers across the backend IO, so a lock would have to span the
// whole request, which serializes the queue the split existed to parallelize.
// What covers them is a contract on the transport rather than a type: mutate them
// only where this loop cannot be mid-dispatch, either by quiescing the queue first
// (join the loop, drain the requests it dispatched) or by already being on the
// loop's own vcpu. vhost-user quiesces for its ring messages and hops onto the
// loop's vcpu for teardown, which is also the only place last_avail can safely be
// read while the loop is expected to advance it. vduse's message loop may not
// quiesce at all: the kernel blocks whoever sent a message until it is answered,
// and gives up after msg_timeout seconds, so a handler cannot wait for the
// in-flight requests. What hops onto the loop's vcpu is not the message loop
// alone -- teardown hops too. Between them they send everything that has to reach
// the loop or the requests it dispatched: a vq state read, and teardown's stop,
// drain and backlog wait, the last of which dereferences an avail ring whose pages
// its own flush_stale is what munmaps. The readiness conflict is settled
// with a per-queue generation counter: an invalidation bumps the counter before it
// clears, so a refresh already resolving that ring withholds its own publish.
// loop()'s readiness re-check after its one yield is the backstop on top.
//
// Bound: dispatch_avail() stops at `num` outstanding chains. avail->idx is a
// guest-written free-running counter, so without a cap a single kick could fan
// out 65535 coroutines; `num` is the natural limit because every chain consumes
// at least one descriptor, so a correct driver never reaches it. What the cap
// leaves pending, loop()'s next KICK_FALLBACK_US re-read picks up.
// ----------------------------------------------------------------------------

class VirtQueueServer {
public:
    struct Hooks {
        // a descriptor buffer address (GPA / IOVA -- only the transport knows
        // which) -> a local VA of `len` bytes; nullptr when unmappable
        Delegate<void*, uint64_t, size_t> translate;
        // the used ring advanced and the driver asked for an interrupt
        Delegate<void> notify;
        // may the loop dispatch, and may completions be published, right now?
        // Unbound means "never" (fire() returns false), which is the safe
        // default before the ring is configured.
        Delegate<bool> ready;
        // top of every loop iteration: deferred ring refresh, release of
        // mappings invalidated while requests were in flight, ...
        Delegate<void> tick;
    } hooks;

    // Field order is padding-driven, do not tidy it: every 8-byte member comes
    // first, then the 4-, 2- and 1-byte ones. `read_only` used to sit between
    // `capacity` and `serial`, and since `serial` needs 8-alignment that left a
    // 7-byte hole at offset 81. 152 bytes vs 160.
    fs::IFile* backend = nullptr;
    std::atomic<uint64_t> capacity{0};   // backend size in bytes; bounds every LBA
    const char* serial = "";      // answers VIRTIO_BLK_T_GET_ID
    const char* tag = "";         // device identity; prefixes the logs

    // the ring, set up by the transport once the frontend/driver published it
    vring_desc* desc = nullptr;
    vring_avail* avail = nullptr;
    vring_used* used = nullptr;
    uint32_t num = 0;
    uint32_t stack_size = DEFAULT_REQ_STACK;   // per-request coroutine stack; the
                                               // transport resolves
                                               // BlkConfig::stack_size into it
    int kickfd = -1;              // eventfd the transport registered with its
                                  // kernel/frontend side; owned by the
                                  // transport. MUST be O_NONBLOCK: loop() drains
                                  // it with a bare read() until EAGAIN, so a
                                  // blocking fd would park the whole vcpu there
                                  // until the next kick -- silently defeating
                                  // KICK_FALLBACK_US below, and with it the
                                  // re-dispatch that dispatch_avail's in-flight
                                  // cap relies on. vduse creates its own with
                                  // EFD_NONBLOCK; vhost-user receives the peer's
                                  // over SCM_RIGHTS and hardens it on arrival.
    std::atomic<uint32_t> in_flight{0};
    uint16_t last_avail = 0;
    uint16_t used_idx = 0;
    bool read_only = false;
    // VIRTIO_RING_F_EVENT_IDX state, per queue. Set by the transport from the
    // NEGOTIATED features, never from what we offered: if the peer masks bit 29
    // off we must fall back to the flags semantics, and deciding from our own
    // offer would have us read a used_event nobody ever wrote (zeroed at setup),
    // which suppresses nearly every interrupt. Atomic: the transport re-derives
    // it from the control plane while should_notify/publish_avail_event read it
    // from the serving side, and those are not necessarily one vcpu.
    std::atomic<bool> event_idx{false};
    // False until the first notification decision on this ring. See
    // should_notify(): it makes the first completion notify unconditionally,
    // which is what a resumed ring needs and §2.7.7.1 explicitly permits.
    // Atomic for the same reason as event_idx next to it: the transport's reset
    // paths clear it from the control plane while should_notify sets it from the
    // serving side. It does not make that pair a well-defined handover -- only
    // the individual accesses.
    std::atomic<bool> notify_valid{false};

    // Both written by teardown and read by loop()/handle_req, which the split
    // described above can put on different vcpus. Atomic only against tearing:
    // it does not wake the loop, which is what wake() + interrupt + join are for.
    std::atomic<bool> run{false};        // the loop coroutine may live
    std::atomic<bool> stopping{false};   // teardown: stop dispatching and leave the
                                         // in-flight requests UNCOMPLETED, so the next
                                         // daemon resumes from used->idx and re-serves
                                         // them (virtio-blk ops are idempotent)

    // the kickfd is the primary wakeup; this bounds how long the loop sleeps
    // before re-checking the avail ring anyway. A kick that races the loop is
    // caught by that re-read, so the fallback only costs latency for a
    // genuinely lost notification -- and one was observed (a vduse probe run
    // saw a request appear without the kickfd firing, never fully
    // root-caused), so the serving path must not depend on kick delivery.
    static constexpr uint64_t KICK_FALLBACK_US = 5000;

    void loop();                  // the serving coroutine body
    void dispatch_avail();        // one coroutine per available chain
    void handle_req(uint16_t head);
    void complete_req(uint16_t head, uint32_t written);
    void drain();                 // wait until in_flight reaches 0
    void wake();                  // nudge loop() out of its kickfd wait

    // Decide whether the used element just appended warrants a notification.
    // `old_used_idx` is the used index BEFORE the append -- §2.7.7.2's "the idx
    // field in the used ring which determined where that descriptor index was
    // placed". The NEW index is not a parameter: `used_idx` must already hold the
    // post-append value when this is called. Calling it before the update makes
    // new == old, which reduces vring_need_event to (old - used_event - 1) < 0 --
    // always false, i.e. silently never notifies. Without EVENT_IDX this is the
    // old avail->flags test.
    bool should_notify(uint16_t old_used_idx);
    // Establish/maintain the invariant avail_event == last_avail. See the .cpp
    // for why that is an invariant and not a batching optimization.
    void publish_avail_event();

private:
    struct ReqArg { VirtQueueServer* q; uint16_t head; };
    static void* req_trampoline(void* a);
};

}  // namespace blk
}  // namespace photon
