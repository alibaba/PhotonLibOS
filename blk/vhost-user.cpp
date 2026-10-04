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

// VHOST-USER transport: exports a photon IFile as a virtio-blk DEVICE (the
// vhost-user backend) to a FRONTEND -- normally QEMU's vhost-user-blk, or the
// mock frontend in test-vhost-user.cpp. Unlike every other transport here
// there is NO kernel registration: the unix socket path IS the identity.
//   - SERVER role: we listen; a stale socket is unlinked and re-bound (the
//     create/attach distinction collapses); a frontend reconnect re-negotiates
//     from scratch.
//   - CLIENT role: the frontend holds the listener (the libvirt DAC /
//     path-labeling use case); we connect and the negotiation is identical.
// The frontend drives: it sends the VHOST_USER_* requests and we reply.
//
// Protocol facts (verified against QEMU docs/interop/vhost-user.rst -- the
// protocol's specification document -- and <linux/vhost_types.h>, 2026-09):
// - Message: {int32 request; uint32 flags; uint32 size; payload}, version 1
//   in flags bits 0-1; REPLY_MASK (0x4) marks a reply; NEED_REPLY_MASK (0x8)
//   asks for one (mandatory for every message once PROTOCOL_F_REPLY_ACK is
//   negotiated -- we negotiate it).
// - fds travel as SCM_RIGHTS cmsgs on the SAME unix socket (SET_MEM_TABLE's
//   region fds, SET_VRING_KICK/CALL's eventfds, SET_BACKEND_REQ_FD's channel).
//   KICK/CALL payloads are u64: low 8 bits = vq index, bit 8 = NOFD.
// - **Two address spaces** (the easy thing to get wrong): SET_VRING_ADDR's
//   desc/used/avail addresses are the FRONTEND's userspace addresses (QVAs --
//   they resolve against each region's userspace_addr), while the addresses
//   INSIDE the descriptor ring were written by the guest and are GPAs (resolved
//   against guest_phys_addr). Both names are fields of <linux/vhost_types.h>'s
//   struct vhost_memory_region; both resolve through the SET_MEM_TABLE regions
//   we mmap ourselves.
// - SET_VRING_BASE carries the split-ring last_avail_idx (the frontend owns
//   the crash-recovery bookkeeping); GET_VRING_BASE asks us to stop a vq and
//   return it.
// - GET_CONFIG/SET_CONFIG move the virtio device config (PROTOCOL_F_CONFIG);
//   capacity changes are announced by sending BACKEND_CONFIG_CHANGE (id 2) on
//   the backend channel fd from SET_BACKEND_REQ_FD (PROTOCOL_F_BACKEND_REQ).
//
// P1 scope: BlkConfig::queues virtqueues -- 0 means one, over MAX_QUEUES means
// clamped to it -- and multiqueue offered exactly when that count is more than one,
// in BOTH words: VIRTIO_BLK_F_MQ in the device features and
// VHOST_USER_PROTOCOL_F_MQ in the protocol features. Neither alone is enough -- the
// device bit is what tells the guest driver to use the queues, the protocol bit is
// what lets the primary find out how many there are. Split ring, no
// indirect descriptors offered (F_RING_INDIRECT_DESC not in our feature set),
// IN/OUT/FLUSH/GET_ID served; FEATURE_DISCARD/WRITE_ZEROES accepted in cfg
// but not offered. Each queue serves on one vcpu -- the caller's, or a pool
// vcpu when BlkConfig::pool is set -- while the control plane (the accept and
// message loops) stays on the caller's; what that split costs in synchronization
// is documented at vq_stop/vq_drain and at the quiesced handlers. The virtio-blk
// device-model core (constants, vring structs, desc-chain walk, request dispatch)
// is shared with the vduse transport via the internal blk/utils.{h,cpp}. All
// virtio fields are little-endian (VERSION_1), LE host assumed.

#include "blk.h"
#include "utils.h"
#include "vhost-user-wire.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
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

// vhost-user wire protocol constants and structs are in vhost-user-wire.h

#define VHU_MSG_MAX_FDS 8

// The smallest payload each request's handler reads, and 0 for the requests that
// read none. recv_msg bounded only the MAXIMUM, so a message declaring a shorter
// payload arrived with the union zeroed and every field reading back as 0 -- and
// for SET_MEM_TABLE that zero looked exactly like a frontend saying "no
// regions": nregions=0, which satisfied the handler's own region/fd agreement
// check, unmapped the live table, and was acked as a success. The device then had
// no memory to translate through and the frontend no reason to suspect it.
//
// A payload too short to hold the fields about to be read is a disagreement about
// FRAMING, not a bad value. It is caught here, before any handler runs, because a
// handler is handed a union that has already been zeroed and then partly filled: it
// cannot tell a truncated message from a well-formed one that declares zero unless
// it goes back to m->size and re-derives the framing itself. Doing that once, where
// the declared length is already in hand, is what keeps the handlers from each
// needing to know it. Ending the session matches the treatment of an oversized
// payload, and for the same reason: the peer's idea of the message layout and ours
// disagree, and nothing downstream can be trusted until that is settled.
//
// Semantic violations stay where they are: a region count that disagrees with the fd
// count, a vring num that is not a power of two, an index past the queue count.
// Those are readable, and their handlers reject them with an error ack and let the
// session carry on, so one bad message does not cost the peer its connection.
//
// SET_MEM_TABLE is the one request that needs both halves, because its payload is
// variable-length: the fixed prefix is checked here and the region count against the
// length that actually arrived is checked there, where the count is known.
static uint32_t payload_min(int32_t request) {
    switch (request) {
    case VHOST_USER_SET_FEATURES:
    case VHOST_USER_SET_PROTOCOL_FEATURES:
    case VHOST_USER_SET_VRING_NUM:
    case VHOST_USER_SET_VRING_BASE:
    case VHOST_USER_GET_VRING_BASE:
    case VHOST_USER_SET_VRING_ENABLE:
    case VHOST_USER_SET_VRING_KICK:
    case VHOST_USER_SET_VRING_CALL:
        return sizeof(uint64_t);            // u64, or vhost_vring_state: both 8
    case VHOST_USER_SET_VRING_ADDR:
        return sizeof(vhost_vring_addr);    // memcpy'd out whole
    case VHOST_USER_GET_CONFIG:
        return offsetof(vhost_user_config, flags);   // the offset and size it reads
    case VHOST_USER_SET_MEM_TABLE:
        // The fixed prefix only. How many regions follow is the handler's business,
        // because the count itself has to be on the wire before it can be checked
        // against the length -- see handle_mem_table.
        return offsetof(vhost_user_memory, regions);
    default:
        return 0;   // reads no payload: GET_*, OWNER/RESET, and the tolerated no-ops
    }
}

// ----------------------------------------------------------------------------

// a unix socket path lives in sockaddr_un::sun_path (108 bytes including the
// NUL) -- the hard bound on cfg.sock_path
static constexpr size_t SUN_PATH_MAX = sizeof(((sockaddr_un*)nullptr)->sun_path);

// Ceiling on a split virtqueue's size: virtio 1.2 §2.7.13 states 32768 as the
// maximum (the highest power of two that fits in 16 bits). Enforced on
// SET_VRING_NUM because the frontend's num reaches two modulo divisors
// (avail->ring[last_avail % num] in dispatch_avail, used->ring[used_idx % num]
// in vring_used_append) and sizes both the vring translation and the OUTER bound
// on the in-flight coroutine cap -- the caller's queue_depth is the inner one, so
// this is what a caller who never set it gets. Nonzero is not optional either, for
// the same reason: a zero divisor in either place.
static constexpr uint32_t MAX_VRING_NUM = 32768;

// the frontend's memory regions, mmap'd by us: translates BOTH address
// spaces (QVA for vring addresses, GPA for descriptor contents)
struct MemTable {
    struct Region {
        uint64_t gpa, qva, size;
        char* base;
        int fd;
    };
    std::vector<Region> regions;

    void clear() {
        for (auto& r : regions) {
            ::munmap(r.base, (size_t)r.size);
            ::close(r.fd);
        }
        regions.clear();
    }
    // Wrap-free containment test. The endpoint form `a + len - 1 < base + size`
    // overflows for an `a` near UINT64_MAX, and the wrapped sum then SATISFIES
    // the bound -- so the caller got base + (a - base) back, an arbitrarily
    // large negative offset into our own mappings, usable as a preadv
    // destination and a pwritev source. Both a and len are hostile: the vring
    // addresses come from the frontend, desc.addr/desc.len from the guest, and
    // serve_chain passes them straight in. Compare distances instead of
    // endpoints -- every subtraction here is guarded by the test before it.
    static bool contains(uint64_t a, size_t len, uint64_t base, uint64_t size) {
        return a >= base && len <= size && a - base <= size - len;
    }
    void* qva2va(uint64_t qva, size_t len) const {
        for (auto& r : regions)
            if (contains(qva, len, r.qva, r.size))
                return r.base + (qva - r.qva);
        return nullptr;
    }
    void* gpa2va(uint64_t gpa, size_t len) const {
        for (auto& r : regions)
            if (contains(gpa, len, r.gpa, r.size))
                return r.base + (gpa - r.gpa);
        return nullptr;
    }
};

// The identity lock file name for a socket path, inside the controller's
// directory. The socket's own basename cannot serve as it is: devlock_acquire
// opens with O_CREAT|O_RDWR, and opening a unix socket node that way fails ENXIO.
// So the name is affixed, in the same shape vduse gives its tombstones.
//
// Derived from the BASENAME, which assumes what this controller already assumes
// everywhere else -- that its directory is one flat level of sockets, which is
// exactly what its orphan scan reads it as. Two sockets with the same basename in
// different subdirectories of one scope would share a lock and refuse each other.
// That is a false refusal rather than a missed one, and it errs the way a lock
// should.
static constexpr size_t VHU_LOCK_BUF = 256;

static int vhu_lock_name(const char* sock_path, char* buf, size_t n) {
    const char* base = strrchr(sock_path, '/');
    base = base ? base + 1 : sock_path;
    if (!*base)
        LOG_ERROR_RETURN(EINVAL, -1, "vhost-user socket path has no basename: ", sock_path);
    // A basename is at most SUN_PATH_MAX-1 and the affixes add nine bytes, so the
    // digest case vduse needs for over-long names cannot arise here. The bound is
    // checked anyway: a truncated name would lock a DIFFERENT identity, which is
    // the one failure a lock must not have.
    if (snprintf(buf, n, "vhu-%s.lock", base) >= (int)n)
        LOG_ERROR_RETURN(ENAMETOOLONG, -1, "vhost-user socket basename too long to lock: ", base);
    return 0;
}

struct VhostUserDeviceImpl : IBlkDevice {
    VhostUserController::Config cfg;

    // Per-queue state. ONE heap allocation per queue, and the pointer vector is
    // deliberate: VirtQueueServer holds a std::atomic, so it is neither copyable
    // nor movable and cannot live in a vector by value -- resize() needs
    // MoveInsertable. Pointers also make every address here stable for the
    // device's lifetime, which the hooks below depend on: they are bound with
    // `this` == the Vq*, and a reallocation would leave them pointing at freed
    // memory.
    struct VqVhu {
        uint64_t desc_qva = 0, used_qva = 0, avail_qva = 0;   // retranslate from
                                                             // these on a new
                                                             // memory table
        // Atomic: the ready hook (vq_may_dispatch) reads this on the queue's own
        // vcpu while handle_mem_table, SET_VRING_ENABLE and stop_session write it
        // on the control plane's vcpu, and those writes are not all inside a
        // quiesce -- two of them deliberately precede vq_stop. Relaxed accesses
        // only: nothing else is published through this flag.
        std::atomic<bool> enabled{false};   // the frontend's SET_VRING_ENABLE state
        bool addr_set = false;     // SET_VRING_ADDR translated successfully
        int callfd = -1;           // completion eventfd (owned here)
        photon::thread* th = nullptr;
        // The vcpu this queue's loop coroutine runs on, recorded by vq_start
        // immediately after the migration: photon::get_vcpu(thread*) reads the field
        // do_thread_migrate stores under the thread's own lock before it returns, so
        // the migrator already holds the answer. That is why no completion handshake
        // belongs here -- a semaphore exists to tell a waiter that some work
        // finished, and no work has to finish for this value to be known. An
        // accepted migration leaves nothing between the create and this read that
        // yields (thread_create only queues the new coroutine, thread_migrate does
        // not switch for a thread that is not the caller, get_vcpu is an inline
        // field read), so no other coroutine on this vcpu can catch the loop created
        // while `home` is still null. That a null means "no loop to race" is the
        // direction run_on_home's !home branch rests on, and the only one that holds:
        // vq_stop_here nulls `th` on the serving vcpu inside the hop, and vq_stop
        // nulls `home` here only after that hop returns. A refused migration needs no
        // handling either, though it is not yield-free -- thread_migrate logs every
        // refusal and migrate_to_pool warns behind it, so a caller-installed sink
        // writing through a photon IFile does yield in here. No reader sees the
        // window that opens, because vq_start runs only from the message loop, and the
        // only other two paths into those teardown wrappers -- stop_session and
        // rollback -- join that loop before they touch a queue (the argument is at
        // run_on_home's !home branch). The value is right anyway, since
        // do_thread_migrate leaves the field untouched when it says no, and what it
        // left is this vcpu, which is where the loop then really runs. With cfg.pool
        // null it is simply the caller's own vcpu.
        //
        // vq_stop/vq_drain move their work there, because the loop and the request
        // coroutines it spawned share it and are the only writers of last_avail,
        // used_idx and the used ring WHILE THE LOOP IS LIVE -- the control plane
        // writes them too (vq_start republishes both, SET_VRING_BASE sets
        // last_avail), but only with the loop joined AND its requests drained
        // (vq_stop then vq_drain), which is the same reason it needs no hop for
        // those.
        //
        // Control-plane field: written by vq_start and read by the teardown callers,
        // every one of which runs on the vcpu that called start() -- the message loop
        // too, which start() creates there and which is deliberately never migrated.
        // That is a contract with the caller, not a property of this code: `home` is a
        // plain pointer, and nothing here would notice a detach() or a shutdown()
        // issued from another vcpu -- `home` would then be written by vq_start on the
        // vcpu that called start() and read and cleared by the detaching caller on
        // the other one: two OS threads, one plain field, nothing ordering them. The
        // loop's own vcpu never touches it. vq_stop clears it once the loop is joined,
        // so a drain that follows a stop runs in place -- which it may, because
        // drain() polls nothing but the atomic in_flight and therefore has no vcpu it
        // must be on. The drains that precede a stop still hop. Clearing it is also
        // what makes a restart inside one session safe: the next vq_start records the
        // landing vcpu of the loop it just created instead of leaving the previous
        // loop's here.
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
        VhostUserDeviceImpl* impl = nullptr;
        uint32_t qid = 0;
        VirtQueueServer srv;
        VqVhu x;
    };
    std::vector<Vq*> vqs;

    fs::IFile* backend = nullptr;
    uint64_t offer_features = 0;
    uint64_t negotiated = 0;
    uint64_t proto_features = 0;   // the frontend's accepted protocol subset
    uint64_t capacity_sectors = 0;
    photon::thread* accept_th = nullptr;   // SERVER: the accept loop
    photon::thread* msg_th = nullptr;      // the negotiation/message loop
    MemTable mem;
    alignas(8) uint8_t dev_config[sizeof(virtio_blk_config)] = {};

    int listen_fd = -1;            // SERVER role
    int conn_fd = -1;              // the live frontend connection
    int backend_req_fd = -1;       // SET_BACKEND_REQ_FD channel (config events)
    // How many virtqueues this device serves, from BlkConfig::queues (0 = one,
    // over the transport maximum = clamped to it). It has to equal every count we
    // publish to the frontend -- the GET_QUEUE_NUM answer and
    // virtio_blk_config::num_queues -- because a frontend that reads one count
    // while the device serves another addresses queues nobody is listening on, and
    // its requests vanish. Fixed in the constructor, never resized afterwards.
    uint32_t nqueues = 1;

    // Log this as `(const char*)sock_path`, never as VALUE(sock_path): VALUE on a
    // char array deduces a reference to the whole array and alog then emits all
    // SUN_PATH_MAX bytes, path followed by NUL padding (measured).
    char sock_path[SUN_PATH_MAX] = {};   // bounded by sockaddr_un::sun_path
    // The controller's directory, which is where this device's identity lock
    // lives. A copy, not a back-pointer: a device may outlive the controller that
    // made it, and do_listen() needs the directory for as long as serving does.
    char lock_dir[SCOPE_DIR_BUF] = {};
    // What VIRTIO_BLK_T_GET_ID answers with. The socket's basename, so that two
    // devices served by one daemon do not report the same serial to their guests --
    // a fixed per-transport string did. It is as unique as this controller's identity
    // lock, and for the same reason: vhu_lock_name() derives that from the basename
    // too, on the flat-one-level-of-sockets assumption the orphan scan already makes,
    // so a basename collision inside one scope is refused by the lock before it can
    // become two devices with one serial.
    char serial[SUN_PATH_MAX] = {};

    bool own_backend = false;
    bool started = false;
    uint8_t  sector_shift = 9;
    bool     read_only = false;
    // Written and read only by the control plane (msg_loop, accept_loop, start,
    // stop_session, rollback), all of which stay on the caller's vcpu -- the
    // serving side is told to stop through the atomic VirtQueueServer::stopping
    // plus wake/interrupt/join instead. So this needs no atomic.
    bool stopping = false;

    explicit VhostUserDeviceImpl(const VhostUserController::Config& c, const char* dir) : cfg(c) {
        sector_shift = cfg.info.sector_size_shift;
        read_only = cfg.read_only;
        capacity_sectors = cfg.info.size >> 9;
        snprintf(sock_path, sizeof(sock_path), "%s", cfg.sock_path.c_str());
        // The same basename vhu_lock_name() locks on. A path with no basename leaves
        // this empty, and such a device is refused later by that function -- so an
        // empty serial is never served, only constructed.
        const char* base = strrchr(sock_path, '/');
        snprintf(serial, sizeof(serial), "%s", base ? base + 1 : sock_path);
        // bounded: the factory checked the length before it got here, and a
        // truncated directory would lock in a DIFFERENT one than the scan reads
        snprintf(lock_dir, sizeof(lock_dir), "%s", dir);

        // Clamped, not rejected, and 0 means "you choose" -- the same reading the
        // ublk transport gives this field, so one BlkConfig means the same thing
        // to every transport. Derived HERE and never again: the constructor below
        // sizes vqs from it, GET_QUEUE_NUM answers with it and fill_config
        // publishes it, so a later change would desynchronize the three.
        nqueues = cfg.queues ? std::min<uint32_t>(cfg.queues, MAX_QUEUES) : 1;

        offer_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_BLK_F_BLK_SIZE) |
                         (1ULL << VIRTIO_RING_F_EVENT_IDX) |
                         (1ULL << VHOST_USER_F_PROTOCOL_FEATURES);   // we always answer
                                                                     // GET_PROTOCOL_FEATURES
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
        vqs.reserve(nqueues);
        for (uint32_t i = 0; i < nqueues; i++) {
            auto* q = new Vq;
            q->impl = this;
            q->qid = i;
            vqs.push_back(q);
        }
        fill_config();
        // The effective half of the descriptor; blk.h documents each axis. `offered`
        // is what this implementation can serve at all and does not depend on what was
        // asked for, so `features & ~offered` shows a caller the requests this transport
        // accepted and cannot honour. The backlog is PeerSide: the rings live in the
        // frontend's memory and detach() closes the connection, so what survives a
        // detach is held by the peer and comes back only when it reconnects -- there is
        // nothing on this side for a later start() to harvest. shutdown() is detach(true)
        // plus unlinking the socket, i.e. it ends the session rather than refusing.
        cfg.info.offered = FEATURE_FLUSH;
        cfg.info.backlog = BlkBacklog::PeerSide;
        cfg.info.shutdown_refusal = BlkShutdownRefusal::Disconnects;
        cfg.info.resize_effect = BlkResizeEffect::BestEffortNotify;
        cfg.info.adoption = BlkAdoption::NoRegistration;
        cfg.info.detach_no_wait = false;
    }

    // pure config validation -- no I/O, no kernel access. The factory runs it
    // before constructing, so a constructed device is always config-valid.
    static int validate(const VhostUserController::Config& c) {
        if (c.sock_path.empty() || c.sock_path.size() >= SUN_PATH_MAX)
            LOG_ERROR_RETURN(EINVAL, -1, "vhost-user sock_path must be 1..` chars",
                             (int)SUN_PATH_MAX - 1);
        if (validate_info(c.info, /*virtio=*/true) < 0)
            return -1;
        return 0;
    }

    const BlkDevInfo& get_info() const override { return cfg.info; }

    // Always nullptr: nothing here touches the kernel, so the only block device
    // that ever exists is the guest's -- there is no local node to name.
    const char* get_device_node() override { return nullptr; }

    ~VhostUserDeviceImpl() {
        if (started)
            shutdown();
        // the backstop for a session ended by detach() rather than shutdown()
        release_backend();
        // Only here, never in rollback(): a rolled-back device must stay able to
        // start again, and these are the slots it starts.
        for (auto* q : vqs)
            delete q;
        vqs.clear();
        if (listen_fd >= 0) ::close(listen_fd);
    }

    // Delete a backend this object owns and forget it either way, so that neither
    // the destructor nor a later start() can see it.
    void release_backend() {
        if (own_backend)
            delete backend;
        backend = nullptr;
        own_backend = false;
    }

    // ----- raw socket layer (recvmsg/sendmsg for the SCM_RIGHTS fds; photon
    // streams cannot carry cmsgs, so everything is plain syscalls + fd waits,
    // the utils.cpp precedent) -----

    // Harden one fd that arrived over SCM_RIGHTS. There is no SOCK_CLOEXEC /
    // SOCK_NONBLOCK equivalent for a received fd, so both have to be set by
    // hand -- the sockets in this file already ask for them at creation, and
    // utils.cpp's unix_listener_live sets them explicitly for the same reason.
    //
    // O_NONBLOCK is load-bearing: the serving vcpu does bare read()/write() on
    // these (vq_notify's callfd, VirtQueueServer::wake() and the kick drain's
    // kickfd). A peer that hands over a pipe whose 64 KiB buffer fills would
    // otherwise block in write() and stall EVERY coroutine on the vcpu, not
    // just this device -- AGENTS.md's "Blocking syscalls in coroutines".
    // FD_CLOEXEC because these are the peer's eventfds and would survive into
    // any fork()+exec() child.
    static int harden_recv_fd(int fd) {
        int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
            LOG_ERRNO_RETURN(0, -1, "vhost-user: O_NONBLOCK on received fd ` failed", fd);
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
            LOG_ERRNO_RETURN(0, -1, "vhost-user: FD_CLOEXEC on received fd ` failed", fd);
        return 0;
    }

    // Read exactly n bytes, looping over short reads, and accumulate every
    // SCM_RIGHTS fd that any recvmsg along the way delivered. 0 ok, -1 closed.
    int recv_exact(int fd, void* buf, size_t n, int* fds, int* nfds) {
        size_t off = 0;
        while (off < n) {
            if (photon::wait_for_fd_readable(fd) < 0) {
                if (stopping) return -1;
                if (errno == EINTR) continue;
                LOG_ERRNO_RETURN(0, -1, "vhost-user recv wait failed");
            }
            iovec iov{(char*)buf + off, n - off};
            char cbuf[CMSG_SPACE(sizeof(int) * VHU_MSG_MAX_FDS)];
            msghdr mh;
            memset(&mh, 0, sizeof(mh));
            memset(cbuf, 0, sizeof(cbuf));
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = cbuf;
            mh.msg_controllen = sizeof(cbuf);
            ssize_t r = ::recvmsg(fd, &mh, MSG_DONTWAIT);
            if (r < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                LOG_ERRNO_RETURN(0, -1, "vhost-user recvmsg failed");
            }
            if (r == 0)
                return -1;   // the frontend closed mid-message
            off += (size_t)r;
            if (mh.msg_flags & MSG_CTRUNC)
                LOG_WARN("vhost-user: the frontend attached more than ` fds to one message; the surplus did not reach us", VHU_MSG_MAX_FDS);
            for (cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
                if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
                int k = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                if (*nfds + k > VHU_MSG_MAX_FDS) {
                    // The kernel caps one SCM_RIGHTS cmsg at our msg_controllen,
                    // so this needs the header read AND the payload read to each
                    // bring some. They are already in our fd table but cannot all
                    // be recorded, and we could not say which handler they were
                    // for -- close them rather than leak, and refuse the message.
                    for (int i = 0; i < k; i++)
                        ::close(((int*)CMSG_DATA(cm))[i]);
                    LOG_ERROR_RETURN(EPROTO, -1, "vhost-user: one message carried more than ` fds", VHU_MSG_MAX_FDS);
                }
                memcpy(fds + *nfds, CMSG_DATA(cm), (size_t)k * sizeof(int));
                *nfds += k;
            }
        }
        return 0;
    }

    // receive one message (+ up to VHU_MSG_MAX_FDS); 0 ok, -1 closed/error
    //
    // Two reads on purpose, and never for more than the bytes of THIS message.
    // The connection is SOCK_STREAM, so a recvmsg asking for sizeof(vhost_user_msg)
    // -- the header plus the whole payload union -- got whatever the kernel had
    // glued into one chunk: consecutive writes from the same peer are merged
    // into a single receive unless their SCM_RIGHTS differ, so SET_OWNER and the
    // GET_FEATURES behind it arrived together and the second message's bytes
    // were discarded past the first message's `size`. A frontend that pipelines
    // then waited forever for an answer we never knew to give. This was not
    // reasoned out from a header: it deadlocked against a real QEMU frontend,
    // which sends both back to back without waiting for a reply. Our own mock is
    // strictly request/reply, so it never showed.
    //
    // Exact sizing also fixes fd attribution: the fds travelling with a message
    // are handed to the FIRST read that touches any of that message's bytes, and
    // a read that leaves bytes behind returns immediately rather than waiting for
    // the next message. So an fd-bearing message queued behind a short one would
    // otherwise have its fds land in the same chunk as the other message's
    // bytes -- and on the wrong handler.
    int recv_msg(int fd, vhost_user_msg* m, int* fds, int* nfds) {
        *nfds = 0;
        // A failure after some fds arrived must not leave them half-owned: the
        // caller's close-the-rest cleanup only runs on success.
        bool ok = false;
        DEFER(if (!ok) { for (int i = 0; i < *nfds; i++) ::close(fds[i]); *nfds = 0; });
        if (recv_exact(fd, m, offsetof(vhost_user_msg, payload), fds, nfds) < 0)
            return -1;
        // copied out first: m is not const here, and alog's forwarding
        // reference cannot bind a packed field (see the access rule above)
        int32_t req = m->request;
        uint32_t sz = m->size;
        if (sz > sizeof(m->payload))
            LOG_ERROR_RETURN(EPROTO, -1, "vhost-user request ` declares a ` byte payload, the largest this protocol has is `", req, sz, (uint32_t)sizeof(m->payload));
        // Both bounds belong here rather than in the handlers: this is the one
        // place that runs before the ambiguity exists. Past the memset below,
        // bytes after the declared length read as zeroes, exactly like ones a
        // well-formed message would have carried. A handler can still read
        // m->size -- handle_mem_table does, for its variable-length tail --
        // but only a bound checked here refuses a message before any of its
        // fields has been read out of that zeroed region.
        uint32_t need = payload_min(req);
        if (sz < need)
            LOG_ERROR_RETURN(EPROTO, -1, "vhost-user request ` declares a ` byte payload, its handler reads at least `", req, sz, need);
        memset(&m->payload, 0, sizeof(m->payload));   // no stale union bytes from
                                                      // the previous message
        if (m->size && recv_exact(fd, &m->payload, m->size, fds, nfds) < 0)
            return -1;
        ok = true;
        return 0;
    }

    int send_msg(int fd, const vhost_user_msg* m, const int* fds = nullptr, int nfds = 0) {
        size_t off = 0;
        size_t total = offsetof(vhost_user_msg, payload) + m->size;
        for (;;) {
            iovec iov{(char*)m + off, total - off};
            char cbuf[CMSG_SPACE(sizeof(int) * VHU_MSG_MAX_FDS)];
            msghdr mh;
            memset(&mh, 0, sizeof(mh));
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            if (nfds > 0 && off == 0) {
                memset(cbuf, 0, sizeof(cbuf));
                mh.msg_control = cbuf;
                mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
                cmsghdr* cm = CMSG_FIRSTHDR(&mh);
                cm->cmsg_level = SOL_SOCKET;
                cm->cmsg_type = SCM_RIGHTS;
                cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
                memcpy(CMSG_DATA(cm), fds, sizeof(int) * (size_t)nfds);
            }
            ssize_t w = ::sendmsg(fd, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN)
                    LOG_ERRNO_RETURN(0, -1, "vhost-user sendmsg failed");
                if (photon::wait_for_fd_writable(fd) < 0) {
                    if (stopping) return -1;
                    if (errno == EINTR) continue;
                    LOG_ERRNO_RETURN(0, -1, "vhost-user send wait failed");
                }
                continue;
            }
            off += (size_t)w;
            nfds = 0;   // fds are consumed by the first (partial) send
            if (off >= total)
                return 0;
        }
    }

    // send a u64 reply (REPLY_MASK set); used for GET_* answers and REPLY_ACKs
    int reply(int fd, int32_t req, uint64_t u64) {
        vhost_user_msg m;
        memset(&m, 0, sizeof(m));
        m.request = req;
        m.flags = VHOST_USER_VERSION | VHOST_USER_REPLY_MASK;
        m.size = sizeof(m.payload.u64);
        m.payload.u64 = u64;
        return send_msg(fd, &m);
    }
    int reply_blob(int fd, int32_t req, const void* data, uint32_t len) {
        vhost_user_msg m;
        memset(&m, 0, sizeof(m));
        m.request = req;
        m.flags = VHOST_USER_VERSION | VHOST_USER_REPLY_MASK;
        m.size = len;
        if (len) memcpy(&m.payload, data, len);
        return send_msg(fd, &m);
    }

    // ----- the virtqueue: the shared engine plus the vhost-user hooks -----

    // `writable` is not consulted, and that is an answer rather than an omission: the
    // frontend's memory regions are the guest's RAM, mmap'd read-write whole, so there
    // is no per-buffer permission here that could disagree with the access, and every
    // direction is servable. vduse is the transport whose mappings carry one, because
    // theirs come from the kernel's DMA mapping of one specific buffer.
    void* vq_translate(uint64_t addr, size_t len, bool writable) {
        // descriptor contents are guest-written GPAs (the vring addresses
        // themselves were QVAs -- two different spaces, see the header)
        return mem.gpa2va(addr, len);
    }

    // Signal the driver's callfd (one eventfd write). The used ring advanced and
    // the driver asked for an IRQ -- and once more when SET_VRING_CALL installs a
    // fresh fd, see there.
    void vq_notify(uint32_t idx) {
        int callfd = vqs[idx]->x.callfd;
        if (callfd < 0)
            return;
        uint64_t one = 1;
        ssize_t w = ::write(callfd, &one, sizeof(one));   // eventfd signal
        if (w < 0)
            LOG_WARN("vhost-user callfd signal failed, ", ERRNO());
    }

    // Bound to Hooks::ready, so this is the engine's DISPATCH gate: may the loop
    // take more work from this ring. `enabled` belongs here and only here -- it
    // tracks the frontend's SET_VRING_ENABLE state, and the transport also clears it
    // while quiescing a queue for a remap, a resize or a teardown. What clearing it
    // must not do is retire the requests already dispatched: a frontend pause leaves
    // the ring published and every mapping intact, which is why the engine's
    // completion path does not read this hook at all. See handle_req.
    bool vq_may_dispatch(uint32_t idx) {
        auto* q = vqs[idx];
        return q->x.enabled.load(std::memory_order_relaxed) && q->x.addr_set &&
               q->srv.desc && q->srv.num;
    }

    // The hooks are bound with the Vq* as their context object, so each one can
    // recover its queue index. Hooks::notify and Hooks::ready are Delegate<void>
    // and Delegate<bool>, whose bind(void*, Func) wants a free function taking the
    // context first -- a member function pointer bound to `this` cannot carry the
    // index, which is why these thunks exist. What they are bound to is a heap
    // Vq whose address never moves, which is what the pointer vector buys.
    static void notify_thunk(void* a) {
        auto* q = (Vq*)a;
        q->impl->vq_notify(q->qid);
    }
    static bool ready_thunk(void* a) {
        auto* q = (Vq*)a;
        return q->impl->vq_may_dispatch(q->qid);
    }
    static void* translate_thunk(void* a, uint64_t addr, size_t len, bool writable) {
        // the memory table is device-wide, not per queue
        return ((Vq*)a)->impl->vq_translate(addr, len, writable);
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
        q->srv.serial = serial;
        q->srv.tag = sock_path;
        q->srv.hooks.translate.bind(q, &translate_thunk);
        q->srv.hooks.notify.bind(q, &notify_thunk);
        q->srv.hooks.ready.bind(q, &ready_thunk);
    }

    // ---- running queue-side work where the queue lives ----
    //
    // The loop coroutine and every request coroutine it spawned run on `home`,
    // which is a pool vcpu once BlkConfig::pool is set. Three things have to join
    // them there instead of running here on the control plane's vcpu: interrupting
    // and joining the loop, waiting out the requests it dispatched, and the
    // backlog wait that reads last_avail while counting on the loop to advance it.
    // photon::thread_migrate only accepts a READY thread, so a caller cannot move
    // itself -- hand the work to a coroutine that can be moved.
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
            // !home means this queue has no loop coroutine, and nothing else:
            // vq_start records `home` in the same stretch that creates the loop, and
            // vq_stop clears it only after joining that loop. That stretch is
            // yield-free only for an accepted migration -- a refusal logs, and a
            // caller-installed sink writing through a photon IFile makes the log
            // yield -- so what closes the window where the loop exists and this reads
            // null is who can get here, and that is not visible from this function:
            // all three call sites are teardown (vq_stop, vq_drain,
            // vq_backlog_drain), vq_start is reachable only from the message loop's
            // own handling, that loop is a single coroutine, and the two callers
            // which are not it (stop_session, rollback) join it before they touch a
            // queue. So nobody can be in here while a start is halfway through, and
            // a null is either "this queue was never started" or "it was stopped and
            // cleared" -- in both cases nothing on another vcpu writes what the body
            // reads.
            body.fire();
            return;
        }
        HomeArg ha{body, home};
        auto th = photon::thread_create(&VhostUserDeviceImpl::home_thunk, &ha);
        if (!th) {
            // Cannot honour the vcpu rule, but leaving the queue up is worse: the
            // caller is tearing it down and the memory table is about to go.
            LOG_ERROR("vhost-user: cannot create the coroutine for the serving vcpu, running it here");
            body.fire();
            return;
        }
        photon::thread_enable_join(th);
        if (photon::thread_migrate(th, home) < 0)
            LOG_WARN("vhost-user: cannot move the work back to the serving vcpu, ", ERRNO());
        ha.done.wait(1);
        photon::thread_join((photon::join_handle*)th);
    }

    // `_here` means "already on this queue's home vcpu". Callers use the
    // unsuffixed wrapper, which does the hop -- wrapping it at every call site
    // instead is how one of them gets missed, and a missed one is a cross-vcpu
    // interrupt/join.
    void vq_stop_here(uint32_t idx) {
        auto* q = vqs[idx];
        if (!q->x.th) return;
        q->x.enabled.store(false, std::memory_order_relaxed);
        q->srv.run.store(false, std::memory_order_relaxed);
        q->srv.wake();   // out of its kickfd wait
        photon::thread_interrupt(q->x.th);
        photon::thread_join((photon::join_handle*)q->x.th);
        q->x.th = nullptr;
    }

    // Cleared in the wrapper, not in vq_stop_here: that one runs after the hop, on
    // the serving vcpu, and `home` is only ever written on this one. This is late
    // enough for the drains that precede a stop to still hop, and a vq_start that
    // follows records the new loop's landing vcpu over the null.
    void vq_stop(uint32_t idx) {
        run_on_home(vqs[idx]->x.home, [&] { vq_stop_here(idx); });
        vqs[idx]->x.home = nullptr;
    }

    // The requests hold iovs into the memory table and complete into the used
    // ring, both of which the caller is about to unmap or re-read.
    void vq_drain(uint32_t idx) {
        run_on_home(vqs[idx]->x.home, [&] { vqs[idx]->srv.drain(); });
    }

    // Wait out what this queue still owes before it is stopped: the requests
    // already dispatched and, when the caller asked for an orderly handover, the
    // avail backlog the loop has not consumed yet. This reads last_avail and
    // dereferences avail WHILE counting on the loop to keep advancing them, so it
    // cannot run anywhere else: from another vcpu both reads race, and a stale
    // last_avail would keep the wait spinning after the loop already caught up.
    void vq_backlog_drain(uint32_t idx, bool drain_backlog) {
        run_on_home(vqs[idx]->x.home, [&] {
            auto* q = vqs[idx];
            if (!q->x.th)
                return;
            // addr_set, not srv.desc: the frontend supplies the three vring QVAs
            // independently, so it can make desc resolve while avail does not, and
            // this dereferences avail. addr_set is exactly "all three resolved"
            // (see vq_retranslate).
            while (q->srv.in_flight.load() ||
                   (drain_backlog && q->x.enabled.load(std::memory_order_relaxed) &&
                    q->x.addr_set &&
                    q->srv.last_avail != vring_avail_idx(q->srv.avail)))
                photon::thread_usleep(1000);
        });
    }

    void vq_start(uint32_t idx) {
        auto* q = vqs[idx];
        if (q->x.th || !vq_may_dispatch(idx))
            return;   // not fully configured yet
        // kickfd is deliberately NOT part of this gate. A SET_VRING_KICK that
        // carries NOFD is protocol-legal and means "there is no kick fd", not
        // "the ring is not ready"; loop() polls every KICK_FALLBACK_US in that
        // case, which is the only way work can be discovered. Refusing to start
        // here made the device silently serve nothing forever.
        q->srv.used_idx = vring_used_idx(q->srv.used);
        // Honour the cursor the frontend gave us in SET_VRING_BASE. For a clean
        // handover between two of our daemons it is exact: GET_VRING_BASE drains,
        // stops and returns our own last_avail, which the frontend stores and hands
        // back on the next start. The previous daemon's dispatched-but-uncompleted
        // entries are genuinely unrecoverable -- they lived in its memory -- and
        // resuming at BASE accepts that loss without publishing duplicate
        // completions for heads the driver has already reclaimed.
        //
        // used->idx is NOT a substitute: it counts completions, not consumed avail
        // entries, and out-of-order completion means the set of completed entries
        // is not a contiguous prefix of the avail ring. Resuming from it both loses
        // uncompleted entries below it and re-serves completed ones above it,
        // producing duplicate used elements for heads the driver no longer owns.
        // The wrap-safe comparison here defends only against a stale BASE that
        // wrapped past used_idx by more than half the counter space -- an artefact
        // of a very old frontend record combined with a 16-bit turn -- and even
        // then it raises to used_idx rather than adopting a cursor known to be
        // wrong. A sane frontend never supplies one.
        if ((uint16_t)(q->srv.last_avail - q->srv.used_idx) >= 0x8000)
            q->srv.last_avail = q->srv.used_idx;
        // Establish avail_event == last_avail before the loop can sleep on the
        // kickfd. SET_VRING_BASE lets the frontend put last_avail anywhere
        // (handle_msg's SET_VRING_BASE branch), and the two lines above can raise
        // it to used_idx, while avail_event still holds whatever the peer's setup
        // left there -- zero for a fresh ring. Without this publish the invariant
        // does not hold until the first head is consumed, and a driver that kicks
        // in that window is legitimately ignored per §2.7.10.1.
        q->srv.publish_avail_event();
        q->srv.stopping.store(false, std::memory_order_relaxed);
        q->srv.run.store(true, std::memory_order_relaxed);
        // Logged before the create: all three are plain fields the loop advances
        // on its own vcpu, so reading them from here afterwards would be a race.
        // The values are final by now -- num comes from SET_VRING_NUM and the
        // other two from the lines just above -- so this prints the same thing.
        LOG_INFO("vhost-user vq` serving: num ` last_avail ` used_idx `",
                 idx, q->srv.num, q->srv.last_avail, q->srv.used_idx);
        q->x.th = photon::thread_create(&loop_thunk, q);
        if (!q->x.th)
            LOG_ERROR_RETURN(ENOMEM, , "vhost-user: cannot create the vq` loop coroutine", idx);
        // enable_join BEFORE the migration: it writes a flag in the thread's own
        // struct, and once migrated that thread is running on another OS thread,
        // so writing it afterwards is an unsynchronized cross-thread write.
        // thread_migrate still applies -- it needs a READY thread on this vcpu,
        // which is exactly what thread_create left, and enable_join does not
        // change either property. The control-plane coroutines (accept_th, msg_th)
        // are deliberately NOT migrated -- they stay on the caller's vcpu.
        //
        // Nothing between the create and the migration may yield, or the loop runs
        // here first and parks in its kickfd wait -- WAITING, not READY -- and the
        // migration fails. The default log path does not (alog takes a spinlock),
        // but a caller-installed sink writing through a photon IFile would, which
        // is why the LOG_INFO above sits before the create rather than after it.
        photon::thread_enable_join(q->x.th);
        migrate_to_pool(cfg.pool, q->x.th);
        // After the migration, and that is not an exception to the rule above: this
        // is an inline field read, so it cannot yield, and the field it reads is
        // written only by do_thread_migrate under the thread's own lock. The thread
        // cannot have gone away either -- enable_join above keeps its struct alive
        // even if the loop exits at once, until vq_stop_here joins it.
        q->x.home = photon::get_vcpu(q->x.th);   // already final -- see its declaration
    }

    // ----- the message loop (one frontend session) -----

    // recompute the vring HVAs from the stored QVAs against the current table
    void vq_retranslate(uint32_t idx) {
        auto* q = vqs[idx];
        if (!q->srv.num || !q->x.desc_qva)
            return;
        size_t dsz = (size_t)q->srv.num * sizeof(vring_desc);
        size_t asz = sizeof(uint16_t) * (3 + q->srv.num);
        size_t usz = sizeof(uint16_t) * 3 + sizeof(vring_used_elem) * q->srv.num;
        auto* d = (vring_desc*)mem.qva2va(q->x.desc_qva, dsz);
        auto* a = (vring_avail*)mem.qva2va(q->x.avail_qva, asz);
        auto* u = (vring_used*)mem.qva2va(q->x.used_qva, usz);
        // set_ring rather than four assignments: the HVAs just moved under any
        // request still in flight against the old ones, and the generation bump
        // is what retires it. num is passed through in both arms, so a failed
        // translation cannot lose the resize this retranslation came from.
        q->srv.set_ring(d, a, u, q->srv.num);
        q->x.addr_set = q->srv.desc && q->srv.avail && q->srv.used;
    }

    // the mappings the vring HVAs were translated through are gone: drop the
    // addresses as well, or vq_may_dispatch() keeps passing (it only re-checks
    // enabled/addr_set/desc/num) and a later SET_VRING_ENABLE dispatches into
    // freed memory
    void vq_invalidate(uint32_t idx) {
        auto* q = vqs[idx];
        // same reason as vq_retranslate, and num survives here too: it is the
        // frontend's SET_VRING_NUM value, not a property of the lost mapping
        q->srv.set_ring(nullptr, nullptr, nullptr, q->srv.num);
        q->x.addr_set = false;
    }

    // Takes ownership of EVERY fd in fds[], marking the ones it keeps as -1 so
    // msg_loop closes only what is left over.
    int handle_mem_table(const vhost_user_msg* m, int* fds, int nfds) {
        vhost_user_memory t;   // memcpy out, per the access rule on vhost_user_msg
        memcpy(&t, &m->payload.memory, sizeof(t));
        uint32_t n = t.nregions;
        uint32_t sz = m->size;
        // Validate BEFORE anything is torn down: a rejected message must leave
        // the device serving exactly as it was, since a failed SET_MEM_TABLE
        // only sets ack=1 and the session carries on. Unmapping first does not
        // leave a device that merely stops serving -- see vq_invalidate() for
        // the mechanism, and note it was measured: clearing the table without
        // also invalidating the queue addresses took the process down.
        //
        // n has a floor and not just a ceiling: zero regions satisfies every
        // agreement check below -- it matches a fd count of zero, and it needs
        // no region bytes past the prefix -- and before this floor existed it
        // went on to unmap the running table while acking success. An empty
        // table is not a state this device can serve, so a message declaring
        // one is either truncated (recv_msg catches that before it gets here)
        // or lying, and both are rejections.
        if (n < 1 || n > 8 || (int)n != nfds)
            LOG_ERROR_RETURN(EPROTO, -1, "vhost-user mem table: ` regions vs ` fds", n, nfds);
        // recv_msg proved only the fixed prefix arrived, so the declared count has
        // to be re-checked against the length that actually came: regions past it
        // would be read out of the zeroed union and fail several steps later as a
        // zero-length mmap, naming a cause that is not the real one.
        uint32_t need = (uint32_t)(offsetof(vhost_user_memory, regions) +
                                   (size_t)n * sizeof(vhost_user_memory_region));
        if (sz < need)
            LOG_ERROR_RETURN(EPROTO, -1, "vhost-user mem table: ` regions need ` payload bytes, the message carried `", n, need, sz);

        // Mapped COMPLETE before the running table is touched, and that order is
        // the other half of the promise above. Clearing first and mapping second
        // meant an mmap failure -- ENOMEM, a region of zero length, one more
        // mapping than the process is allowed -- left the device with no memory at
        // all, and the error ack told the frontend only that the NEW table was
        // refused, not that the old one was already gone. Nothing below this
        // loop returns an error, so from here the swap is unconditional.
        MemTable next;
        bool ok = false;
        DEFER(if (!ok) next.clear());
        next.regions.reserve(n);
        for (uint32_t i = 0; i < n; i++) {
            vhost_user_memory_region& r = t.regions[i];
            int fd = fds[i];
            fds[i] = -1;   // ours from here: the mmap failure below closes it,
                           // and anything past it belongs to the region table
            void* base = ::mmap(nullptr, (size_t)r.memory_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, (off_t)r.mmap_offset);
            if (base == MAP_FAILED) {
                ::close(fd);   // not in `next` yet, so its cleanup will not see it
                LOG_ERRNO_RETURN(0, -1, "vhost-user region mmap failed, size `", r.memory_size);
            }
            next.regions.push_back(MemTable::Region{r.guest_phys_addr, r.userspace_addr,
                                                    r.memory_size, (char*)base, fd});
        }

        // the old mappings back the vring HVAs (and possibly in-flight request
        // iovs): stop dispatch, drain, then swap the table and retranslate.
        // was_enabled is per queue, not one flag: a frontend may have enabled only
        // some of the queues, and restoring a single bool would start ones that
        // were never enabled.
        std::vector<bool> was_enabled(nqueues);
        for (uint32_t i = 0; i < nqueues; i++) {
            was_enabled[i] = vqs[i]->x.enabled.load(std::memory_order_relaxed);
            vqs[i]->x.enabled.store(false, std::memory_order_relaxed);
            vq_stop(i);
            vq_drain(i);   // in-flight iovs point into the OLD mappings
        }
        mem.clear();
        mem.regions = std::move(next.regions);
        ok = true;         // cancels the cleanup above, which therefore never
                           // inspects the moved-from vector
        LOG_INFO("vhost-user mem table: ` regions", n);
        for (uint32_t i = 0; i < nqueues; i++) {
            vq_retranslate(i);
            if (was_enabled[i]) {
                vqs[i]->x.enabled.store(true, std::memory_order_relaxed);
                vq_start(i);
            }
        }
        return 0;
    }

    // Returns false when the session ended (disconnect / stopping). `fds` is
    // mutable: a handler that KEEPS an fd marks its slot -1, and msg_loop closes
    // everything still >= 0, so no SCM_RIGHTS fd can be dropped on the floor.
    bool handle_msg(const vhost_user_msg* m, int* fds, int nfds) {
        bool need_reply = (m->flags & VHOST_USER_NEED_REPLY_MASK) &&
                          (proto_features & (1ULL << VHOST_USER_PROTOCOL_F_REPLY_ACK));
        uint64_t ack = 0;   // REPLY_ACK payload: 0 ok, !=0 error
        switch (m->request) {
        case VHOST_USER_GET_FEATURES:
            if (reply(conn_fd, m->request, offer_features) < 0) return false;
            return true;
        case VHOST_USER_SET_FEATURES: {
            const uint64_t f = m->payload.u64;
            // A bit this device never offered is a claim about behaviour it does
            // not implement, and the cost is concrete rather than tidy: FLUSH
            // accepted without being offered clears write_through below, so writes
            // stop being synced on the strength of a flush this backend was never
            // asked to support. Refused rather than masked off -- masking would
            // settle a different word than the peer asked for and leave it no way
            // to learn what we settled on.
            if (f & ~offer_features) {
                LOG_ERROR("vhost-user SET_FEATURES refused: bits ` are outside the offer `",
                          HEX(f & ~offer_features), HEX(offer_features));
                ack = 1;
                break;
            }
            negotiated = f;
            // The descriptor's copy of the same word, so that get_info() reports what
            // the peer accepted rather than what we offered. Written here and nowhere
            // else: this is the only place the session learns the settled features.
            cfg.info.negotiated = (f & (1ULL << VIRTIO_BLK_F_FLUSH)) ? FEATURE_FLUSH : 0;
            // From the NEGOTIATED word, not from offer_features: if the frontend
            // masked bit 29 off we must keep the flags semantics. Deciding from
            // our own offer would have us read a used_event nobody wrote.
            // A store, not an assign: should_notify reads event_idx from the
            // serving side, this handler writes it from the control plane.
            for (uint32_t i = 0; i < nqueues; i++) {
                vqs[i]->srv.event_idx.store(!!(negotiated & (1ULL << VIRTIO_RING_F_EVENT_IDX)),
                                            std::memory_order_relaxed);
                // FLUSH absent from the negotiated word leaves the frontend with
                // no command that asks for persistence, so a write cannot be
                // completed on the strength of a FLUSH that may never come.
                // Offered is not enough -- this reads the word the peer accepted,
                // which is the one that says whether it will send FLUSH.
                vqs[i]->srv.write_through.store(!(negotiated & (1ULL << VIRTIO_BLK_F_FLUSH)),
                                                std::memory_order_relaxed);
            }
            LOG_INFO("vhost-user negotiated features ", HEX(negotiated));
            // Ring enablement is tied to this bit: SET_VRING_ENABLE "should be sent
            // only when VHOST_USER_F_PROTOCOL_FEATURES has been negotiated", and a
            // SET_FEATURES without it means the "back-end must enable all rings
            // immediately". A frontend that declined the bit does not send the
            // enable, the "should be sent only when" above being what stops it, so
            // waiting for one leaves every request sitting in an avail ring nobody
            // drains -- and the session gives no sign anything is wrong, because
            // every control message it did send was answered normally.
            // Enabled here rather than at construction: this is the message that
            // tells us which of the two regimes the peer is in. vq_start is a no-op
            // until the ring is actually configured, and the later SET_VRING_*
            // handlers call it again.
            if (!(negotiated & (1ULL << VHOST_USER_F_PROTOCOL_FEATURES))) {
                for (uint32_t i = 0; i < nqueues; i++) {
                    vqs[i]->x.enabled.store(true, std::memory_order_relaxed);
                    vq_start(i);
                }
                LOG_INFO("vhost-user protocol features not negotiated, every ring enabled");
            }
            break;
        }
        case VHOST_USER_GET_PROTOCOL_FEATURES: {
            uint64_t pf = (1ULL << VHOST_USER_PROTOCOL_F_REPLY_ACK) |
                          (1ULL << VHOST_USER_PROTOCOL_F_BACKEND_REQ) |
                          (1ULL << VHOST_USER_PROTOCOL_F_CONFIG);
            // The same gate as the device-level VIRTIO_BLK_F_MQ offer, and the two
            // have to move together: this bit is what lets the primary ask how many
            // queues there are, that one is what tells the guest driver to use them.
            // Either offered alone is a device that advertises a count nobody can
            // act on. nqueues is fixed at construction, so the answer is stable
            // across a reconnect.
            if (nqueues >= 2)
                pf |= (1ULL << VHOST_USER_PROTOCOL_F_MQ);
            if (reply(conn_fd, m->request, pf) < 0)
                return false;
            return true;
        }
        case VHOST_USER_SET_PROTOCOL_FEATURES:
            proto_features = m->payload.u64;
            // REPLY_ACK just took effect -- honor it for THIS message too
            // (need_reply was computed before the switch, when the feature
            // was not yet negotiated; frontends differ on whether they ask)
            if (m->flags & VHOST_USER_NEED_REPLY_MASK) {
                if (reply(conn_fd, m->request, 0) < 0) return false;
                return true;
            }
            break;
        case VHOST_USER_SET_OWNER:
            break;
        case VHOST_USER_RESET_OWNER:
            // Deprecated, and the recommendation is explicit: a back-end should
            // "either ignore this message, or use it to disable all rings". This
            // device ignores it, and that is a choice rather than an omission --
            // the other reading is unavailable to a frontend that never negotiated
            // bit 30: such a frontend is not supposed to send SET_VRING_ENABLE at
            // all, so it has no protocol-legal way to re-enable a ring, and
            // disabling here would leave it unable to recover except by breaking
            // the protocol itself. The spec also records that the ambiguity arose
            // from back-ends that discarded connection state on this message; that
            // is a third reading and must not be inferred from an empty arm.
            break;
        case VHOST_USER_RESET_DEVICE:
            // "Only valid if the VHOST_USER_PROTOCOL_F_RESET_DEVICE protocol feature
            // is set by the back-end", and GET_PROTOCOL_FEATURES does not set it.
            // Acking success would promise that every ring is disabled and all
            // internal state is back to initial, none of which happens, and a peer
            // that believed it would reinitialize a device still mid-session. If
            // that protocol bit is ever advertised, this arm has to become the real
            // reset rather than a refusal.
            LOG_ERROR("vhost-user RESET_DEVICE refused: the reset protocol feature is not advertised");
            ack = 1;
            break;
        case VHOST_USER_GET_QUEUE_NUM:
            if (reply(conn_fd, m->request, nqueues) < 0) return false;
            return true;
        case VHOST_USER_SET_MEM_TABLE:
            if (handle_mem_table(m, fds, nfds) < 0) ack = 1;
            break;
        case VHOST_USER_SET_VRING_NUM: {
            const uint32_t idx = m->payload.state.index;
            if (idx >= nqueues) {
                LOG_ERROR("vhost-user SET_VRING_NUM rejected: vring index ` of ` queues",
                          idx, nqueues);
                ack = 1;
                break;
            }
            uint32_t n = m->payload.state.num;
            // num is a modulo divisor in dispatch_avail and in
            // vring_used_append, and it bounds the in-flight coroutine cap from
            // above, so it is checked here instead of trusted: 65536 used to sail
            // through vq_may_dispatch() (which rejects only 0) and then divide by
            // zero on the first completion -- SIGFPE, whole process down.
            if (n < 2 || n > MAX_VRING_NUM || (n & (n - 1))) {
                LOG_ERROR("vhost-user SET_VRING_NUM rejected: num `, need a power of two in [2, `]",
                          n, MAX_VRING_NUM);
                ack = 1;
                break;
            }
            // num and the three vring pointers are published as a group and the
            // loop reads all four without a lock, so this changes them with the
            // queue quiesced and then puts it back the way it found it: `enabled`
            // is the frontend's state, and a frontend that had the queue running
            // expects it to still be running after a resize. A rejected num never
            // gets this far, so a bad message cannot stop a live queue.
            auto* q = vqs[idx];
            bool was_enabled = q->x.enabled.load(std::memory_order_relaxed);
            vq_stop(idx);
            vq_drain(idx);
            q->srv.num = n;
            // The stored vring HVAs were validated against the OLD num's lengths,
            // and qva2va proves only that the declared length fits a region -- the
            // returned pointer is then indexed by num. So re-check, but ONLY when
            // this NUM resizes an already-live (translated) vring; a num too large
            // for the region then clears addr_set and dispatch stops instead of
            // running off the end. Tell the frontend: a spec-valid num the region
            // cannot hold is still a rejection, and without the ack it believes the
            // queue was resized.
            //
            // Gate on the incoming addr_set, NOT on desc_qva != 0, to cover both
            // non-live cases. Fresh session: QEMU sends NUM before ADDR (MEM_TABLE
            // -> NUM -> BASE -> ADDR -> KICK), so addr_set is still false at the
            // first NUM and there is nothing to fit yet -- SET_VRING_ADDR runs the
            // same check, with a correct message, once the addresses land. Frontend
            // RECONNECT: msg_loop tears down with vq_stop, not vq_reset, so
            // desc_qva survives from the previous frontend and the new session's
            // SET_MEM_TABLE re-translates that stale QVA against the new region,
            // fails, and leaves addr_set false -- a desc_qva != 0 gate would then
            // reject the reconnect's protocol-legal NUM with this wrong-cause ack.
            // A live resize has addr_set true on entry, so it re-checks as before.
            bool had_vring = q->x.addr_set;
            vq_retranslate(idx);
            if (had_vring && !q->x.addr_set) {
                LOG_ERROR("vhost-user SET_VRING_NUM ` does not fit the declared region", n);
                ack = 1;
            }
            q->x.enabled.store(was_enabled, std::memory_order_relaxed);
            vq_start(idx);
            break;
        }
        case VHOST_USER_SET_VRING_ADDR: {
            vhost_vring_addr a;   // memcpy out, per the access rule on vhost_user_msg
            memcpy(&a, &m->payload.addr, sizeof(a));
            const uint32_t idx = a.index;
            if (idx >= nqueues) {
                LOG_ERROR("vhost-user SET_VRING_ADDR rejected: vring index ` of ` queues",
                          idx, nqueues);
                ack = 1;
                break;
            }
            // Same group as SET_VRING_NUM, same quiesce: the loop dereferences
            // these three together with num. Same restore of `enabled` -- the
            // frontend may re-address a queue it already has running, and the
            // retranslation below can also fail, in which case vq_start's own
            // readiness gate is what keeps dispatch off the bad addresses.
            auto* q = vqs[idx];
            bool was_enabled = q->x.enabled.load(std::memory_order_relaxed);
            vq_stop(idx);
            vq_drain(idx);
            q->x.desc_qva = a.desc_user_addr;
            q->x.used_qva = a.used_user_addr;
            q->x.avail_qva = a.avail_user_addr;
            vq_retranslate(idx);
            if (!q->x.addr_set) {
                LOG_ERROR("vhost-user vring addr translation failed (SET_MEM_TABLE first?), num `",
                          q->srv.num);
                ack = 1;
            }
            q->x.enabled.store(was_enabled, std::memory_order_relaxed);
            vq_start(idx);
            break;
        }
        case VHOST_USER_SET_VRING_BASE: {
            const uint32_t idx = m->payload.state.index;
            if (idx >= nqueues) {
                LOG_ERROR("vhost-user SET_VRING_BASE rejected: vring index ` of ` queues",
                          idx, nqueues);
                ack = 1;
                break;
            }
            auto* q = vqs[idx];
            // The frontend is supposed to have stopped this vq before it publishes
            // a base for it, so in the normal sequence both calls below are no-ops.
            // They are not redundant: last_avail is a plain field that
            // dispatch_avail advances on the queue's own vcpu, so writing it from
            // here is only safe with that loop joined, and a frontend that skips
            // the stop is exactly the case the quiesce has to cover.
            //
            // Deliberately NOT restarted afterwards: vq_start re-derives last_avail
            // from the used ring, which would throw away the base just written. A
            // frontend that broke the stop-first rule is left with a stopped queue
            // -- the state it claimed to be in -- and SET_VRING_ENABLE brings it
            // back.
            vq_stop(idx);
            vq_drain(idx);
            q->srv.last_avail = (uint16_t)m->payload.state.num;
            // vq_start() publishes this invariant too, but it early-returns while
            // the queue is not enabled -- the normal state here, BASE being part of
            // setup -- so publish it explicitly. Guarded on addr_set because BASE
            // may legitimately precede SET_VRING_ADDR, and used is still null then.
            if (q->x.addr_set)
                q->srv.publish_avail_event();
            break;
        }
        case VHOST_USER_GET_VRING_BASE: {
            const uint32_t idx = m->payload.state.index;
            if (idx >= nqueues) {
                LOG_ERROR("vhost-user GET_VRING_BASE rejected: vring index ` of ` queues",
                          idx, nqueues);
                ack = 1;
                break;
            }
            // let dispatched requests complete FIRST: replying with a
            // last_avail that outruns the used ring would drop them when the
            // frontend resumes the vq elsewhere (their completions never land)
            vq_drain(idx);
            vq_stop(idx);
            // read after the join: last_avail belongs to the loop's vcpu
            vhost_vring_state s{m->payload.state.index, vqs[idx]->srv.last_avail};
            if (reply_blob(conn_fd, m->request, &s, sizeof(s)) < 0) return false;
            return true;
        }
        case VHOST_USER_SET_VRING_KICK:
        case VHOST_USER_SET_VRING_CALL: {
            uint64_t u = m->payload.u64;
            // low 8 bits are the vring index; bit 8 is the "no fd" flag
            const uint32_t idx = (uint32_t)(u & 0xff);
            if (idx >= nqueues) {
                LOG_ERROR("vhost-user vring fd rejected: index ` of ` queues", idx, nqueues);
                ack = 1;
                break;   // msg_loop's DEFER still closes the fd we did not take
            }
            auto* q = vqs[idx];
            bool is_kick = m->request == VHOST_USER_SET_VRING_KICK;
            // The loop waits on kickfd and drains it with a bare read, and every
            // completion writes callfd -- both from the queue's own vcpu. A
            // descriptor we have just closed can be handed straight back out for
            // something else, so swapping these under a running loop risks doing
            // IO on a recycled fd number: quiesce first. `enabled` is restored and
            // the queue restarted because a KICK or a CALL on a live queue (the
            // NOFD revoke, a frontend reconnect) must leave it live; during setup
            // `was_enabled` is false and vq_start's own gate keeps it that way.
            bool was_enabled = q->x.enabled.load(std::memory_order_relaxed);
            vq_stop(idx);
            vq_drain(idx);
            int* slot = is_kick ? &q->srv.kickfd : &q->x.callfd;
            if (*slot >= 0) { ::close(*slot); *slot = -1; }
            if (!(u & VHOST_USER_VRING_NOFD_MASK) && nfds > 0) {
                *slot = fds[0];
                fds[0] = -1;   // ours now
            }
            q->x.enabled.store(was_enabled, std::memory_order_relaxed);
            if (!is_kick && q->x.callfd >= 0) {
                // Signal once on installing the callfd: a frontend that
                // reconnects may be blocked on an interrupt for completions whose
                // notification died with the old connection, and one spurious
                // signal makes it re-poll the used ring. Explicitly permitted --
                // virtio 1.2 §2.7.7.1 requires the driver to handle spurious
                // notifications from the device -- and harmless by construction:
                // it re-reads used->idx and finds nothing new. QEMU's
                // vhost-user-blk idx test waits for exactly this ISR before it
                // sends its first request.
                vq_notify(idx);
            }
            vq_start(idx);
            break;
        }
        case VHOST_USER_SET_VRING_ENABLE: {
            const uint32_t idx = m->payload.state.index;
            if (idx >= nqueues) {
                LOG_ERROR("vhost-user SET_VRING_ENABLE rejected: vring index ` of ` queues",
                          idx, nqueues);
                ack = 1;
                break;
            }
            if (m->payload.state.num) {
                // A preceding disable only joins the loop; the requests it had
                // already dispatched are still completing into the used ring that
                // vq_start re-reads and republishes. Wait them out first so the
                // two never overlap.
                vq_drain(idx);
                vqs[idx]->x.enabled.store(true, std::memory_order_relaxed);
                vq_start(idx);
            } else {
                vq_stop(idx);
            }
            break;
        }
        case VHOST_USER_GET_CONFIG: {
            vhost_user_config c;
            memset(&c, 0, sizeof(c));
            uint32_t off = m->payload.config.offset;
            uint32_t sz = m->payload.config.size;
            c.offset = off;
            // 64-bit on purpose: off + sz wraps in uint32 (off=1, size=~0u sums to
            // 0 and would pass), and region[] is smaller than dev_config anyway.
            // The peer is remote, so both numbers are hostile input.
            if ((uint64_t)off + sz > sizeof(dev_config) || sz > sizeof(c.region)) {
                LOG_WARN("vhost-user GET_CONFIG out of range: off ` size `", off, sz);
                sz = 0;   // hand back nothing; the reply length then cannot wrap
            }
            c.size = sz;
            if (sz)
                memcpy(c.region, dev_config + off, sz);
            if (reply_blob(conn_fd, m->request, &c,
                           (uint32_t)(offsetof(vhost_user_config, region) + sz)) < 0)
                return false;
            return true;
        }
        case VHOST_USER_SET_CONFIG:
            break;   // the device config is read-only from the driver side
        case VHOST_USER_SET_BACKEND_REQ_FD:
            if (backend_req_fd >= 0) ::close(backend_req_fd);
            backend_req_fd = nfds > 0 ? fds[0] : -1;
            if (nfds > 0) fds[0] = -1;   // ours now
            break;
        case VHOST_USER_SET_LOG_BASE:
        case VHOST_USER_SET_LOG_FD:
        case VHOST_USER_SEND_RARP:
        case VHOST_USER_SET_VRING_ERR:
        case VHOST_USER_SET_VRING_ENDIAN:
        case VHOST_USER_VRING_KICK:
        case VHOST_USER_GET_STATUS:
        case VHOST_USER_SET_STATUS:
            break;   // not negotiated / tolerated no-ops
        default:
            LOG_WARN("vhost-user unknown request `, acking", m->request);
            break;
        }
        if (need_reply && reply(conn_fd, m->request, ack) < 0)
            return false;
        return true;
    }

    void msg_loop() {
        vhost_user_msg m;
        int fds[VHU_MSG_MAX_FDS];
        while (!stopping) {
            int nfds = 0;
            if (recv_msg(conn_fd, &m, fds, &nfds) < 0) {
                if (!stopping)
                    LOG_INFO("vhost-user frontend disconnected");
                break;
            }
            if ((m.flags & VHOST_USER_VERSION_MASK) != VHOST_USER_VERSION)
                LOG_WARN("vhost-user message version `, expected 1", m.flags & VHOST_USER_VERSION_MASK);
            // recvmsg put these in our fd table, so they are ours from here on.
            // Harden them ALL here rather than at each consumer, so no handler
            // can forget: -1 in a slot is what every handler already reads as
            // "no fd offered", so a failure degrades to not using that channel
            // instead of risking a blocking syscall on the serving vcpu.
            //
            // Then the ownership rule: a handler that keeps one marks its slot
            // -1 and we close the rest. Without this, every message we reject,
            // tolerate as a no-op or do not recognise leaked its fds, and a
            // frontend could exhaust our fd table by attaching one to each.
            for (int i = 0; i < nfds; i++)
                if (harden_recv_fd(fds[i]) < 0) {
                    ::close(fds[i]);
                    fds[i] = -1;
                }
            DEFER(for (int i = 0; i < nfds; i++)
                      if (fds[i] >= 0) { ::close(fds[i]); fds[i] = -1; });
            if (!handle_msg(&m, fds, nfds))
                break;
        }
        // Session over. WHO tears the queues down depends on why we are here:
        //
        // A disconnect (or a handler that ended the session) leaves the listener
        // up for the frontend's reconnect (QEMU reconnect=on drives the
        // recovery), and a reconnect must start from stopped queues -- so this
        // is that teardown. The requests already out are NOT waited for here:
        // every path that unmaps the memory table they hold iovs into
        // (handle_mem_table, stop_session, rollback) drains them itself first.
        //
        // `stopping` means stop_session or rollback is what ended us, and both
        // stop and drain every queue themselves -- deliberately AFTER joining
        // this loop, so that no SET_VRING_* can swap the ring out from under
        // them. Stopping the queues here as well would leave them with no loop
        // coroutine by the time stop_session's backlog wait runs, and that wait
        // counts on the loop to keep consuming avail entries: it would return
        // at once and detach(true) would stop honouring its wait_pending
        // contract. So leave them running and let the caller quiesce them.
        if (!stopping)
            for (uint32_t i = 0; i < nqueues; i++)
                vq_stop(i);
    }

    // ----- connection setup -----

    int do_listen() {
        // The identity lock, taken by EVERY server start -- including the ones that
        // go on to bind a path nothing held. A lock that only the takeover path
        // took would not be held by the process that WON the path, so a loser would
        // still be free to read that node as dead and remove it.
        //
        // Held until listen() has succeeded, not merely until bind() has: a socket
        // that is bound but not yet listening answers the probe with ECONNREFUSED,
        // which is indistinguishable from a listener that died. EBUSY here is the
        // same refusal blk.h documents for a live identity -- another daemon is
        // between claiming this name and listening on it.
        char lname[VHU_LOCK_BUF];
        if (vhu_lock_name(sock_path, lname, sizeof(lname)) < 0)
            return -1;   // vhu_lock_name logged it
        int lock_fd = -1;
        if (devlock_acquire(lock_dir, lname, &lock_fd) < 0)
            LOG_ERRNO_RETURN(0, -1, "vhost-user cannot claim the socket ", sock_path);
        DEFER(devlock_release(lock_fd));

        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            LOG_ERRNO_RETURN(0, -1, "vhost-user socket failed");
        bool owned = false;
        DEFER({ if (!owned) ::close(fd); });
        sockaddr_un un;
        memset(&un, 0, sizeof(un));
        un.sun_family = AF_UNIX;
        memcpy(un.sun_path, sock_path, strlen(sock_path) + 1);   // start() bounded it
        // Bind BEFORE asking whether the path is ours, because bind() is the one
        // atomic step available here. When it succeeds nothing was probed and
        // nothing was removed, which is the whole of the absent-path case and the
        // common one. Only a path something else already holds answers EADDRINUSE,
        // and only then does this have to decide whether that node may be taken
        // over at all.
        if (::bind(fd, (sockaddr*)&un, sizeof(un)) < 0) {
            if (errno != EADDRINUSE)
                LOG_ERRNO_RETURN(0, -1, "vhost-user bind failed: ", sock_path);
            // blk.h start() contract: EBUSY when another live process is serving
            // this identity. Do NOT steal a live backend's socket path -- it keeps
            // serving the orphaned inode while new frontends come to us. A verdict
            // that cannot be reached is refused the same way, and so is a node that
            // is not a socket: neither is evidence that this path is ours to take.
            int r = unix_endpoint_replaceable(sock_path);
            if (r < 0)
                LOG_ERRNO_RETURN(0, -1, "vhost-user reached no verdict on the socket ", sock_path);
            if (r == 0)
                LOG_ERRNO_RETURN(0, -1, "vhost-user refuses to take over the socket ", sock_path);
            // A socket node with no listener behind it. ENOENT here is a race --
            // something else removed it since the verdict -- and is the state the
            // bind below wants anyway.
            if (::unlink(sock_path) != 0 && errno != ENOENT)
                LOG_ERRNO_RETURN(0, -1, "failed to remove the stale vhost-user socket ", sock_path);
            if (::bind(fd, (sockaddr*)&un, sizeof(un)) < 0)
                LOG_ERRNO_RETURN(0, -1, "vhost-user bind failed after clearing ", sock_path);
        }
        if (::listen(fd, 1) < 0)
            LOG_ERRNO_RETURN(0, -1, "vhost-user listen failed: ", sock_path);
        // bind() created the node with 0777 & ~umask; chmod it to what the caller
        // asked for. Not derived from umask: that has no read-only query, and the
        // set-and-restore which emulates one changes the mask of the whole process
        // rather than this thread, so any file another thread creates in the window
        // is unmasked.
        if (::chmod(sock_path, (mode_t)cfg.sock_mode) < 0)
            LOG_WARN("vhost-user chmod failed on `, ", sock_path, ERRNO());
        owned = true;
        listen_fd = fd;
        return 0;
    }

    int do_connect() {
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            LOG_ERRNO_RETURN(0, -1, "vhost-user socket failed");
        sockaddr_un un;
        memset(&un, 0, sizeof(un));
        un.sun_family = AF_UNIX;
        memcpy(un.sun_path, sock_path, strlen(sock_path) + 1);   // start() bounded it
        if (::connect(fd, (sockaddr*)&un, sizeof(un)) < 0) {
            if (errno != EINPROGRESS && errno != EAGAIN) {
                ::close(fd);
                LOG_ERRNO_RETURN(0, -1, "vhost-user connect failed: ", sock_path);
            }
            if (photon::wait_for_fd_writable(fd, Timeout(10ull * 1000 * 1000)) < 0) {
                ::close(fd);
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "vhost-user connect timed out: `", sock_path);
            }
            int err = 0;
            socklen_t el = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) {
                ::close(fd);
                LOG_ERROR_RETURN(err, -1, "vhost-user connect failed: ", sock_path);
            }
        }
        conn_fd = fd;
        return 0;
    }

    void accept_loop() {
        while (!stopping) {
            if (photon::wait_for_fd_readable(listen_fd) < 0) {
                if (stopping) break;
                if (errno == EINTR) continue;
                LOG_ERRNO_RETURN(0, , "vhost-user accept wait failed");
            }
            int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                LOG_ERRNO_RETURN(0, , "vhost-user accept failed");
            }
            // conn_fd is always -1 here: this loop is the only writer of it in
            // the SERVER role, and the close below runs before we come back
            conn_fd = fd;
            LOG_INFO("vhost-user frontend connected on ", sock_path);
            // run the session inline; when it ends, loop back and accept again
            // (QEMU reconnect=on drives the recovery)
            msg_loop();
            if (conn_fd >= 0) { ::close(conn_fd); conn_fd = -1; }
        }
    }

    // ----- lifecycle -----

    void fill_config() {
        memset(dev_config, 0, sizeof(dev_config));
        auto* bc = (virtio_blk_config*)dev_config;
        bc->capacity = capacity_sectors;
        bc->blk_size = 1u << sector_shift;
        bc->num_queues = (uint16_t)nqueues;
    }

    int start(fs::IFile* bk, bool ownership) override {
        if (started)
            LOG_ERROR_RETURN(EALREADY, -1, "vhost-user device already started");
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

        stopping = false;
        // After the DEFER, so a rejected pool unwinds through the same rollback as
        // every other start() failure, and before anything is bound or listened on.
        if (check_pool_engines(cfg.pool) < 0)
            return -1;
        for (uint32_t i = 0; i < nqueues; i++) {
            vq_bind(i);
            vqs[i]->srv.stopping.store(false, std::memory_order_relaxed);
        }
        if (cfg.sock_role == VhostUserController::SockRole::SERVER) {
            if (do_listen() < 0)
                return -1;
            accept_th = photon::thread_create11(&VhostUserDeviceImpl::accept_loop, this);
            photon::thread_enable_join(accept_th);
        } else {
            if (do_connect() < 0)
                return -1;
            msg_th = photon::thread_create11(&VhostUserDeviceImpl::msg_loop, this);
            photon::thread_enable_join(msg_th);
        }
        started = true;
        ok = true;
        LOG_INFO("vhost-user device started, ",
                 make_named_value("sock_path", (const char*)sock_path),
                 "role=", (int)cfg.sock_role, VALUE(cfg.info.size), HEX(offer_features));
        return 0;
    }

    // close every queue's fds and reset it for a future session (srv holds
    // atomics, so no wholesale assignment)
    void vq_reset() {
        // Clear the negotiated features: after reset/shutdown the engine returns
        // to write-through policy, and without this a caller observing get_info()
        // between sessions sees the previous frontend's negotiated word.
        // SET_FEATURES re-populates it when the new frontend connects.
        cfg.info.negotiated = 0;
        for (uint32_t i = 0; i < nqueues; i++) {
            auto* q = vqs[i];
            vq_stop(i);
            if (q->srv.kickfd >= 0) { ::close(q->srv.kickfd); q->srv.kickfd = -1; }
            if (q->x.callfd >= 0) { ::close(q->x.callfd); q->x.callfd = -1; }
            q->srv.num = 0;
            q->x.desc_qva = q->x.used_qva = q->x.avail_qva = 0;
            vq_invalidate(i);
            q->srv.last_avail = 0;
            q->srv.used_idx = 0;
            // SET_FEATURES arrives every session and resets event_idx, but leaving a
            // true value on the reset path is a hazard; leaving notify_valid true is
            // worse -- it would cost the first completion after a reset its
            // unconditional notification.
            q->srv.event_idx.store(false, std::memory_order_relaxed);
            q->srv.notify_valid.store(false, std::memory_order_relaxed);
            // write_through goes the other way, back to the engine's default: with
            // no negotiated word in hand, the safe assumption is that no FLUSH will
            // arrive. Being wrong here costs a sync per write until SET_FEATURES
            // re-derives it; defaulting the other way would cost durability.
            q->srv.write_through.store(true, std::memory_order_relaxed);
            q->x.enabled.store(false, std::memory_order_relaxed);
        }
    }

    // stop serving + disconnect; keep listening state consistent with `role`
    void stop_session(bool drain_backlog) {
        if (!drain_backlog)
            for (auto* q : vqs)
                q->x.enabled.store(false, std::memory_order_relaxed);
        // Device-level `stopping`, and it has to be set BEFORE the two joins
        // below: msg_loop's and accept_loop's conditions are `while (!stopping)`,
        // and an interrupted accept_loop that still finds stopping false takes the
        // EINTR branch and loops again -- so without this the join never returns.
        // Both loops stay on this vcpu, which is why `stopping` needs no atomic.
        stopping = true;
        if (accept_th) {
            photon::thread_interrupt(accept_th);
            photon::thread_join((photon::join_handle*)accept_th);
            accept_th = nullptr;
        }
        if (msg_th) {
            photon::thread_interrupt(msg_th);
            photon::thread_join((photon::join_handle*)msg_th);
            msg_th = nullptr;
        }
        for (uint32_t i = 0; i < nqueues; i++) {
            // The backlog wait comes first and the engine-level `stopping` second,
            // and that order is load-bearing: the wait expects the loop to keep
            // consuming avail entries, and `stopping` is exactly what tells it to
            // leave them alone. Setting the flag first DEADLOCKS, and nothing
            // times out to break it: `stopping` is the very flag that makes the
            // loop return, while this wait counts on that same loop to keep
            // advancing last_avail until it catches the avail idx, and the wait
            // itself has no bound -- so detach(true) never returns.
            //
            // Both joins above are what make the wait possible AND safe. Possible:
            // msg_loop's own exit path stops the queues when a disconnect ends the
            // session, but not when `stopping` does -- see the comment there -- so
            // the loop this wait needs is still alive precisely because we came in
            // through `stopping`. Safe: while msg_loop lived it could process a
            // SET_VRING_* in the middle of the drain and swap the ring out from
            // under it.
            vq_backlog_drain(i, drain_backlog);
            // from here the engine leaves in-flight requests uncompleted
            vqs[i]->srv.stopping.store(true, std::memory_order_relaxed);
            vq_stop(i);   // join the vq loop: no further dispatch
            // the backlog wait cannot see a batch dispatched in the window before
            // `stopping` took effect: those request coroutines hold VAs into the
            // memory table, so wait for them before vq_reset/mem.clear unmapping
            // under them (use-after-free)
            vq_drain(i);
        }
        vq_reset();
        mem.clear();
        if (conn_fd >= 0) { ::close(conn_fd); conn_fd = -1; }
        if (backend_req_fd >= 0) { ::close(backend_req_fd); backend_req_fd = -1; }
    }

    int detach(bool wait_pending) override {
        if (!started)
            return 0;
        stop_session(wait_pending);
        if (listen_fd >= 0) { ::close(listen_fd); listen_fd = -1; }
        started = false;
        LOG_INFO("vhost-user device detached, ",
                 make_named_value("sock_path", (const char*)sock_path),
                 "flush=", (int)wait_pending);
        return 0;
    }

    int shutdown() override {
        if (started) {
            detach(true);
            if (cfg.sock_role == VhostUserController::SockRole::SERVER)
                ::unlink(sock_path);   // remove the tombstone; a CLIENT frontend
                                       // owns its socket and is left alone
            LOG_INFO("vhost-user device shut down, ",
                     make_named_value("sock_path", (const char*)sock_path));
        }
        // blk.h's start() contract: an owned backend is deleted on shutdown, not
        // only by the destructor. The object outlives a shutdown() and the next
        // start() overwrites the pointer, so releasing it only at destruction leaks
        // the first backend. Outside the `started` test on purpose: a detach()
        // leaves the backend held while the device is not started, and shutdown()
        // is still the call that ends the session. detach(true) has joined every
        // serving coroutine by now, so nothing can touch it again.
        release_backend();
        return 0;
    }

    int resize(uint64_t new_size) override {
        if (!started)
            LOG_ERROR_RETURN(ENODEV, -1, "vhost-user resize: not started");
        if (new_size % 512)
            LOG_ERROR_RETURN(EINVAL, -1, "resize size ` is not a multiple of 512", new_size);
        uint64_t cur = capacity_sectors << 9;
        if (new_size == cur)
            return 0;
        if (new_size < cur)
            LOG_ERROR_RETURN(EINVAL, -1, "vhost-user resize: shrink (` -> `) is rejected",
                             cur, new_size);
        capacity_sectors = new_size >> 9;
        // serve_chain's LBA bound must grow with us. A store, not an assign:
        // serve_chain reads it from the serving side while resize() runs on the
        // control plane -- the same reason tcmu's dev_size/num_lbas are atomic.
        for (uint32_t i = 0; i < nqueues; i++)
            vqs[i]->srv.capacity.store(new_size, std::memory_order_relaxed);
        cfg.info.size = new_size;
        fill_config();
        if (backend_req_fd >= 0) {   // announce: the frontend re-reads GET_CONFIG
            vhost_user_msg m;
            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_BACKEND_CONFIG_CHANGE_MSG;
            m.flags = VHOST_USER_VERSION;
            m.size = 0;
            if (send_msg(backend_req_fd, &m) < 0)
                LOG_WARN("vhost-user config-change notification failed (the frontend picks the new capacity up at the next GET_CONFIG)");
        } else {
            LOG_WARN("vhost-user resize without a backend channel: the frontend sees the new capacity at its next GET_CONFIG");
        }
        LOG_INFO("vhost-user device resized, ",
                 make_named_value("sock_path", (const char*)sock_path),
                 VALUE(cur), VALUE(new_size));
        return 0;
    }

    void rollback() {
        stopping = true;
        if (accept_th) {
            photon::thread_interrupt(accept_th);
            photon::thread_join((photon::join_handle*)accept_th);
            accept_th = nullptr;
        }
        if (msg_th) {
            photon::thread_interrupt(msg_th);
            photon::thread_join((photon::join_handle*)msg_th);
            msg_th = nullptr;
        }
        // Same order as stop_session(): the request coroutines hold VAs into the
        // memory table, so mem.clear() must not run until the loop has stopped
        // dispatching and the ones already out have finished. Both are no-ops on
        // the common rollback path (nothing was ever started).
        for (uint32_t i = 0; i < nqueues; i++) {
            vq_stop(i);
            vq_drain(i);
        }
        vq_reset();
        mem.clear();
        if (conn_fd >= 0) { ::close(conn_fd); conn_fd = -1; }
        if (backend_req_fd >= 0) { ::close(backend_req_fd); backend_req_fd = -1; }
        if (listen_fd >= 0) {
            ::close(listen_fd);
            listen_fd = -1;
            // only OUR listener's socket file: when do_listen refused the path or
            // failed to bind it, listen_fd is still -1 and the node there belongs
            // to someone else -- do not unlink it (same invariant as ublk's
            // rollback-vs-DEL_DEV)
            if (cfg.sock_role == VhostUserController::SockRole::SERVER)
                ::unlink(sock_path);
        }
        // virgin state includes backend ownership (the cross-transport rule).
        // sock_path is NOT part of it: it is the identity, fixed at construction.
        backend = nullptr;
        own_backend = false;
        started = false;
    }
};

struct VhostUserControllerImpl : VhostUserController {
    char sock_dir[SCOPE_DIR_BUF] = {};   // never empty: the factory rejects that

    explicit VhostUserControllerImpl(const char* d) {
        snprintf(sock_dir, sizeof(sock_dir), "%s", d);   // bounded: the factory checked
    }

    // Is path's directory dir? Textual, with trailing slashes stripped from both
    // sides, and deliberately NOT resolved: realpath would be I/O in a factory,
    // which must not block (see blk.h). By the same dirname semantics a path that
    // IS the directory ("/a/b/") counts as inside it -- not an oversight: such a
    // sock_path cannot silently pass, start() would fail to bind a directory.
    static bool inside(const char* dir, const char* path) {
        size_t dl = strlen(dir);
        while (dl > 1 && dir[dl - 1] == '/')
            dl--;
        const char* slash = strrchr(path, '/');
        if (!slash)
            return false;   // a bare filename has no directory to be inside of
        size_t pl = (size_t)(slash - path);
        while (pl > 1 && path[pl - 1] == '/')
            pl--;
        if (dl == 1 && dir[0] == '/')
            return pl == 0;   // dir is "/": only a path with no further slash is in it
        return pl == dl && strncmp(dir, path, dl) == 0;
    }

    IBlkDevice* new_device(const VhostUserController::Config& cfg) override {
        if (VhostUserDeviceImpl::validate(cfg) < 0)
            return nullptr;
        // the one coupling this controller exists to state: we listen where we scan
        if (!inside(sock_dir, cfg.sock_path.c_str()))
            LOG_ERROR_RETURN(EINVAL, nullptr, "socket ` is not inside this controller's directory ",
                             cfg.sock_path.c_str(), sock_dir);
        return new VhostUserDeviceImpl(cfg, sock_dir);
    }

    // Orphan scan: a vhost-user tombstone is a socket file in our directory whose
    // listener is gone -- the connect probe gets ECONNREFUSED. A successful connect
    // means a live backend (skip it). Descriptor fields are unspecified (zero):
    // there is no registry to read them from; recovery is a blind re-listen via
    // start().
    std::vector<BlkDevInfo> list_orphans() override {
        std::vector<BlkDevInfo> ret;
        DIR* dd = ::opendir(sock_dir);
        if (!dd)
            return ret;
        DEFER(::closedir(dd));
        struct dirent* e;
        while ((e = readdir(dd))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char path[PATH_MAX];
            if (snprintf(path, sizeof(path), "%s/%s", sock_dir, e->d_name) >= (int)sizeof(path))
                continue;
            struct stat st;
            if (::stat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
                continue;
            size_t plen = strlen(path);
            if (plen >= SUN_PATH_MAX)
                continue;   // not connectable as a unix socket anyway
            // 1 = a live backend (not an orphan), -1 = not a listener tombstone
            // (a socket race, a permission problem, ...): only a dead listener
            // counts, and recovery is a blind re-listen anyway
            if (unix_listener_live(path) != 0)
                continue;
            BlkDevInfo bi;
            bi.identity = path;
            ret.push_back(bi);
        }
        return ret;
    }

    // Remove one orphan. The identity is caller-supplied and names something to
    // delete, so it passes the same containment check new_device() applies to a
    // sock_path: without that check this is an unlink of any path the caller can
    // spell. The S_ISSOCK test is the same idea one level down -- this directory
    // is ours, but it is not empty of everything else either.
    int destroy_orphan(const BlkDevInfo& orphan) override {
        const char* path = orphan.identity.c_str();
        if (!inside(sock_dir, path))
            LOG_ERROR_RETURN(EINVAL, -1, "refusing to destroy `: not inside this controller's directory `",
                             path, sock_dir);
        struct stat st;
        if (::stat(path, &st) != 0)
            LOG_ERRNO_RETURN(0, -1, "cannot stat the vhost-user socket to destroy ", path);
        if (!S_ISSOCK(st.st_mode))
            LOG_ERROR_RETURN(EINVAL, -1, "` is not a socket, so it is not a vhost-user registration", path);
        // A live listener means some server holds this path. list_orphans() never
        // reports one, but the caller's BlkDevInfo can predate another daemon
        // re-binding the path -- the window UblkDeviceImpl::shutdown() closes by
        // re-claiming the flock before it lets DEL_DEV run.
        int live = unix_listener_live(path);
        if (live < 0)
            LOG_ERRNO_RETURN(0, -1, "cannot probe the vhost-user socket ", path);
        if (live == 1)
            LOG_ERROR_RETURN(EBUSY, -1, "vhost-user socket ` still has a live listener; leaving it alone", path);
        // ENOENT is the goal state, reached by a race: someone removed it between
        // the stat above and here.
        if (::unlink(path) != 0 && errno != ENOENT)
            LOG_ERRNO_RETURN(0, -1, "failed to unlink the vhost-user socket ", path);
        return 0;
    }
};

VhostUserController* new_vhost_user_controller(const char* sock_dir) {
    if (validate_scope_dir(sock_dir, "socket") < 0)
        return nullptr;   // already logged
    return new VhostUserControllerImpl(sock_dir);
}

}  // namespace blk
}  // namespace photon
