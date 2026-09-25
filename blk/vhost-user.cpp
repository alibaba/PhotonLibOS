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
// P1 scope: one virtqueue (no F_MQ / VIRTIO_BLK_F_MQ), split ring, no
// indirect descriptors offered (F_RING_INDIRECT_DESC not in our feature set),
// IN/OUT/FLUSH/GET_ID served; FEATURE_DISCARD/WRITE_ZEROES accepted in cfg
// but not offered; serving on the caller's vcpu. The virtio-blk device-model
// core (constants, vring structs, desc-chain walk, request dispatch) is shared
// with the vduse transport via the internal blk/utils.{h,cpp}. All virtio
// fields are little-endian (VERSION_1), LE host assumed.

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

// ----------------------------------------------------------------------------
// vhost-user wire protocol: message ids and the header/payload layout from
// QEMU docs/interop/vhost-user.rst (the protocol's specification document),
// the vhost structures verbatim from <linux/vhost_types.h>
// ----------------------------------------------------------------------------

enum : int32_t {
    VHOST_USER_NONE = 0,
    VHOST_USER_GET_FEATURES = 1,
    VHOST_USER_SET_FEATURES = 2,
    VHOST_USER_SET_OWNER = 3,
    VHOST_USER_RESET_OWNER = 4,
    VHOST_USER_SET_MEM_TABLE = 5,
    VHOST_USER_SET_LOG_BASE = 6,
    VHOST_USER_SET_LOG_FD = 7,
    VHOST_USER_SET_VRING_NUM = 8,
    VHOST_USER_SET_VRING_ADDR = 9,
    VHOST_USER_SET_VRING_BASE = 10,
    VHOST_USER_GET_VRING_BASE = 11,
    VHOST_USER_SET_VRING_KICK = 12,
    VHOST_USER_SET_VRING_CALL = 13,
    VHOST_USER_SET_VRING_ERR = 14,
    VHOST_USER_GET_PROTOCOL_FEATURES = 15,
    VHOST_USER_SET_PROTOCOL_FEATURES = 16,
    VHOST_USER_GET_QUEUE_NUM = 17,
    VHOST_USER_SET_VRING_ENABLE = 18,
    VHOST_USER_SEND_RARP = 19,
    VHOST_USER_NET_SET_MTU = 20,
    VHOST_USER_SET_BACKEND_REQ_FD = 21,
    VHOST_USER_IOTLB_MSG = 22,
    VHOST_USER_SET_VRING_ENDIAN = 23,
    VHOST_USER_GET_CONFIG = 24,
    VHOST_USER_SET_CONFIG = 25,
    VHOST_USER_RESET_DEVICE = 34,
    VHOST_USER_VRING_KICK = 35,
    VHOST_USER_GET_MAX_MEM_SLOTS = 36,
    VHOST_USER_ADD_MEM_REG = 37,
    VHOST_USER_REM_MEM_REG = 38,
    VHOST_USER_SET_STATUS = 39,
    VHOST_USER_GET_STATUS = 40,
    // backend -> frontend, on the SET_BACKEND_REQ_FD channel
    VHOST_USER_BACKEND_CONFIG_CHANGE_MSG = 2,
};

#define VHOST_USER_VERSION          1
#define VHOST_USER_VERSION_MASK     0x3u
#define VHOST_USER_REPLY_MASK       (0x1u << 2)
#define VHOST_USER_NEED_REPLY_MASK  (0x1u << 3)
#define VHOST_USER_VRING_IDX_MASK   0xFFu
#define VHOST_USER_VRING_NOFD_MASK  0x100u

// protocol features we negotiate
#define VHOST_USER_PROTOCOL_F_REPLY_ACK   3
#define VHOST_USER_PROTOCOL_F_BACKEND_REQ 5
#define VHOST_USER_PROTOCOL_F_CONFIG      9

// NOT a protocol feature: bit 30 of the DEVICE feature word, and the gate on the
// whole protocol-feature negotiation. vhost-user.rst defines it, in both its
// GET_FEATURES and SET_FEATURES entries, as the bit that "signals back-end
// support for VHOST_USER_GET_PROTOCOL_FEATURES and
// VHOST_USER_SET_PROTOCOL_FEATURES", and separately requires that on receiving
// SET_FEATURES without it the back-end enable all rings immediately. A
// conformant frontend therefore asks GET_PROTOCOL_FEATURES only if GET_FEATURES
// offered this bit. Omitting it made every protocol feature above unreachable:
// no REPLY_ACK, no GET_CONFIG (the guest never learns the capacity), and no
// SET_BACKEND_REQ_FD channel, so resize() could never announce itself.
#define VHOST_USER_F_PROTOCOL_FEATURES    30

struct vhost_user_memory_region {
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;    // the FRONTEND's VA of the region (QVA space)
    uint64_t mmap_offset;
};
struct vhost_user_memory {
    uint32_t nregions;
    uint32_t padding;
    vhost_user_memory_region regions[8];   // VHOST_MEMORY_BASELINE_NREGIONS
};
struct vhost_vring_state { uint32_t index, num; };
struct vhost_vring_addr {
    uint32_t index;
    uint32_t flags;
    uint64_t desc_user_addr;    // QVAs -- translate via the region table
    uint64_t used_user_addr;
    uint64_t avail_user_addr;
    uint64_t log_guest_addr;
};
struct vhost_user_config {
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
    uint8_t region[256];        // VHOST_USER_MAX_CONFIG_SIZE
};

// The wire header is request(4) + flags(4) + size(4) = 12 bytes and the payload
// follows IMMEDIATELY, with no padding -- that is vhost-user.rst's message
// layout. Unpacked, the union's uint64_t would force 8-byte alignment and pad
// the header to 16 -- 4 bytes of garbage on the wire ahead of every payload, and
// a length check no conforming frontend can satisfy. The nested types keep their
// own layouts, which already match the protocol (vhost_user_memory's padding
// field is part of it), so packing the outer struct is what aligns us.
// sizeof is deliberately NOT asserted: whether packed propagates into the
// anonymous union differs by compiler, while the payload offset does not.
//
// Access rule: reading a SCALAR member in place is fine -- the compiler knows
// the reduced alignment and emits a load to match, which is why handle_msg
// reads payload.u64 / payload.state.num directly. What is NOT fine is taking
// the ADDRESS of a nested struct: that pointer would be under-aligned for its
// type (UB, and -Waddress-of-packed-member). The two handlers that need a whole
// nested struct therefore memcpy it out first and say so at the copy.
struct __attribute__((packed)) vhost_user_msg {
    int32_t request;
    uint32_t flags;
    uint32_t size;
    union {
        uint64_t u64;
        struct vhost_vring_state state;
        struct vhost_vring_addr addr;
        struct vhost_user_memory memory;
        struct vhost_user_config config;
    } payload;
};
static_assert(offsetof(vhost_user_msg, payload) == 12,
              "vhost-user payload must follow the 12-byte header with no padding");

// The multiqueue path reads these two `index` fields off the wire for the first
// time; both were already parsed and then dropped. They keep their own natural
// layout even nested inside the packed vhost_user_msg (packing the outer struct
// does not repack a named nested type), which is what makes the in-place scalar
// reads in handle_msg legal.
static_assert(sizeof(vhost_vring_state) == 8, "vhost_vring_state size");
static_assert(offsetof(vhost_vring_state, index) == 0, "vhost_vring_state index offset");
static_assert(offsetof(vhost_vring_state, num) == 4, "vhost_vring_state num offset");
static_assert(sizeof(vhost_vring_addr) == 40, "vhost_vring_addr size");
static_assert(offsetof(vhost_vring_addr, index) == 0, "vhost_vring_addr index offset");
static_assert(offsetof(vhost_vring_addr, flags) == 4, "vhost_vring_addr flags offset");
static_assert(offsetof(vhost_vring_addr, desc_user_addr) == 8, "vhost_vring_addr desc offset");
static_assert(offsetof(vhost_vring_addr, used_user_addr) == 16, "vhost_vring_addr used offset");
static_assert(offsetof(vhost_vring_addr, avail_user_addr) == 24, "vhost_vring_addr avail offset");

#define VHU_MSG_MAX_FDS 8

// ----------------------------------------------------------------------------

// a unix socket path lives in sockaddr_un::sun_path (108 bytes including the
// NUL) -- the hard bound on cfg.sock_path
static constexpr size_t SUN_PATH_MAX = sizeof(((sockaddr_un*)nullptr)->sun_path);

// Ceiling on a split virtqueue's size: virtio 1.2 §2.7.13 states 32768 as the
// maximum (the highest power of two that fits in 16 bits). Enforced on
// SET_VRING_NUM because the frontend's num reaches two modulo divisors
// (avail->ring[last_avail % num] in dispatch_avail, used->ring[used_idx % num]
// in vring_used_append) and sizes both the vring translation and the in-flight
// coroutine cap. Nonzero is not optional either, for the same reason: a zero
// divisor in either place.
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

struct VhostUserDeviceImpl : IBlkDevice {
    // Field order is padding-driven, do not tidy it: the 8-aligned members come
    // first, with the alignas(8) `dev_config` LAST among them because its 60
    // bytes are not a multiple of 8 and would leave a hole after it; then the
    // 4-, then the 1-byte ones. The previous order scattered 23 bytes across five
    // holes (before listen_fd, offer_features, capacity_sectors, mem and
    // accept_th). 584 bytes vs 608.
    VhostUserController::Config cfg;

    // the shared serving engine (ring state, dispatch, completion, drain);
    // P1 drives a single virtqueue
    VirtQueueServer vq;

    fs::IFile* backend = nullptr;
    uint64_t offer_features = 0;
    uint64_t negotiated = 0;
    uint64_t proto_features = 0;   // the frontend's accepted protocol subset
    uint64_t capacity_sectors = 0;
    photon::thread* accept_th = nullptr;   // SERVER: the accept loop
    photon::thread* msg_th = nullptr;      // the negotiation/message loop
    MemTable mem;
    struct VqVhu {
        uint64_t desc_qva = 0, used_qva = 0, avail_qva = 0;   // retranslate from
                                                             // these on a new
                                                             // memory table
        bool enabled = false;      // the frontend's SET_VRING_ENABLE state
        bool addr_set = false;     // SET_VRING_ADDR translated successfully
        int callfd = -1;           // completion eventfd (owned here)
        photon::thread* th = nullptr;
    } vqx;
    alignas(8) uint8_t dev_config[sizeof(virtio_blk_config)] = {};

    int listen_fd = -1;            // SERVER role
    int conn_fd = -1;              // the live frontend connection
    int backend_req_fd = -1;       // SET_BACKEND_REQ_FD channel (config events)

    // Log this as `(const char*)sock_path`, never as VALUE(sock_path): VALUE on a
    // char array deduces a reference to the whole array and alog then emits all
    // SUN_PATH_MAX bytes, path followed by NUL padding (measured).
    char sock_path[SUN_PATH_MAX] = {};   // bounded by sockaddr_un::sun_path

    bool own_backend = false;
    bool started = false;
    uint8_t  sector_shift = 9;
    bool     read_only = false;
    bool stopping = false;

    explicit VhostUserDeviceImpl(const VhostUserController::Config& c) : cfg(c) {
        sector_shift = cfg.info.sector_size_shift;
        read_only = cfg.read_only;
        capacity_sectors = cfg.info.size >> 9;
        snprintf(sock_path, sizeof(sock_path), "%s", cfg.sock_path.c_str());

        offer_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_BLK_F_BLK_SIZE) |
                         (1ULL << VIRTIO_RING_F_EVENT_IDX) |
                         (1ULL << VHOST_USER_F_PROTOCOL_FEATURES);   // we always answer
                                                                     // GET_PROTOCOL_FEATURES
        if (cfg.info.features & FEATURE_FLUSH)
            offer_features |= (1ULL << VIRTIO_BLK_F_FLUSH);
        if (read_only)
            offer_features |= (1ULL << VIRTIO_BLK_F_RO);
        fill_config();
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
        if (own_backend)
            delete backend;
        if (listen_fd >= 0) ::close(listen_fd);
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
                LOG_WARN("vhost-user: the frontend attached more than ` fds to one message; "
                         "the surplus did not reach us", VHU_MSG_MAX_FDS);
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
                    LOG_ERROR_RETURN(EPROTO, -1, "vhost-user: one message carried more than "
                                     "` fds", VHU_MSG_MAX_FDS);
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
        if (m->size > sizeof(m->payload)) {
            // copied out first: m is not const here, and alog's forwarding
            // reference cannot bind a packed field (see the access rule above)
            int32_t req = m->request;
            uint32_t sz = m->size;
            LOG_ERROR_RETURN(EPROTO, -1, "vhost-user request ` declares a ` byte payload, "
                             "the largest this protocol has is `",
                             req, sz, (uint32_t)sizeof(m->payload));
        }
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

    void* vq_translate(uint64_t addr, size_t len) {
        // descriptor contents are guest-written GPAs (the vring addresses
        // themselves were QVAs -- two different spaces, see the header)
        return mem.gpa2va(addr, len);
    }

    // Signal the driver's callfd (one eventfd write). The used ring advanced and
    // the driver asked for an IRQ -- and once more when SET_VRING_CALL installs a
    // fresh fd, see there.
    void vq_notify() {
        if (vqx.callfd < 0)
            return;
        uint64_t one = 1;
        ssize_t w = ::write(vqx.callfd, &one, sizeof(one));   // eventfd signal
        if (w < 0)
            LOG_WARN("vhost-user callfd signal failed, ", ERRNO());
    }

    bool vq_may_dispatch() { return vqx.enabled && vqx.addr_set && vq.desc && vq.num; }

    static void* vq_loop_thunk(void* d) {
        ((VhostUserDeviceImpl*)d)->vq.loop();
        return nullptr;
    }

    void vq_bind() {
        vq.backend = backend;
        vq.capacity = capacity_sectors << 9;   // the LBA bound serve_chain enforces
        vq.stack_size = resolve_stack_size(cfg.stack_size);
        vq.read_only = read_only;
        vq.serial = "photon-vhost-user";
        vq.tag = sock_path;
        vq.hooks.translate.bind(this, &VhostUserDeviceImpl::vq_translate);
        vq.hooks.notify.bind(this, &VhostUserDeviceImpl::vq_notify);
        vq.hooks.ready.bind(this, &VhostUserDeviceImpl::vq_may_dispatch);
    }

    void vq_stop() {
        if (!vqx.th) return;
        vqx.enabled = false;
        vq.run = false;
        vq.wake();   // out of its kickfd wait
        photon::thread_interrupt(vqx.th);
        photon::thread_join((photon::join_handle*)vqx.th);
        vqx.th = nullptr;
    }

    void vq_start() {
        if (vqx.th || !vq_may_dispatch())
            return;   // not fully configured yet
        // kickfd is deliberately NOT part of this gate. A SET_VRING_KICK that
        // carries NOFD is protocol-legal and means "there is no kick fd", not
        // "the ring is not ready"; loop() polls every KICK_FALLBACK_US in that
        // case, which is the only way work can be discovered. Refusing to start
        // here made the device silently serve nothing forever.
        vq.used_idx = vring_used_idx(vq.used);
        // Wrap-safe, not a plain `<`: both are free-running uint16 counters, so a
        // crash adoption whose BASE is a stale non-zero value from BEFORE a 65536
        // wrap of the dead daemon's counters (BASE 65530 against used_idx 3)
        // would skip the bump, and dispatch would then re-consume ~65530
        // already-served avail entries -- duplicate completions on possibly
        // recycled descriptor heads.
        if ((uint16_t)(vq.used_idx - vq.last_avail) < 0x8000)
            vq.last_avail = vq.used_idx;   // the frontend's BASE is stale; the
                                           // used ring is authoritative
        // Establish avail_event == last_avail before the loop can sleep on the
        // kickfd. SET_VRING_BASE lets the frontend put last_avail anywhere
        // (handle_msg's SET_VRING_BASE branch), and the two lines above can raise
        // it to used_idx, while avail_event still holds whatever the peer's setup
        // left there -- zero for a fresh ring. Without this publish the invariant
        // does not hold until the first head is consumed, and a driver that kicks
        // in that window is legitimately ignored per §2.7.10.1.
        vq.publish_avail_event();
        vq.stopping = false;
        vq.run = true;
        vqx.th = photon::thread_create(&VhostUserDeviceImpl::vq_loop_thunk, this);
        photon::thread_enable_join(vqx.th);
        LOG_INFO("vhost-user vq0 serving: num ` last_avail ` used_idx `",
                 vq.num, vq.last_avail, vq.used_idx);
    }

    // ----- the message loop (one frontend session) -----

    // recompute the vring HVAs from the stored QVAs against the current table
    void vq_retranslate() {
        if (!vq.num || !vqx.desc_qva)
            return;
        size_t dsz = (size_t)vq.num * sizeof(vring_desc);
        size_t asz = sizeof(uint16_t) * (3 + vq.num);
        size_t usz = sizeof(uint16_t) * 3 + sizeof(vring_used_elem) * vq.num;
        vq.desc = (vring_desc*)mem.qva2va(vqx.desc_qva, dsz);
        vq.avail = (vring_avail*)mem.qva2va(vqx.avail_qva, asz);
        vq.used = (vring_used*)mem.qva2va(vqx.used_qva, usz);
        vqx.addr_set = vq.desc && vq.avail && vq.used;
    }

    // the mappings the vring HVAs were translated through are gone: drop the
    // addresses as well, or vq_may_dispatch() keeps passing (it only re-checks
    // enabled/addr_set/desc/num) and a later SET_VRING_ENABLE dispatches into
    // freed memory
    void vq_invalidate() {
        vq.desc = nullptr;
        vq.avail = nullptr;
        vq.used = nullptr;
        vqx.addr_set = false;
    }

    // Takes ownership of EVERY fd in fds[], marking the ones it keeps as -1 so
    // msg_loop closes only what is left over.
    int handle_mem_table(const vhost_user_msg* m, int* fds, int nfds) {
        uint32_t n = m->payload.memory.nregions;
        // Validate BEFORE anything is torn down: a rejected message must leave
        // the device serving exactly as it was. Clearing the table first (as
        // this used to) unmapped the live regions while vqx.addr_set stayed
        // true, and since a failed SET_MEM_TABLE only sets ack=1 and the session
        // carries on, the next SET_VRING_ENABLE resumed dispatch into them.
        if (n > 8 || (int)n != nfds)
            LOG_ERROR_RETURN(EPROTO, -1, "vhost-user mem table: ` regions vs ` fds", n, nfds);

        // the old mappings back the vring HVAs (and possibly in-flight request
        // iovs): stop dispatch, drain, then swap the table and retranslate
        bool was_enabled = vqx.enabled;
        vqx.enabled = false;
        vq_stop();
        vq.drain();   // in-flight iovs point into the OLD mappings
        mem.clear();
        for (uint32_t i = 0; i < n; i++) {
            vhost_user_memory_region r;   // memcpy out, per the access rule on vhost_user_msg
            memcpy(&r, &m->payload.memory.regions[i], sizeof(r));
            void* base = ::mmap(nullptr, (size_t)r.memory_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fds[i], (off_t)r.mmap_offset);
            if (base == MAP_FAILED) {
                mem.clear();   // munmaps and closes the regions already stored
                vq_invalidate();   // those mappings backed the vring: drop it too
                LOG_ERRNO_RETURN(0, -1, "vhost-user region mmap failed, size `", r.memory_size);
            }
            mem.regions.push_back(MemTable::Region{r.guest_phys_addr, r.userspace_addr,
                                                   r.memory_size, (char*)base, fds[i]});
            fds[i] = -1;   // owned by mem now; msg_loop must not close it
        }
        LOG_INFO("vhost-user mem table: ` regions", n);
        vq_retranslate();
        if (was_enabled) {
            vqx.enabled = true;
            vq_start();
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
        case VHOST_USER_SET_FEATURES:
            negotiated = m->payload.u64;
            // From the NEGOTIATED word, not from offer_features: if the frontend
            // masked bit 29 off we must keep the flags semantics. Deciding from
            // our own offer would have us read a used_event nobody wrote.
            vq.event_idx = !!(negotiated & (1ULL << VIRTIO_RING_F_EVENT_IDX));
            LOG_INFO("vhost-user negotiated features ", HEX(negotiated));
            break;
        case VHOST_USER_GET_PROTOCOL_FEATURES:
            if (reply(conn_fd, m->request,
                      (1ULL << VHOST_USER_PROTOCOL_F_REPLY_ACK) |
                      (1ULL << VHOST_USER_PROTOCOL_F_BACKEND_REQ) |
                      (1ULL << VHOST_USER_PROTOCOL_F_CONFIG)) < 0)
                return false;
            return true;
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
        case VHOST_USER_RESET_OWNER:
        case VHOST_USER_RESET_DEVICE:
            break;
        case VHOST_USER_GET_QUEUE_NUM:
            if (reply(conn_fd, m->request, 1) < 0) return false;
            return true;
        case VHOST_USER_SET_MEM_TABLE:
            if (handle_mem_table(m, fds, nfds) < 0) ack = 1;
            break;
        case VHOST_USER_SET_VRING_NUM: {
            uint32_t n = m->payload.state.num;
            // num is a modulo divisor in dispatch_avail and in
            // vring_used_append, and it sizes the in-flight coroutine cap, so
            // it is checked here instead of trusted: 65536 used to sail through
            // vq_may_dispatch() (which rejects only 0) and then divide by zero
            // on the first completion -- SIGFPE, whole process down.
            if (n < 2 || n > MAX_VRING_NUM || (n & (n - 1))) {
                LOG_ERROR("vhost-user SET_VRING_NUM rejected: num `, need a power of two in [2, `]",
                          n, MAX_VRING_NUM);
                ack = 1;
                break;
            }
            vq.num = n;
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
            // RECONNECT: msg_loop tears down with vq_stop(), not vq_reset(), so
            // desc_qva survives from the previous frontend and the new session's
            // SET_MEM_TABLE re-translates that stale QVA against the new region,
            // fails, and leaves addr_set false -- a desc_qva != 0 gate would then
            // reject the reconnect's protocol-legal NUM with this wrong-cause ack.
            // A live resize has addr_set true on entry, so it re-checks as before.
            bool had_vring = vqx.addr_set;
            vq_retranslate();
            if (had_vring && !vqx.addr_set) {
                LOG_ERROR("vhost-user SET_VRING_NUM ` does not fit the declared region", n);
                ack = 1;
            }
            break;
        }
        case VHOST_USER_SET_VRING_ADDR: {
            vhost_vring_addr a;   // memcpy out, per the access rule on vhost_user_msg
            memcpy(&a, &m->payload.addr, sizeof(a));
            vqx.desc_qva = a.desc_user_addr;
            vqx.used_qva = a.used_user_addr;
            vqx.avail_qva = a.avail_user_addr;
            vq_retranslate();
            if (!vqx.addr_set) {
                LOG_ERROR("vhost-user vring addr translation failed (SET_MEM_TABLE first?), num `",
                          vq.num);
                ack = 1;
            }
            break;
        }
        case VHOST_USER_SET_VRING_BASE:
            vq.last_avail = (uint16_t)m->payload.state.num;
            // vq_start() early-returns once vqx.th is set, so the publish above
            // does NOT cover a BASE that arrives on a live session. That is a
            // protocol violation -- the frontend is supposed to stop the vq first
            // -- but it stays a bounded one only if the invariant still holds, so
            // restore it here. Guarded on addr_set because BASE may legitimately
            // precede SET_VRING_ADDR, and used is still null until then.
            if (vqx.addr_set)
                vq.publish_avail_event();
            break;
        case VHOST_USER_GET_VRING_BASE: {
            // let dispatched requests complete FIRST: replying with a
            // last_avail that outruns the used ring would drop them when the
            // frontend resumes the vq elsewhere (their completions never land)
            vq.drain();
            vq_stop();
            vhost_vring_state s{m->payload.state.index, vq.last_avail};
            if (reply_blob(conn_fd, m->request, &s, sizeof(s)) < 0) return false;
            return true;
        }
        case VHOST_USER_SET_VRING_KICK:
        case VHOST_USER_SET_VRING_CALL: {
            uint64_t u = m->payload.u64;
            bool is_kick = m->request == VHOST_USER_SET_VRING_KICK;
            int* slot = is_kick ? &vq.kickfd : &vqx.callfd;
            if (*slot >= 0) { ::close(*slot); *slot = -1; }
            if (!(u & VHOST_USER_VRING_NOFD_MASK) && nfds > 0) {
                *slot = fds[0];
                fds[0] = -1;   // ours now
            }
            if (is_kick) {
                vq_start();
            } else if (vqx.callfd >= 0) {
                // Signal once on installing the callfd: a frontend that
                // reconnects may be blocked on an interrupt for completions whose
                // notification died with the old connection, and one spurious
                // signal makes it re-poll the used ring. Explicitly permitted --
                // virtio 1.2 §2.7.7.1 requires the driver to handle spurious
                // notifications from the device -- and harmless by construction:
                // it re-reads used->idx and finds nothing new. QEMU's
                // vhost-user-blk idx test waits for exactly this ISR before it
                // sends its first request.
                vq_notify();
            }
            break;
        }
        case VHOST_USER_SET_VRING_ENABLE:
            if (m->payload.state.num) {
                vqx.enabled = true;
                vq_start();
            } else {
                vq_stop();
            }
            break;
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
        // session over: stop serving; the listener (SERVER) stays up for the
        // frontend's reconnect (QEMU reconnect=on drives the recovery)
        vq_stop();
    }

    // ----- connection setup -----

    int do_listen() {
        // blk.h start() contract: EBUSY when another live process is serving
        // this identity -- do NOT steal a live backend's socket path (it would
        // keep serving the orphaned inode while new frontends come to us). A
        // full backlog or a dead-slow listener times out and counts as stale,
        // the same heuristic the controller's list_orphans() uses.
        int live = unix_listener_live(sock_path);
        if (live < 0)
            LOG_ERRNO_RETURN(0, -1, "vhost-user listener probe failed on ", sock_path);
        if (live > 0)
            LOG_ERROR_RETURN(EBUSY, -1, "vhost-user socket ` is served by another live backend",
                             sock_path);
        ::unlink(sock_path);   // stale or absent: ours to (re)create
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            LOG_ERRNO_RETURN(0, -1, "vhost-user socket failed");
        sockaddr_un un;
        memset(&un, 0, sizeof(un));
        un.sun_family = AF_UNIX;
        memcpy(un.sun_path, sock_path, strlen(sock_path) + 1);   // start() bounded it
        if (::bind(fd, (sockaddr*)&un, sizeof(un)) < 0) {
            ::close(fd);
            LOG_ERRNO_RETURN(0, -1, "vhost-user bind failed: ", sock_path);
        }
        if (::listen(fd, 1) < 0) {
            ::close(fd);
            LOG_ERRNO_RETURN(0, -1, "vhost-user listen failed: ", sock_path);
        }
        // bind() created the node with 0777 & ~umask; honor sock_mode, or the
        // documented 0666 & ~umask default (the guest process may run as
        // another user). umask has no read-only query: set-and-restore.
        mode_t um = ::umask(0);
        ::umask(um);
        if (::chmod(sock_path, cfg.sock_mode ? (mode_t)cfg.sock_mode
                                             : (mode_t)(0666 & ~um)) < 0)
            LOG_WARN("vhost-user chmod failed on `, ", sock_path, ERRNO());
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
        bc->num_queues = 1;
    }

    int start(fs::IFile* bk, bool ownership) override {
        if (started)
            LOG_ERROR_RETURN(EALREADY, -1, "vhost-user device already started");
        if (!bk)
            LOG_ERROR_RETURN(EINVAL, -1, "backend IFile is null");

        backend = bk;
        own_backend = ownership;

        bool ok = false;
        DEFER(if (!ok) { int e = errno; rollback(); errno = e; });

        stopping = false;
        vq_bind();
        vq.stopping = false;
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

    // close the vq's fds and reset it for a future session (vq holds an
    // atomic, so no wholesale assignment)
    void vq_reset() {
        vq_stop();
        if (vq.kickfd >= 0) { ::close(vq.kickfd); vq.kickfd = -1; }
        if (vqx.callfd >= 0) { ::close(vqx.callfd); vqx.callfd = -1; }
        vq.num = 0;
        vqx.desc_qva = vqx.used_qva = vqx.avail_qva = 0;
        vq_invalidate();
        vq.last_avail = 0;
        vq.used_idx = 0;
        // SET_FEATURES arrives every session and resets event_idx, but leaving a
        // true value on the reset path is a hazard; leaving notify_valid true is
        // worse -- it would cost the first completion after a reset its
        // unconditional notification.
        vq.event_idx = false;
        vq.notify_valid = false;
        vqx.enabled = false;
    }

    // stop serving + disconnect; keep listening state consistent with `role`
    void stop_session(bool drain_backlog) {
        if (!drain_backlog)
            vqx.enabled = false;
        if (vqx.th) {
            // addr_set, not vq.desc: the frontend supplies the three vring QVAs
            // independently, so it can make desc resolve while avail does not,
            // and this dereferences avail. addr_set is exactly "all three
            // resolved" (see vq_retranslate).
            while (vq.in_flight.load() ||
                   (drain_backlog && vqx.enabled && vqx.addr_set &&
                    vq.last_avail != vring_avail_idx(vq.avail)))
                photon::thread_usleep(1000);
        }
        stopping = true;
        vq.stopping = true;   // from here the engine leaves in-flight requests
                              // uncompleted
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
        vq_stop();   // join the vq loop: no further dispatch
        // the pre-join drain cannot see a batch dispatched in the window
        // before `stopping` took effect: those request coroutines hold VAs
        // into the memory table, so wait for them before vq_reset/mem.clear
        // unmapping under them (use-after-free)
        vq.drain();
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
        if (!started)
            return 0;
        detach(true);
        if (cfg.sock_role == VhostUserController::SockRole::SERVER)
            ::unlink(sock_path);   // remove the tombstone; a CLIENT frontend
                                   // owns its socket and is left alone
        LOG_INFO("vhost-user device shut down, ",
                 make_named_value("sock_path", (const char*)sock_path));
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
        vq.capacity = new_size;   // serve_chain's LBA bound must grow with us
        cfg.info.size = new_size;
        fill_config();
        if (backend_req_fd >= 0) {   // announce: the frontend re-reads GET_CONFIG
            vhost_user_msg m;
            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_BACKEND_CONFIG_CHANGE_MSG;
            m.flags = VHOST_USER_VERSION;
            m.size = 0;
            if (send_msg(backend_req_fd, &m) < 0)
                LOG_WARN("vhost-user config-change notification failed (the frontend "
                         "picks the new capacity up at the next GET_CONFIG)");
        } else {
            LOG_WARN("vhost-user resize without a backend channel: the frontend sees "
                     "the new capacity at its next GET_CONFIG");
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
        vq_stop();
        vq.drain();
        vq_reset();
        mem.clear();
        if (conn_fd >= 0) { ::close(conn_fd); conn_fd = -1; }
        if (backend_req_fd >= 0) { ::close(backend_req_fd); backend_req_fd = -1; }
        if (listen_fd >= 0) {
            ::close(listen_fd);
            listen_fd = -1;
            // only OUR listener's socket file: when do_listen failed at the
            // live-probe/bind, the path belongs to another backend -- do not
            // unlink it (same invariant as ublk's rollback-vs-DEL_DEV)
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
        return new VhostUserDeviceImpl(cfg);
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
};

VhostUserController* new_vhost_user_controller(const char* sock_dir) {
    if (!sock_dir || !*sock_dir)
        LOG_ERROR_RETURN(EINVAL, nullptr, "a vhost-user controller needs a socket directory; there is no default");
    if (validate_scope_dir(sock_dir) < 0)
        return nullptr;   // already logged
    return new VhostUserControllerImpl(sock_dir);
}

}  // namespace blk
}  // namespace photon
