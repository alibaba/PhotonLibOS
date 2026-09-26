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

// Built only on Linux (the vhost-user transport is LINUX-gated). There is no
// kernel consumer for this transport: the test embeds a MOCK FRONTEND (the
// QEMU side of vhost-user-blk) that negotiates over the unix socket, shares
// guest memory via memfd + SCM_RIGHTS, builds virtio-blk requests in the
// vring, kicks, and checks the used ring. The mock keeps its own copy of the
// wire constants -- it is an independent implementation of the other end.
//
// The mock runs entirely on a std::thread (blocking syscalls); the photon
// vcpu stays free to run the backend's coroutines -- the vduse/tcmu lesson.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
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
#include <photon/thread/thread11.h>   // thread_create11 for the two helper coroutines

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace photon {
namespace blk {

// ---- vhost-user wire format (mirrors vhost-user.cpp; independent copy on
// ---- purpose: the mock is the other end of the protocol)
enum : int32_t {
    MU_GET_FEATURES = 1,
    MU_SET_FEATURES = 2,
    MU_SET_OWNER = 3,
    MU_SET_MEM_TABLE = 5,
    MU_SET_VRING_NUM = 8,
    MU_SET_VRING_ADDR = 9,
    MU_SET_VRING_BASE = 10,
    MU_GET_VRING_BASE = 11,
    MU_SET_VRING_KICK = 12,
    MU_SET_VRING_CALL = 13,
    MU_GET_PROTOCOL_FEATURES = 15,
    MU_SET_PROTOCOL_FEATURES = 16,
    MU_GET_QUEUE_NUM = 17,
    MU_SET_VRING_ENABLE = 18,
    MU_SET_BACKEND_REQ_FD = 21,
    MU_GET_CONFIG = 24,
    MU_SET_STATUS = 39,
};
#define MU_VERSION       1
#define MU_REPLY_MASK    (0x1u << 2)
#define MU_NEED_REPLY    (0x1u << 3)
// in the u64 index field of SET_VRING_KICK / SET_VRING_CALL: "no fd attached"
#define MU_VRING_NOFD    0x100u

struct mu_mem_region { uint64_t gpa, size, qva, mmap_offset; };
struct mu_mem { uint32_t nregions, padding; mu_mem_region regions[8]; };
struct mu_vring_state { uint32_t index, num; };
struct mu_vring_addr {
    uint32_t index, flags;
    uint64_t desc_user_addr, used_user_addr, avail_user_addr, log_guest_addr;
};
struct mu_config { uint32_t offset, size, flags; uint8_t region[256]; };
// packed for the same reason the transport's vhost_user_msg is: the header is 12
// bytes and the payload follows immediately. Sharing the defect is exactly how
// this suite stayed green against a wire format no real frontend speaks.
struct __attribute__((packed)) mu_msg {
    int32_t request;
    uint32_t flags;
    uint32_t size;
    union {
        uint64_t u64;
        mu_vring_state state;
        mu_vring_addr addr;
        mu_mem memory;
        mu_config config;
    } payload;
};
static_assert(offsetof(mu_msg, payload) == 12,
              "vhost-user payload must follow the 12-byte header with no padding");

// ---- virtio-blk (mirrors the transport) ----
#define T_IN 0
#define T_OUT 1
#define T_FLUSH 4
#define S_OK 0
#define S_IOERR 1
#define F_VERSION_1     (1ULL << 32)
#define F_BLK_RO        (1ULL << 5)
#define F_BLK_SIZE      (1ULL << 6)
#define F_BLK_FLUSH     (1ULL << 9)
// virtio 1.2 §5.2.3: bit 12. §5.2.4 makes virtio_blk_config::num_queues
// meaningful only when this is set, so the two are asserted together below.
#define F_BLK_MQ        (1ULL << 12)
// virtio 1.2 §2.7.7.2 / §2.7.10.1: negotiate this and both sides stop looking
// at the flags low bit and suppress by index instead
#define F_RING_EVENT_IDX  (1ULL << 29)
// vhost-user's own bit in the device feature word: the gate on whether the
// frontend negotiates protocol features at all
#define F_VHU_PROTOCOL_FEATURES 30
struct blk_outhdr { uint32_t type, ioprio; uint64_t sector; };
struct blk_config { uint64_t capacity; uint32_t size_max, seg_max;
                    uint16_t cyl; uint8_t heads, sectors; uint32_t blk_size;
                    uint8_t topology[8]; uint8_t wce, unused;
                    uint16_t num_queues; };
// Deliberately NOT the device's virtio_blk_config (sharing it is how a suite
// becomes self-consistently wrong), so this models only the prefix the mock
// reads: capacity at 0, blk_size at 20 and num_queues at 34. The device's
// struct is 60 bytes and blk_size sits at 20 there too -- the
// cyl/heads/sectors group is exactly 4 bytes, so a uint32_t needs no padding
// after it, which is easy to miscompute by hand. Pinned so that reading any
// LATER field from this mock cannot silently land on the wrong bytes.
static_assert(offsetof(blk_config, capacity) == 0 && offsetof(blk_config, blk_size) == 20 &&
              offsetof(blk_config, num_queues) == 34,
              "mock blk_config offsets must match virtio_blk_config for the fields it reads");
struct vdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
#define DESC_F_NEXT 1
#define DESC_F_WRITE 2
struct vavail { uint16_t flags, idx, ring[]; };
struct vused_elem { uint32_t id, len; };
struct vused { uint16_t flags, idx; vused_elem ring[]; };

static const char IMG_PATH[]       = "/tmp/photon-blk-vhu.img";
static constexpr uint64_t IMG_SIZE = 64ull << 20;
// SOCK_PATH deliberately lives INSIDE SOCK_DIR: one VhostUserController scopes
// both the sockets we listen on and the directory we scan for dead ones, and its
// new_device() rejects a sock_path outside it.
static const char SOCK_DIR[]       = "/tmp/photon-blk-vhu-dir";
static const char SOCK_PATH[]      = "/tmp/photon-blk-vhu-dir/vhu.sock";

static constexpr uint64_t IO_LEN = 64ull << 10;   // 64 KiB test IOs
static constexpr uint32_t VQ_NUM = 256;           // descriptors in the split ring
static constexpr uint32_t SLOTS = 64;             // concurrent request slots; a
                                                  // chain takes 3 descriptors,
                                                  // so SLOTS*3 <= VQ_NUM
static constexpr uint64_t DATA_SLOT = 64ull << 10;

// guest-memory layout inside one memfd region (GPA base 0). L_AVAIL/L_USED sit
// past the whole descriptor array: a chain must never be able to write into
// them, however the heads are allocated.
static constexpr uint64_t L_DESC  = 0x0000;                  // VQ_NUM * 16
static constexpr uint64_t L_AVAIL = 0x2000;                  // 2*VQ_NUM + 6
static constexpr uint64_t L_USED  = 0x3000;                  // 8*VQ_NUM + 6
static constexpr uint64_t L_HDR   = 0x10000;                 // SLOTS * 64
static constexpr uint64_t L_DATA  = 0x20000;                 // SLOTS * DATA_SLOT
static constexpr uint64_t L_STATUS = L_DATA + SLOTS * DATA_SLOT;   // SLOTS * 64
static constexpr uint64_t MEM_SIZE = L_STATUS + SLOTS * 64 + 0x10000;

#define USED_F_NO_NOTIFY 1
// The two event-index slots are the uint16 right past each ring's `num`
// entries -- <linux/virtio_ring.h>:193-194 spells them avail->ring[num] and
// *(__virtio16 *)&used->ring[num]. The layout above already reserves room:
// L_AVAIL holds 2*VQ_NUM + 6 bytes and L_USED holds 8*VQ_NUM + 6.
#define USED_EVENT_OFF  (L_AVAIL + 4 + 2 * VQ_NUM)
#define AVAIL_EVENT_OFF (L_USED + 4 + 8 * VQ_NUM)
static_assert(USED_EVENT_OFF + 2 <= L_USED, "used_event must fit before the used ring");
static_assert(AVAIL_EVENT_OFF + 2 <= L_HDR, "avail_event must fit before the header slots");

// ---------------------------------------------------------------------------
// the mock frontend (QEMU side). All methods run on the test's std::thread.
// ---------------------------------------------------------------------------
struct MockFrontend {
    int fd = -1;                 // the vhost-user connection
    int listen_fd = -1;          // CLIENT-role tests: we hold the listener
    int memfd = -1;
    char* mem = nullptr;         // our mapping of the guest memory
    int kickfd = -1, callfd = -1;
    int backend_fd = -1;         // our end of the SET_BACKEND_REQ_FD pair
    uint16_t avail_idx = 0, used_idx = 0, desc_head = 0;
    uint64_t features = 0;
    std::string err;

    bool fail(const char* what) {
        if (err.empty()) err = std::string(what) + ": " + strerror(errno);
        return false;
    }

    // ---- raw message IO (blocking; runs off the photon vcpu) ----
    bool send(mu_msg* m, const int* fds = nullptr, int nfds = 0) {
        m->flags |= MU_VERSION;
        size_t total = offsetof(mu_msg, payload) + m->size;
        size_t off = 0;
        while (off < total) {
            iovec iov{(char*)m + off, total - off};
            char cbuf[CMSG_SPACE(sizeof(int) * 8)];
            msghdr mh;
            memset(&mh, 0, sizeof(mh));
            memset(cbuf, 0, sizeof(cbuf));
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            if (nfds > 0 && off == 0) {
                mh.msg_control = cbuf;
                mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
                cmsghdr* cm = CMSG_FIRSTHDR(&mh);
                cm->cmsg_level = SOL_SOCKET;
                cm->cmsg_type = SCM_RIGHTS;
                cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
                memcpy(CMSG_DATA(cm), fds, sizeof(int) * (size_t)nfds);
            }
            // MSG_NOSIGNAL: nothing here ignores SIGPIPE, and a write to a
            // connection the backend already closed would kill the test binary
            ssize_t w = ::sendmsg(fd, &mh, MSG_NOSIGNAL);
            if (w <= 0) return fail("sendmsg");
            off += (size_t)w;
            nfds = 0;
        }
        return true;
    }
    // Write several messages as ONE byte stream with no read in between, which is
    // what a pipelining frontend produces (QEMU sends SET_OWNER and the next
    // GET_FEATURES back to back) and what the kernel then hands the backend as a
    // single chunk. Flattened into a buffer rather than an iovec list so no
    // address of a packed struct's member is ever taken.
    bool send_pipeline(const std::vector<mu_msg*>& ms) {
        std::vector<char> wire;
        for (auto* m : ms) {
            m->flags |= MU_VERSION;
            size_t total = offsetof(mu_msg, payload) + m->size;
            wire.insert(wire.end(), (char*)m, (char*)m + total);
        }
        size_t off = 0;
        while (off < wire.size()) {
            ssize_t w = ::send(fd, wire.data() + off, wire.size() - off, 0);
            if (w <= 0) return fail("send");
            off += (size_t)w;
        }
        return true;
    }
    // Read exactly n bytes (short reads loop) and accumulate every SCM_RIGHTS fd
    // delivered along the way. The mock used to ask recvmsg for sizeof(mu_msg) in
    // one go, which is the same over-read the backend had: on SOCK_STREAM the
    // kernel glues consecutive messages from one writer into a single chunk, so
    // everything past the first reply's `size` was silently dropped and two
    // pipelined replies could never both be observed.
    bool recv_exact(void* buf, size_t n, int* fds, int* nfds, int ms) {
        size_t off = 0;
        while (off < n) {
            pollfd pfd{fd, POLLIN, 0};
            int pr = ::poll(&pfd, 1, ms);
            if (pr <= 0) { errno = ETIMEDOUT; return fail(pr == 0 ? "reply timeout" : "poll"); }
            iovec iov{(char*)buf + off, n - off};
            char cbuf[CMSG_SPACE(sizeof(int) * 8)];
            msghdr mh;
            memset(&mh, 0, sizeof(mh));
            memset(cbuf, 0, sizeof(cbuf));
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = cbuf;
            mh.msg_controllen = sizeof(cbuf);
            ssize_t r = ::recvmsg(fd, &mh, 0);
            if (r <= 0) return fail("recvmsg");
            off += (size_t)r;
            for (cmsghdr* cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                    int k = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    memcpy(fds + *nfds, CMSG_DATA(cm), (size_t)k * sizeof(int));
                    *nfds += k;
                }
        }
        return true;
    }
    // receive a message, waiting up to `ms`; returns false on timeout/close
    bool recv(mu_msg* m, int* fds, int* nfds, int ms = 5000) {
        *nfds = 0;
        if (!recv_exact(m, offsetof(mu_msg, payload), fds, nfds, ms)) return false;
        if (m->size > sizeof(m->payload)) { errno = EPROTO; return fail("oversized reply"); }
        memset(&m->payload, 0, sizeof(m->payload));
        if (m->size && !recv_exact(&m->payload, m->size, fds, nfds, ms)) return false;
        return true;
    }
    // a request that expects one reply (GET_* or NEED_REPLY acks). `check_ack`
    // opts into requiring the REPLY_ACK payload to be 0 (accepted); it defaults
    // off so callers that read the ack themselves, or do not care, are untouched.
    bool transact(mu_msg* m, mu_msg* reply, const int* fds = nullptr, int nfds = 0,
                  bool check_ack = false) {
        m->flags |= MU_NEED_REPLY;
        if (!send(m, fds, nfds)) return false;
        int got = 0;
        if (!recv(reply, nullptr, &got)) return false;
        if (!(reply->flags & MU_REPLY_MASK) || reply->request != m->request) {
            errno = EPROTO;
            return fail("unexpected reply");
        }
        if (check_ack && reply->payload.u64 != 0) {
            errno = EPROTO;
            return fail("backend error-acked a protocol-legal request");
        }
        return true;
    }

    // ---- connection ----
    bool connect_to(const char* path) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return fail("socket");
        sockaddr_un un;
        memset(&un, 0, sizeof(un));
        un.sun_family = AF_UNIX;
        snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
        for (int i = 0; i < 50; i++) {   // the backend may still be binding
            if (::connect(fd, (sockaddr*)&un, sizeof(un)) == 0)
                return true;
            if (errno != ECONNREFUSED && errno != ENOENT)
                return fail("connect");
            ::usleep(20 * 1000);
        }
        return fail("connect retries exhausted");
    }
    bool listen_on(const char* path) {
        ::unlink(path);
        listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0) return fail("socket");
        sockaddr_un un;
        memset(&un, 0, sizeof(un));
        un.sun_family = AF_UNIX;
        snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
        if (::bind(listen_fd, (sockaddr*)&un, sizeof(un)) < 0) return fail("bind");
        if (::listen(listen_fd, 1) < 0) return fail("listen");
        return true;
    }
    bool accept_from_frontend(int ms = 10000) {
        pollfd pfd{listen_fd, POLLIN, 0};
        if (::poll(&pfd, 1, ms) <= 0) { errno = ETIMEDOUT; return fail("accept poll"); }
        fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) return fail("accept");
        return true;
    }

    // ---- negotiation (the QEMU sequence) ----
    bool negotiate(bool with_backend_channel) {
        mu_msg m, r;
        int fds[8], nfds;
        memset(&m, 0, sizeof(m));

        m.request = MU_GET_FEATURES; m.size = 0;
        if (!transact(&m, &r)) return false;
        features = r.payload.u64;

        // A conformant frontend negotiates protocol features ONLY if the device
        // feature word offered bit 30: vhost-user.rst defines that bit, in both
        // its GET_FEATURES and SET_FEATURES entries, as the one that "signals
        // back-end support for VHOST_USER_GET_PROTOCOL_FEATURES and
        // VHOST_USER_SET_PROTOCOL_FEATURES". Modelling that gate is what lets this
        // suite see a backend that advertises PROTOCOL_F_* but forgets bit 30: the
        // mock used to send the two messages below unconditionally, so REPLY_ACK
        // got negotiated and every ack-based test passed against a frontend no
        // real one would be.
        if (!(features & (1ULL << F_VHU_PROTOCOL_FEATURES))) {
            errno = EPROTONOSUPPORT;
            return fail("GET_FEATURES did not offer bit 30 (VHOST_USER_F_PROTOCOL_FEATURES), "
                        "so the protocol features are unreachable");
        }
        memset(&m, 0, sizeof(m));
        m.request = MU_GET_PROTOCOL_FEATURES; m.size = 0;
        if (!transact(&m, &r)) return false;
        uint64_t proto = r.payload.u64;

        memset(&m, 0, sizeof(m));
        m.request = MU_SET_PROTOCOL_FEATURES; m.size = 8; m.payload.u64 = proto;
        if (!transact(&m, &r)) return false;

        memset(&m, 0, sizeof(m));
        m.request = MU_SET_OWNER; m.size = 0;
        if (!transact(&m, &r)) return false;

        memset(&m, 0, sizeof(m));
        m.request = MU_SET_FEATURES; m.size = 8;
        m.payload.u64 = features;   // the "guest" accepts everything offered
        if (!transact(&m, &r)) return false;

        memset(&m, 0, sizeof(m));
        m.request = MU_GET_QUEUE_NUM; m.size = 0;
        if (!transact(&m, &r)) return false;
        if (r.payload.u64 < 1) { errno = EPROTO; return fail("queue num"); }

        // guest memory: one memfd region, GPA base 0, QVA = our mapping
        memfd = ::memfd_create("vhu-guest", 0);
        if (memfd < 0) return fail("memfd_create");
        if (::ftruncate(memfd, MEM_SIZE) < 0) return fail("ftruncate");
        mem = (char*)::mmap(nullptr, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (mem == MAP_FAILED) return fail("mmap guest");
        memset(mem, 0, MEM_SIZE);
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_MEM_TABLE;
        m.size = offsetof(mu_mem, regions) + sizeof(mu_mem_region);
        m.payload.memory.nregions = 1;
        m.payload.memory.regions[0] = mu_mem_region{0, MEM_SIZE, (uint64_t)mem, 0};
        if (!transact(&m, &r, &memfd, 1)) return false;

        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_NUM; m.size = sizeof(mu_vring_state);
        m.payload.state = {0, VQ_NUM};
        // Require ack == 0: this NUM is protocol-legal, so a backend that
        // error-acks it must fail the negotiation rather than be silently
        // tolerated (the mock never used to inspect the ack payload).
        if (!transact(&m, &r, nullptr, 0, /*check_ack=*/true)) return false;

        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_BASE; m.size = sizeof(mu_vring_state);
        m.payload.state = {0, 0};
        if (!transact(&m, &r)) return false;

        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_ADDR; m.size = sizeof(mu_vring_addr);
        m.payload.addr = mu_vring_addr{0, 0, (uint64_t)(mem + L_DESC),
                                       (uint64_t)(mem + L_USED),
                                       (uint64_t)(mem + L_AVAIL), 0};
        // Same: these addresses fit the region, so a correct backend acks 0.
        if (!transact(&m, &r, nullptr, 0, /*check_ack=*/true)) return false;

        kickfd = ::eventfd(0, EFD_NONBLOCK);
        callfd = ::eventfd(0, EFD_NONBLOCK);
        if (kickfd < 0 || callfd < 0) return fail("eventfd");
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_KICK; m.size = 8; m.payload.u64 = 0;   // idx 0
        if (!transact(&m, &r, &kickfd, 1)) return false;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_CALL; m.size = 8; m.payload.u64 = 0;
        if (!transact(&m, &r, &callfd, 1)) return false;

        if (with_backend_channel) {
            int sp[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) return fail("socketpair");
            backend_fd = sp[0];
            memset(&m, 0, sizeof(m));
            m.request = MU_SET_BACKEND_REQ_FD; m.size = 0;
            if (!transact(&m, &r, &sp[1], 1)) { ::close(sp[1]); return false; }
            ::close(sp[1]);
        }

        // the device config: capacity check happens in the test body
        memset(&m, 0, sizeof(m));
        m.request = MU_GET_CONFIG;
        m.size = offsetof(mu_config, region) + sizeof(blk_config);
        m.payload.config.offset = 0;
        m.payload.config.size = sizeof(blk_config);
        if (!transact(&m, &r)) return false;

        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_ENABLE; m.size = sizeof(mu_vring_state);
        m.payload.state = {0, 1};
        if (!transact(&m, &r)) return false;

        (void)fds; (void)nfds;
        return true;
    }

    uint64_t config_capacity() {
        mu_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = MU_GET_CONFIG;
        m.size = offsetof(mu_config, region) + sizeof(blk_config);
        m.payload.config.offset = 0;
        m.payload.config.size = sizeof(blk_config);
        if (!transact(&m, &r)) return UINT64_MAX;
        blk_config bc;
        memcpy(&bc, r.payload.config.region, sizeof(bc));
        return bc.capacity;
    }

    // ---- request slots (batch submission) ----
    // The used ring reports a request by its descriptor HEAD, so the mock keeps
    // the head -> slot mapping to find the buffers a completion belongs to.
    int16_t slot_of_head[VQ_NUM] = {};
    uint16_t slot_seq = 0;

    static uint64_t hdr_off(uint16_t slot)    { return L_HDR    + (uint64_t)slot * 64; }
    static uint64_t data_off(uint16_t slot)   { return L_DATA   + (uint64_t)slot * DATA_SLOT; }
    static uint64_t status_off(uint16_t slot) { return L_STATUS + (uint64_t)slot * 64; }

    // a chain never straddles the end of the descriptor array (a real driver
    // allocates from a free list and would refuse to split one). The bound has
    // to leave room for all THREE descriptors: reject a next head whose own
    // chain would not fit, i.e. head + 3 + 2 > VQ_NUM - 1. The old
    // `head + 3 >= VQ_NUM` still handed out 254 and 255, whose chains reach
    // 256/257 -- past the ring this mock declares in SET_VRING_NUM. Both sides
    // used to agree on that (the device indexed the descriptor array without a
    // bound, and the shared mapping made the over-read succeed), so it stayed
    // invisible until the device started checking.
    uint16_t alloc_head() {
        uint16_t head = desc_head;
        desc_head = (uint16_t)(head + 6 > VQ_NUM ? 0 : head + 3);
        return head;
    }

    // build one request into `slot` and publish it to the avail ring; no kick
    int submit(uint16_t slot, uint32_t type, uint64_t sector, uint32_t len, bool data_write) {
        uint16_t head = alloc_head();
        auto* desc = (vdesc*)(mem + L_DESC);
        auto* avail = (vavail*)(mem + L_AVAIL);
        auto* hdr = (blk_outhdr*)(mem + hdr_off(slot));
        hdr->type = type;
        hdr->ioprio = 0;
        hdr->sector = sector;
        *(uint8_t*)(mem + status_off(slot)) = 0xff;

        desc[head + 0] = vdesc{hdr_off(slot), sizeof(blk_outhdr), DESC_F_NEXT, (uint16_t)(head + 1)};
        desc[head + 1] = vdesc{data_off(slot), len,
                               (uint16_t)(DESC_F_NEXT | (data_write ? DESC_F_WRITE : 0)),
                               (uint16_t)(head + 2)};
        desc[head + 2] = vdesc{status_off(slot), 1, DESC_F_WRITE, 0};

        avail->ring[avail_idx % VQ_NUM] = head;
        __sync_synchronize();
        avail->idx = ++avail_idx;
        __sync_synchronize();
        slot_of_head[head] = (int16_t)slot;
        return 0;
    }

    bool kick() {
        uint64_t one = 1;
        if (::write(kickfd, &one, 8) != 8) { errno = EIO; return fail("kick"); }
        return true;
    }

    // ---- event index: the mock is the driver, so it owns used_event and reads
    // ---- avail_event. Both predicates are rewritten here rather than shared
    // ---- with blk/utils.h -- see the file's standing rule.

    void set_used_event(uint16_t v) {
        __sync_synchronize();
        *(uint16_t*)(mem + USED_EVENT_OFF) = v;
    }
    uint16_t get_used_event() { return *(uint16_t*)(mem + USED_EVENT_OFF); }
    uint16_t get_avail_event() {
        __sync_synchronize();
        return *(uint16_t*)(mem + AVAIL_EVENT_OFF);
    }

    // §2.7.10.1: notify iff the index that determined where the descriptor
    // landed equals avail_event, and the used ring's flags low bit is clear.
    // `submit()` has already incremented avail_idx, so the index that picked the
    // slot is avail_idx - 1; the uint16 form below is that equality written to
    // survive wraparound.
    bool kick_if_needed() {
        if (kickfd < 0)
            return true;   // no kick fd (revoked by a NOFD SET_VRING_KICK): there
                           // is nothing to notify with, so the device can only
                           // find the work by its own fallback re-scan
        auto* used = (vused*)(mem + L_USED);
        uint16_t flags = used->flags;
        uint16_t idx = (uint16_t)(avail_idx - 1);
        __sync_synchronize();
        uint16_t ae = get_avail_event();
        if ((flags & USED_F_NO_NOTIFY) == 0 && (uint16_t)(idx - ae) < 1)
            return kick();
        return true;
    }

    // strict poll: does NOT consume. collect() drains, so it cannot be used to
    // assert that a notification did or did not happen.
    bool callfd_readable(int ms) {
        pollfd pfd{callfd, POLLIN, 0};
        return ::poll(&pfd, 1, ms) > 0;
    }
    // eventfd accumulates, so one read returns the total since the last read
    uint64_t callfd_drain() {
        uint64_t v = 0;
        while (::read(callfd, &v, 8) == 8) {}
        return v;
    }

    // wait for the next used element (completions come back in ARBITRARY order).
    // Check the ring BEFORE polling: one eventfd read consumes the whole
    // accumulated counter, so a poll-first loop would burn a full timeout on
    // every completion after the first.
    bool collect(uint32_t* head_out, uint32_t* len_out, int ms = 20000) {
        auto* used = (vused*)(mem + L_USED);
        for (int i = 0; i <= ms / 10; i++) {
            // A conformant driver keeps used_event at the index it has consumed
            // to, so §2.7.7.2's equality fires on the next element the device
            // appends (old == U, new == U+1 -> (U+1-U-1)=0 < 1 -> notify), and
            // also when the device appends several at once (old == U, new == U+3
            // -> 2 < 3 -> notify). Without this the slot stays at its memset 0,
            // the device stops interrupting after the first completion, and this
            // loop still finds every result by polling the ring -- a silent
            // false green for the whole suite.
            set_used_event(used_idx);
            __sync_synchronize();
            if (used->idx != used_idx) {
                auto elem = used->ring[used_idx % VQ_NUM];
                used_idx++;
                *head_out = elem.id;
                *len_out = elem.len;
                return true;
            }
            pollfd pfd{callfd, POLLIN, 0};
            int pr = ::poll(&pfd, 1, 10);
            if (pr > 0) {
                uint64_t v;
                while (::read(callfd, &v, 8) == 8) {}
            }
        }
        errno = ETIMEDOUT;
        return fail("used ring");
    }

    int do_request(uint32_t type, uint64_t sector, void* data, size_t len, bool data_write) {
        if (len > DATA_SLOT) { errno = E2BIG; fail("len > DATA_SLOT"); return -1; }
        uint16_t slot = (uint16_t)(slot_seq++ % SLOTS);
        if (data && len && !data_write) memcpy(mem + data_off(slot), data, len);
        submit(slot, type, sector, (uint32_t)len, data_write);
        // a conformant driver: kick only when §2.7.10.1 says to. The
        // unconditional kick() stays for cases that are not about notification.
        if (!kick_if_needed()) return -1;

        uint32_t head, ulen;
        if (!collect(&head, &ulen)) return -1;
        if (head >= VQ_NUM || slot_of_head[head] != (int16_t)slot) {
            errno = EPROTO;
            fail("used elem id");
            return -1;
        }
        if (data && len && data_write) memcpy(data, mem + data_off(slot), len);
        return *(uint8_t*)(mem + status_off(slot));
    }

    int write_dev(uint64_t off, const void* buf, size_t len) {
        return do_request(T_OUT, off >> 9, (void*)buf, len, false);
    }
    int read_dev(uint64_t off, void* buf, size_t len) {
        return do_request(T_IN, off >> 9, buf, len, true);
    }
    int flush_dev() {
        return do_request(T_FLUSH, 0, nullptr, 0, false);
    }

    // ---- raw and malformed messages ----
    // Everything below exists to send what a well-behaved frontend never would.
    // negotiate() and submit() only build valid sequences, which is precisely why
    // the device's rejection paths had never executed: a guard that never runs is
    // indistinguishable from a guard that is not there.

    // transact() checks that a REPLY_ACK arrived, and with check_ack also that
    // the ack payload is 0 (accepted). Left at its default it does not look at
    // the payload, so a non-zero ack -- rejected -- is the caller's to interpret.
    bool set_vring_num(uint32_t n, uint64_t* ack) {
        mu_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_NUM; m.size = sizeof(mu_vring_state);
        m.payload.state = {0, n};
        if (!transact(&m, &r)) return false;
        *ack = r.payload.u64;
        return true;
    }

    // same shape as set_vring_num above: the reply carries the REPLY_ACK payload
    bool set_vring_base(uint32_t base, uint64_t* ack) {
        mu_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_BASE; m.size = sizeof(mu_vring_state);
        m.payload.state = {0, base};
        if (!transact(&m, &r)) return false;
        *ack = r.payload.u64;
        return true;
    }

    bool set_vring_enable(bool on) {
        mu_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_ENABLE; m.size = sizeof(mu_vring_state);
        m.payload.state = {0, (uint32_t)(on ? 1 : 0)};
        return transact(&m, &r);
    }

    // stop the vq, move its base, start it again. Used by the two cases that
    // need a ring whose used_idx does not start at 0 -- a fresh setup() cannot
    // produce one, and both handover and uint16 wraparound need it.
    bool restart_with_base(uint16_t base) {
        mu_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_ENABLE; m.size = sizeof(mu_vring_state);
        m.payload.state = {0, 0};
        if (!transact(&m, &r)) return false;
        uint64_t ack = 0;
        if (!set_vring_base(base, &ack)) return false;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_ENABLE; m.size = sizeof(mu_vring_state);
        m.payload.state = {0, 1};
        return transact(&m, &r);
    }

    bool set_vring_addr(uint64_t desc_qva, uint64_t avail_qva, uint64_t used_qva, uint64_t* ack) {
        mu_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_ADDR; m.size = sizeof(mu_vring_addr);
        m.payload.addr = mu_vring_addr{0, 0, desc_qva, used_qva, avail_qva, 0};
        if (!transact(&m, &r)) return false;
        *ack = r.payload.u64;
        return true;
    }
    bool restore_vring_addr(uint64_t* ack) {
        return set_vring_addr((uint64_t)(mem + L_DESC), (uint64_t)(mem + L_AVAIL),
                              (uint64_t)(mem + L_USED), ack);
    }

    // declare `nregions` but attach only `nfds` of them
    bool set_mem_table_mismatched(uint32_t nregions, int nfds, uint64_t* ack) {
        mu_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_MEM_TABLE;
        m.size = offsetof(mu_mem, regions) + sizeof(mu_mem_region);
        m.payload.memory.nregions = nregions;
        m.payload.memory.regions[0] = mu_mem_region{0, MEM_SIZE, (uint64_t)mem, 0};
        if (!transact(&m, &r, &memfd, nfds)) return false;
        *ack = r.payload.u64;
        return true;
    }

    // Publish a chain the CALLER built and return the device's status byte, or -1
    // if no completion came back. *used_len receives the used element's length,
    // which is the only signal a chain that broke BEFORE its status descriptor
    // leaves behind: the device writes the status byte only if the walk reached
    // it, so a rejected chain reports 0 bytes written and leaves the 0xff
    // sentinel untouched.
    int do_raw(uint16_t head, uint16_t slot, const blk_outhdr& hdr,
               const vdesc* chain, int n, uint32_t* used_len) {
        auto* desc = (vdesc*)(mem + L_DESC);
        *(blk_outhdr*)(mem + hdr_off(slot)) = hdr;
        *(uint8_t*)(mem + status_off(slot)) = 0xff;
        for (int i = 0; i < n; i++)
            desc[head + i] = chain[i];
        publish(head);
        if (!kick()) return -1;
        uint32_t got;
        if (!collect(&got, used_len)) return -1;
        if (got != head) { errno = EPROTO; fail("used elem id"); return -1; }
        return *(uint8_t*)(mem + status_off(slot));
    }

    // append `head` to the avail ring without building a chain
    void publish(uint16_t head) {
        auto* avail = (vavail*)(mem + L_AVAIL);
        avail->ring[avail_idx % VQ_NUM] = head;
        __sync_synchronize();
        avail->idx = ++avail_idx;
        __sync_synchronize();
    }

    uint16_t used_idx_now() {
        __sync_synchronize();
        return ((vused*)(mem + L_USED))->idx;
    }
    // Wait for the used ring to advance by `n`. Measured on the free-running
    // 16-bit index, NOT by counting elements: the device may legitimately lap
    // the 256-entry ring when more than VQ_NUM completions are outstanding.
    bool wait_used_advance(uint16_t from, uint16_t n, int ms) {
        for (int i = 0; i <= ms / 10; i++) {
            if ((uint16_t)(used_idx_now() - from) >= n)
                return true;
            ::usleep(10 * 1000);
        }
        return false;
    }

    // SCM_RIGHTS passes a reference to the SAME open file description, and both
    // ends live in this process, so O_NONBLOCK set by the device's
    // harden_recv_fd is observable on our descriptor. That makes this a direct
    // assertion rather than an inference from behaviour. FD_CLOEXEC is
    // per-descriptor and so is NOT observable this way.
    bool kickfd_is_nonblock() {
        int fl = ::fcntl(kickfd, F_GETFL, 0);
        return fl >= 0 && (fl & O_NONBLOCK);
    }

    // Hand the device an eventfd created WITHOUT EFD_NONBLOCK, replacing the one
    // negotiate() sent. A well-behaved frontend (QEMU) always sends a
    // non-blocking one, which is why the O_NONBLOCK half of harden_recv_fd had
    // never been exercised.
    bool replace_kickfd_blocking() {
        int nfd = ::eventfd(0, 0);
        if (nfd < 0) return fail("eventfd");
        mu_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_KICK; m.size = 8; m.payload.u64 = 0;
        if (!transact(&m, &r, &nfd, 1)) { ::close(nfd); return false; }
        if (kickfd >= 0) ::close(kickfd);
        kickfd = nfd;   // SCM_RIGHTS gave the device its own descriptor
        return true;
    }

    // Revoke the kick fd the protocol's way: SET_VRING_KICK with the NOFD flag
    // and no fd attached. The device closes what it had and is left with none, so
    // from here on it can only find work by its own fallback re-scan -- which is
    // what a NOFD kick means. Our own end goes too, so submit() cannot kick.
    bool drop_kickfd_nofd() {
        mu_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_VRING_KICK; m.size = 8;
        m.payload.u64 = 0 | MU_VRING_NOFD;   // idx 0, no fd
        if (!transact(&m, &r, nullptr, 0)) return false;
        if (kickfd >= 0) ::close(kickfd);
        kickfd = -1;
        return true;
    }

    static int count_fds() {
        int n = 0;
        if (DIR* d = ::opendir("/proc/self/fd")) {
            while (readdir(d)) n++;
            ::closedir(d);
        }
        return n;   // includes the opendir fd itself; the offset cancels
    }

    void close_conn() {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    ~MockFrontend() {
        close_conn();
        if (listen_fd >= 0) ::close(listen_fd);
        if (kickfd >= 0) ::close(kickfd);
        if (callfd >= 0) ::close(callfd);
        if (backend_fd >= 0) ::close(backend_fd);
        if (mem) ::munmap(mem, MEM_SIZE);
        if (memfd >= 0) ::close(memfd);
    }
};

// ---------------------------------------------------------------------------

class VhostUserTest : public ::testing::Test {
public:
    test::TestImage img;
    fs::IFile* file = nullptr;
    // Scopes SOCK_DIR: every device this fixture builds listens inside it, and
    // every orphan it lists is a dead socket in it -- the coupling the controller
    // exists to state.
    VhostUserController* ctl = nullptr;

    void SetUp() override {
        ::mkdir(SOCK_DIR, 0755);
        // clean the socket dir; SOCK_PATH lives inside it now, so this covers it
        if (DIR* d = ::opendir(SOCK_DIR)) {
            struct dirent* e;
            char p[PATH_MAX];
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                snprintf(p, sizeof(p), "%s/%s", SOCK_DIR, e->d_name);
                ::unlink(p);
            }
            ::closedir(d);
        }
        ctl = new_vhost_user_controller(SOCK_DIR);
        ASSERT_NE(nullptr, ctl);
        ASSERT_EQ(0, img.create(IMG_PATH, IMG_SIZE));
        file = img.file;
    }
    void TearDown() override {
        img.release();
        ::unlink(SOCK_PATH);
        delete ctl;
        ctl = nullptr;
    }

    BlkDevInfo make_info() {
        BlkDevInfo i;
        i.identity = "photon-vhu-test";
        i.size = IMG_SIZE;
        i.sector_size_shift = 9;
        i.features = FEATURE_FLUSH;
        return i;
    }

    std::vector<char> pattern(uint8_t seed, size_t n = IO_LEN) { return test::pattern(seed, n); }

    // run a mock-frontend session off the vcpu; `body` returns 0 on success
    // or an errno; the backend serves on this vcpu meanwhile
    template <typename F>
    int run_frontend(F&& body) {
        int rc = -1;
        std::string err;
        test::run_off_vcpu([&] {
            MockFrontend fe;
            rc = body(fe);
            err = fe.err;
        });
        if (!err.empty())
            LOG_ERROR("mock frontend: `", err);
        return rc;
    }

    // the backend-file side of a verification (runs on the vcpu)
    int verify_backend(uint64_t off, const std::vector<char>& expect) {
        std::vector<char> buf(expect.size());
        iovec iov{buf.data(), buf.size()};
        if (file->preadv(&iov, 1, (off_t)off) != (ssize_t)expect.size())
            return EIO;
        return memcmp(buf.data(), expect.data(), expect.size()) ? EILSEQ : 0;
    }
};

TEST_F(VhostUserTest, config_validation) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;

    // the pure config checks are construction-time now: no object at all
    errno = 0;
    VhostUserController::Config bad = cfg;
    bad.sock_path = "";
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    bad = cfg;
    bad.info.size = 0;
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    bad = cfg;
    bad.info.size = 513;
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    // The controller's scope is part of construction too: a socket outside its
    // directory is a device it could never list, so it refuses to build one.
    bad = cfg;
    bad.sock_path = "/tmp/outside-the-scope.sock";
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    // a bare filename has no directory to be inside of
    bad = cfg;
    bad.sock_path = "vhu.sock";
    errno = 0;
    EXPECT_EQ(nullptr, ctl->new_device(bad));
    EXPECT_EQ(EINVAL, errno);

    // trailing slashes are stripped from both sides, so this is the same directory
    std::string slashy = std::string(SOCK_DIR) + "//";
    auto ctl2 = new_vhost_user_controller(slashy.c_str());
    ASSERT_NE(nullptr, ctl2);
    DEFER(delete ctl2);
    auto d2 = ctl2->new_device(cfg);   // SOCK_PATH, spelled without any slash
    EXPECT_NE(nullptr, d2);
    delete d2;                         // never started: nothing to tear down

    // a null backend is start()'s to reject: it is not part of the config
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(nullptr));
    EXPECT_EQ(EINVAL, errno);

    ASSERT_EQ(0, dev->start(file));
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EALREADY, errno);

    // a second object must not steal a LIVE backend's socket path: the start
    // probe finds the listener alive -> EBUSY, and the failed start's rollback
    // must leave dev's socket file in place
    auto dev2 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    errno = 0;
    EXPECT_EQ(-1, dev2->start(file));
    EXPECT_EQ(EBUSY, errno);
    EXPECT_EQ(0, ::access(SOCK_PATH, F_OK));

    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(SOCK_PATH, F_OK));   // SERVER shutdown unlinks
}

TEST_F(VhostUserTest, server_basic_io) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto wbuf = pattern(0x5a);
    std::vector<char> rbuf(IO_LEN);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        auto bc = (blk_config*)(fe.mem + 0);   // unused; capacity via GET_CONFIG
        (void)bc;
        if (fe.write_dev(1 << 20, wbuf.data(), wbuf.size()) != S_OK) return EIO;
        if (fe.flush_dev() != S_OK) return EIO;
        if (fe.read_dev(1 << 20, rbuf.data(), rbuf.size()) != S_OK) return EIO;
        if (memcmp(wbuf.data(), rbuf.data(), wbuf.size())) return EILSEQ;
        // a second range
        auto w2 = pattern(0xa5);
        if (fe.write_dev(IMG_SIZE - (1 << 20), w2.data(), w2.size()) != S_OK) return EIO;
        std::vector<char> r2(w2.size());
        if (fe.read_dev(IMG_SIZE - (1 << 20), r2.data(), r2.size()) != S_OK) return EIO;
        if (memcmp(w2.data(), r2.data(), w2.size())) return EILSEQ;
        return 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(1 << 20, wbuf));
    EXPECT_EQ(0, verify_backend(IMG_SIZE - (1 << 20), pattern(0xa5)));
}

TEST_F(VhostUserTest, client_role) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.sock_role = VhostUserController::SockRole::CLIENT;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    auto wbuf = pattern(0x33);
    std::atomic<int> rc{-1};
    std::string ferr;
    // the mock LISTENS; the backend (CLIENT) connects to it. Deliberately a
    // hand-rolled thread, not test::run_off_vcpu: this one must run CONCURRENTLY
    // with the vcpu-side start() below, which is what connects to it.
    std::thread th([&] {
        MockFrontend fe;
        int r = 0;
        do {
            if (!fe.listen_on(SOCK_PATH)) { r = EADDRINUSE; break; }
            if (!fe.accept_from_frontend()) { r = ETIMEDOUT; break; }
            if (!fe.negotiate(false)) { r = EPROTO; break; }
            if (fe.write_dev(2 << 20, wbuf.data(), wbuf.size()) != S_OK) { r = EIO; break; }
            std::vector<char> rb(wbuf.size());
            if (fe.read_dev(2 << 20, rb.data(), rb.size()) != S_OK) { r = EIO; break; }
            if (memcmp(wbuf.data(), rb.data(), wbuf.size())) { r = EILSEQ; break; }
        } while (false);
        ferr = fe.err;
        rc = r;
    });
    // give the mock a moment to bind, then start (it retries the connect)
    photon::thread_usleep(100 * 1000);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    while (rc.load() < 0)
        photon::thread_usleep(1000);
    th.join();
    if (!ferr.empty()) LOG_ERROR("mock frontend: `", ferr);
    ASSERT_EQ(0, rc.load());
    EXPECT_EQ(0, verify_backend(2 << 20, wbuf));
}

TEST_F(VhostUserTest, read_only) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.read_only = true;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!(fe.features & F_BLK_RO)) return ENXIO;   // F_RO must be offered
        auto wbuf = pattern(0x77);
        if (fe.write_dev(1 << 20, wbuf.data(), wbuf.size()) != S_IOERR) return EACCES;
        std::vector<char> rb(IO_LEN);
        if (fe.read_dev(1 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return 0;
    });
    EXPECT_EQ(0, rc);
}

TEST_F(VhostUserTest, resize_and_config_change) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint64_t NEW_SIZE = 96ull << 20;
    std::atomic<int> phase{0};   // 0: negotiating, 1: negotiated, 2: done
    std::atomic<int> rc{-1};
    std::atomic<uint64_t> cap_before{0}, cap_after{0};
    std::string ferr;
    // deliberately a hand-rolled thread, not test::run_off_vcpu: it interleaves
    // with the vcpu-side resize below through `phase`
    std::thread th([&] {
        MockFrontend fe;
        int r = 0;
        do {
            if (!fe.connect_to(SOCK_PATH)) { r = ECONNREFUSED; break; }
            if (!fe.negotiate(true)) { r = EPROTO; break; }
            cap_before = fe.config_capacity();
            if (cap_before != IMG_SIZE / 512) { r = EILSEQ; break; }
            phase = 1;
            // wait for the backend's resize + CONFIG_CHANGE on the channel
            pollfd pfd{fe.backend_fd, POLLIN, 0};
            if (::poll(&pfd, 1, 10000) <= 0) { r = ETIMEDOUT; break; }
            char b[64];
            ssize_t n = ::read(fe.backend_fd, b, sizeof(b));   // the event msg
            if (n < 12) { r = EPROTO; break; }
            cap_after = fe.config_capacity();
            if (cap_after != NEW_SIZE / 512) { r = EILSEQ; break; }
            // IO past the old size now works
            auto wbuf = pattern(0x66, 4096);
            if (fe.write_dev(IMG_SIZE + (1 << 20), wbuf.data(), wbuf.size()) != S_OK) { r = EIO; break; }
            std::vector<char> rb(wbuf.size());
            if (fe.read_dev(IMG_SIZE + (1 << 20), rb.data(), rb.size()) != S_OK) { r = EIO; break; }
            if (memcmp(wbuf.data(), rb.data(), wbuf.size())) { r = EILSEQ; break; }
            phase = 2;
        } while (false);
        ferr = fe.err;
        rc = r;
    });
    while (phase.load() < 1 && rc.load() < 0)
        photon::thread_usleep(1000);
    if (rc.load() < 0) {
        ASSERT_EQ(0, file->ftruncate(NEW_SIZE));
        ASSERT_EQ(0, dev->resize(NEW_SIZE));
        // shrink is rejected
        errno = 0;
        EXPECT_EQ(-1, dev->resize(IMG_SIZE));
        EXPECT_EQ(EINVAL, errno);
    }
    while (rc.load() < 0)
        photon::thread_usleep(1000);
    th.join();
    if (!ferr.empty()) LOG_ERROR("mock frontend: `", ferr);
    EXPECT_EQ(0, rc.load());
    EXPECT_EQ(IMG_SIZE / 512, cap_before.load());
    EXPECT_EQ(NEW_SIZE / 512, cap_after.load());
}

TEST_F(VhostUserTest, frontend_reconnect) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto wbuf = pattern(0x11);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.write_dev(1 << 20, wbuf.data(), wbuf.size()) != S_OK) return EIO;
        return 0;
    });
    ASSERT_EQ(0, rc);
    // the frontend "crashes"; a new one reconnects and re-negotiates from
    // scratch (this is the vhost-user recovery story: no kernel state)
    std::vector<char> rbuf(IO_LEN);
    rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.read_dev(1 << 20, rbuf.data(), rbuf.size()) != S_OK) return EIO;
        return memcmp(wbuf.data(), rbuf.data(), wbuf.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
}

TEST_F(VhostUserTest, stale_socket_takeover) {
    // a dead listener's socket file: SERVER start() must unlink + rebind
    int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(lfd, 0);
    sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", SOCK_PATH);
    ASSERT_EQ(0, ::bind(lfd, (sockaddr*)&un, sizeof(un)));
    ASSERT_EQ(0, ::listen(lfd, 1));
    ::close(lfd);   // the "crashed" backend: the socket file survives
    ASSERT_EQ(0, ::access(SOCK_PATH, F_OK));

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));   // must replace the stale socket
    DEFER(dev->shutdown());
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        auto wbuf = pattern(0x22, 4096);
        if (fe.write_dev(1 << 20, wbuf.data(), wbuf.size()) != S_OK) return EIO;
        return 0;
    });
    EXPECT_EQ(0, rc);
}

TEST_F(VhostUserTest, orphan_list) {
    // a dead listener in the scan dir -> tombstone; a live one -> skipped
    char dead[96], live[96];   // bounded: they go into sun_path (108)
    snprintf(dead, sizeof(dead), "%s/dead.sock", SOCK_DIR);
    snprintf(live, sizeof(live), "%s/live.sock", SOCK_DIR);
    ::unlink(dead); ::unlink(live);
    int dfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(dfd, 0);
    sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", dead);
    ASSERT_EQ(0, ::bind(dfd, (sockaddr*)&un, sizeof(un)));
    ASSERT_EQ(0, ::listen(dfd, 1));
    ::close(dfd);   // dead: connect gets ECONNREFUSED
    int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(lfd, 0);
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", live);
    ASSERT_EQ(0, ::bind(lfd, (sockaddr*)&un, sizeof(un)));
    ASSERT_EQ(0, ::listen(lfd, 1));   // live: connect succeeds
    DEFER(::close(lfd));

    auto orphans = ctl->list_orphans();
    bool found_dead = false, found_live = false;
    for (auto& rec : orphans) {
        if (rec.identity == dead) found_dead = true;
        if (rec.identity == live) found_live = true;
        EXPECT_EQ(0u, rec.size);   // tombstone: descriptors unspecified
    }
    EXPECT_TRUE(found_dead);
    EXPECT_FALSE(found_live);
    ::unlink(dead);
    ::unlink(live);
}

// High-concurrency stress at the PROTOCOL level: the mock fills the avail ring
// with a whole batch of request chains and kicks ONCE, so the backend
// dispatches them all at once (one coroutine per request) and completes them
// in ARBITRARY order. Each used element is matched back to its slot by
// descriptor head, and the data is validated through harness.h's
// self-describing blocks: a completion carrying another request's data shows
// up as a foreign block, a partially served one as torn.
TEST_F(VhostUserTest, concurrent_stress) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        constexpr int BATCH = 48;         // +1 flush slot: 49*3 = 147 descs <= VQ_NUM
        constexpr uint16_t FLUSH_SLOT = BATCH;
        constexpr int ROUNDS = 12;
        constexpr uint32_t BS = 16384;    // per-request block (<= DATA_SLOT)
        constexpr uint64_t BASE = 1ull << 20;
        std::vector<char> buf(BS);

        for (int r = 0; r < ROUNDS; r++) {
            uint64_t base = BASE + (uint64_t)r * BATCH * BS;   // a fresh grid per
                                                               // round, so a read
                                                               // has one writer
            for (int i = 0; i < BATCH; i++) {
                uint64_t off = base + (uint64_t)i * BS;
                test::stress_format(buf.data(), BS, off, (uint32_t)i, (uint32_t)r + 1);
                memcpy(fe.mem + MockFrontend::data_off((uint16_t)i), buf.data(), BS);
                fe.submit((uint16_t)i, T_OUT, off >> 9, BS, false);
            }
            fe.submit(FLUSH_SLOT, T_FLUSH, 0, 0, false);
            if (!fe.kick()) return EIO;

            bool saw_flush = false;
            for (int n = 0; n < BATCH + 1; n++) {
                uint32_t head, len;
                if (!fe.collect(&head, &len)) return ETIMEDOUT;
                if (head >= VQ_NUM) return EPROTO;
                int slot = fe.slot_of_head[head];
                if (slot < 0 || slot > BATCH) return EPROTO;
                uint8_t st = *(uint8_t*)(fe.mem + MockFrontend::status_off((uint16_t)slot));
                if (st != S_OK) return EIO;
                if (len != 1) return EPROTO;   // OUT and FLUSH write only the status
                if (slot == FLUSH_SLOT) saw_flush = true;
            }
            if (!saw_flush) return EPROTO;

            for (int i = 0; i < BATCH; i++) {
                memset(fe.mem + MockFrontend::data_off((uint16_t)i), 0, BS);
                fe.submit((uint16_t)i, T_IN, (base + (uint64_t)i * BS) >> 9, BS, true);
            }
            if (!fe.kick()) return EIO;
            for (int n = 0; n < BATCH; n++) {
                uint32_t head, len;
                if (!fe.collect(&head, &len)) return ETIMEDOUT;
                if (head >= VQ_NUM) return EPROTO;
                int slot = fe.slot_of_head[head];
                if (slot < 0 || slot >= BATCH) return EPROTO;
                uint8_t st = *(uint8_t*)(fe.mem + MockFrontend::status_off((uint16_t)slot));
                if (st != S_OK) return EIO;
                if (len != BS + 1) return EPROTO;   // the data plus the status byte
            }
            for (int i = 0; i < BATCH; i++) {
                uint64_t off = base + (uint64_t)i * BS;
                memcpy(buf.data(), fe.mem + MockFrontend::data_off((uint16_t)i), BS);
                uint32_t tid = 0, seq = 0;
                int v = test::stress_validate(buf.data(), BS, off, &tid, &seq);
                if (v == 0 && (tid != (uint32_t)i || seq != (uint32_t)r + 1))
                    v = EILSEQ;   // another slot's block in OUR slot
                if (v) {
                    LOG_ERROR("vhost-user stress slot ` off ` round `: ` (owner tid=` seq=`)",
                              i, off, r, test::stress_diagnose(buf.data(), BS, off), tid, seq);
                    return v;
                }
            }
        }
        return 0;
    });
    EXPECT_EQ(0, rc);
}

// ---------------------------------------------------------------------------
// Rejection paths. Everything above drives the device with a well-behaved
// frontend, which is exactly why the guards below had never executed: a check
// that never runs is indistinguishable from a check that is not there. Each of
// these sends something a correct QEMU never would, and each asserts an
// observable that DIFFERS between the guarded and the unguarded code -- "did not
// crash" is not one of them, because this mock's single mapping makes most
// out-of-bounds accesses succeed silently.
// ---------------------------------------------------------------------------

// The descriptor INDEX is guest-written, and MAX_DESC_CHAIN bounds how many steps
// the walk takes, not where they land. desc[VQ_NUM+5] is at byte 0x1050 -- past
// the 256-entry ring the device was told about, but INSIDE this mock's mapping
// (padding before L_AVAIL at 0x2000), so the pre-fix over-read succeeded on
// zeroes and produced exactly the post-fix observables: used_len 0 and the status
// sentinel untouched. The discriminator therefore has to be content: plant a
// descriptor there that the unguarded walk would serve as a real 4 KiB write, and
// assert the backend still holds what was written before it.
TEST_F(VhostUserTest, oob_descriptor_index) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint16_t OOB = VQ_NUM + 5;
    constexpr uint64_t OFF = 4ull << 20;
    constexpr uint16_t RAW_SLOT = 2, EVIL_SLOT = SLOTS - 2;
    static_assert(OOB * sizeof(vdesc) + sizeof(vdesc) <= MEM_SIZE &&
                  OOB * sizeof(vdesc) >= VQ_NUM * sizeof(vdesc),
                  "the out-of-range descriptor must land inside the mock's mapping but "
                  "outside the declared ring, or this proves a segfault rather than a guard");
    static_assert((uint64_t)OOB * sizeof(vdesc) < L_AVAIL,
                  "and it must not overlap the avail ring the device dereferences");

    auto good = pattern(0x11, 4096);
    auto evil = pattern(0xee, 4096);
    int st = -1;
    uint32_t ulen = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        // a known-good block at OFF, so the backend has content to KEEP
        if (fe.write_dev(OFF, good.data(), good.size()) != S_OK) return EIO;

        auto* desc = (vdesc*)(fe.mem + L_DESC);
        memcpy(fe.mem + MockFrontend::data_off(EVIL_SLOT), evil.data(), evil.size());
        // readable and no NEXT: the walk would take it as this T_OUT's payload
        desc[OOB] = vdesc{MockFrontend::data_off(EVIL_SLOT), (uint32_t)evil.size(), 0, 0};

        uint16_t head = fe.alloc_head();
        blk_outhdr hdr{T_OUT, 0, OFF >> 9};
        vdesc chain[1] = {
            {MockFrontend::hdr_off(RAW_SLOT), sizeof(blk_outhdr), DESC_F_NEXT, OOB},
        };
        st = fe.do_raw(head, RAW_SLOT, hdr, chain, 1, &ulen);
        return st < 0 ? EIO : 0;
    });
    ASSERT_EQ(0, rc);
    // the walk broke at the payload, so it never reached the status descriptor:
    // 0 bytes written and the 0xff sentinel intact, under BOTH the old and the new
    // code -- which is the whole reason the backend check below is the assertion
    EXPECT_EQ(0xff, st);
    EXPECT_EQ(0u, ulen);
    EXPECT_EQ(0, verify_backend(OFF, good));
}

// A vring index at or past the queue count must be rejected with an error ack,
// not silently applied to some queue. Before multiqueue the index was parsed and
// then dropped, so every value was accepted; now it selects a slot, and an
// unchecked one is an out-of-bounds subscript into a vector whose length the
// peer does not control but does get to probe.
//
// The ack is the oracle and its value genuinely differs: with the index dropped
// the backend applied a protocol-legal message and acked 0 (or, past the end of
// the vector, died), with the bound in place it acks 1. Every payload below is
// otherwise VALID -- a legal num, legal addresses, NOFD where no fd is offered --
// so the index is the only thing that can make the backend say no. That is what
// keeps this from passing for the wrong reason.
//
// GET_VRING_BASE is only exercised at index 0xffff, not at 1: its accepted reply
// is a vhost_vring_state that ECHOES the index in the low 32 bits, so at index 1
// with last_avail 0 the accepted blob and the error ack are both the u64 value 1
// and no assertion can tell them apart. At 0xffff they differ.
TEST_F(VhostUserTest, vring_index_out_of_range_is_rejected) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.queues = 1;                 // the only legal index is 0
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto good = pattern(0x11, 4096);

    // {message, does it take a vring_state, does it take a vring_addr}
    struct Msg { uint32_t req; bool state; bool addr; const char* name; };
    static const Msg msgs[] = {
        {MU_SET_VRING_NUM,    true,  false, "SET_VRING_NUM"},
        {MU_SET_VRING_ADDR,   false, true,  "SET_VRING_ADDR"},
        {MU_SET_VRING_BASE,   true,  false, "SET_VRING_BASE"},
        {MU_GET_VRING_BASE,   true,  false, "GET_VRING_BASE"},
        {MU_SET_VRING_KICK,   false, false, "SET_VRING_KICK"},
        {MU_SET_VRING_CALL,   false, false, "SET_VRING_CALL"},
        {MU_SET_VRING_ENABLE, true,  false, "SET_VRING_ENABLE"},
    };
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        // negotiate() settles REPLY_ACK, so every request below gets a u64 ack
        if (!fe.negotiate(false)) return EPROTO;
        for (const auto& t : msgs) {
            // GET_VRING_BASE only at 0xffff -- see the comment above
            const uint32_t idxs[2] = {1u, 0xffffu};
            for (int k = (t.req == MU_GET_VRING_BASE ? 1 : 0); k < 2; k++) {
                mu_msg m, r;
                memset(&m, 0, sizeof(m));
                m.request = t.req;
                if (t.state) {
                    m.size = sizeof(mu_vring_state);
                    // num is legal in every case: a power of two in range for NUM,
                    // 0 for BASE, and 1 (enable) for ENABLE -- so a rejection can
                    // only have been caused by the index
                    m.payload.state = {idxs[k], t.req == MU_SET_VRING_NUM ? VQ_NUM
                                       : (t.req == MU_SET_VRING_ENABLE ? 1u : 0u)};
                } else if (t.addr) {
                    m.size = sizeof(mu_vring_addr);
                    // Real QVAs, not the bare L_* offsets: negotiate() declared one
                    // region whose qva base is fe.mem, so a bare offset does not
                    // resolve. Sending an address that cannot be translated would
                    // make the backend reject this message for a DIFFERENT reason,
                    // the ack would be 1 either way, and deleting the index guard
                    // would no longer turn this case red.
                    mu_vring_addr a{idxs[k], 0, (uint64_t)(fe.mem + L_DESC),
                                    (uint64_t)(fe.mem + L_USED),
                                    (uint64_t)(fe.mem + L_AVAIL), 0};
                    memcpy(&m.payload.addr, &a, sizeof(a));
                } else {
                    // KICK/CALL: the index is the low 8 bits of the u64 and NOFD
                    // says "no fd attached", so no SCM_RIGHTS is needed
                    m.size = 8;
                    m.payload.u64 = idxs[k] | MU_VRING_NOFD;
                }
                if (!fe.transact(&m, &r)) return EPROTO;
                if (r.size != 8) {
                    fe.fail("reply was not an 8-byte ack");
                    return EPROTO;
                }
                if (r.payload.u64 != 1) {
                    char why[128];
                    snprintf(why, sizeof(why), "%s accepted vring index %u (ack %llu)",
                             t.name, idxs[k], (unsigned long long)r.payload.u64);
                    fe.fail(why);
                    return EPROTO;
                }
            }
        }
        // And the queue that IS in range must still work. Without this the case
        // also passes against a guard that is too strict -- `if (idx >= 0)` rejects
        // everything and turns every assertion above green. negotiate() set vq 0 up
        // and enabled it, so a rejection must have left it serving.
        if (fe.write_dev(0, good.data(), good.size()) != S_OK) {
            fe.fail("the in-range queue stopped serving after the rejections");
            return EIO;
        }
        return 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(0, good));
}

// blk/utils.h's MAX_QUEUES, spelled out here on purpose: this suite models the
// peer side of the wire and must not reach into the device's internals to learn
// the limit it is asserting against -- that is how a suite becomes
// self-consistently wrong. If the two ever disagree this test goes red, which is
// the point.
static constexpr uint32_t PEER_MAX_QUEUES = 64;

// BlkConfig::queues decides how many virtqueues the device serves, and it
// publishes that one count through two independent channels -- the
// GET_QUEUE_NUM reply and virtio_blk_config::num_queues -- plus advertises
// VIRTIO_BLK_F_MQ exactly when the count is more than one. virtio 1.2 §5.2.4
// makes num_queues meaningful only when F_MQ is set, and a frontend that reads
// one count while the device serves another addresses queues nobody is
// listening on, so its requests vanish. Asserting all three together is what
// stops the two channels from drifting apart.
//
// An over-large request is CLAMPED, not rejected: that is what the ublk
// transport already does with the same field, and a library that failed the
// device would leave the caller no way to ask for "as many as you can".
TEST_F(VhostUserTest, queue_count_follows_config) {
    struct Case { uint32_t ask, want; bool mq; };
    static const Case cases[] = {
        {0,                      1,                      false},   // the default: one queue, no F_MQ
        {1,                      1,                      false},   // one queue must NOT offer F_MQ
        {2,                      2,                      true},    // the exact F_MQ boundary
        {3,                      3,                      true},
        {PEER_MAX_QUEUES,        PEER_MAX_QUEUES,        true},
        {PEER_MAX_QUEUES + 5,    PEER_MAX_QUEUES,        true},    // clamped, not rejected
    };
    for (const auto& c : cases) {
        VhostUserController::Config cfg(make_info());
        cfg.sock_path = SOCK_PATH;
        cfg.queues = c.ask;
        auto dev = ctl->new_device(cfg);
        ASSERT_NE(nullptr, dev) << "queues=" << c.ask;
        DEFER(delete dev);
        ASSERT_EQ(0, dev->start(file)) << "queues=" << c.ask;
        DEFER(dev->shutdown());

        uint64_t want = c.want, got_qn = 0, got_feat = 0;
        uint16_t got_nq = 0;
        int rc = run_frontend([&](MockFrontend& fe) -> int {
            if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
            // Negotiate first, the way a conformant frontend does: this suite was
            // deliberately hardened to model the protocol's own gates, and reading
            // the device config before PROTOCOL_F_CONFIG is settled is not
            // something a real peer does. It also leaves vq 0 running, so the
            // teardown at the end of the iteration exercises every queue slot --
            // including the ones this frontend never addressed.
            if (!fe.negotiate(false)) return EPROTO;
            mu_msg m, r;
            memset(&m, 0, sizeof(m));
            m.request = MU_GET_FEATURES; m.size = 0;
            if (!fe.transact(&m, &r)) return EPROTO;
            got_feat = r.payload.u64;

            memset(&m, 0, sizeof(m));
            m.request = MU_GET_QUEUE_NUM; m.size = 0;
            if (!fe.transact(&m, &r)) return EPROTO;
            got_qn = r.payload.u64;

            memset(&m, 0, sizeof(m));
            m.request = MU_GET_CONFIG;
            m.size = offsetof(mu_config, region) + sizeof(blk_config);
            m.payload.config.offset = 0;
            m.payload.config.size = sizeof(blk_config);
            if (!fe.transact(&m, &r)) return EPROTO;
            blk_config bc;
            memcpy(&bc, r.payload.config.region, sizeof(bc));
            got_nq = bc.num_queues;
            return 0;
        });
        ASSERT_EQ(0, rc) << "queues=" << c.ask;
        EXPECT_EQ(want, got_qn) << "GET_QUEUE_NUM, queues=" << c.ask;
        EXPECT_EQ((uint16_t)want, got_nq) << "num_queues, queues=" << c.ask;
        EXPECT_EQ(c.mq, !!(got_feat & F_BLK_MQ)) << "F_MQ, queues=" << c.ask;
    }
}

// `gpa + len - 1 < base + size` wraps: addr = 2^64 - 101 with len = 200 sums to
// 0x62, which passes, and the old gpa2va then returned base - 101 -- a pointer
// 101 bytes BEFORE the guest mapping, handed to preadv as its destination. The
// iotlb cache in vduse had the same shape. There is no host-side race and nothing
// here depends on the frontend misbehaving: vring_desc.addr and .len are
// guest-written and reach translate() verbatim.
TEST_F(VhostUserTest, wrapping_buffer_address) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint64_t WRAP_ADDR = 0xFFFFFFFFFFFFFF9BULL;   // 2^64 - 101
    constexpr uint32_t WRAP_LEN = 200;   // -101 + 200 = 0x62, well under MEM_SIZE
    static_assert(WRAP_ADDR + WRAP_LEN - 1 < MEM_SIZE, "the predicate this test pins must wrap");

    int st = -1;
    uint32_t ulen = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        uint16_t head = fe.alloc_head();
        constexpr uint16_t SLOT = 3;
        blk_outhdr hdr{T_IN, 0, 0};   // a read: the wrapped pointer is the DESTINATION
        vdesc chain[3] = {
            {MockFrontend::hdr_off(SLOT), sizeof(blk_outhdr), DESC_F_NEXT, (uint16_t)(head + 1)},
            {WRAP_ADDR, WRAP_LEN, (uint16_t)(DESC_F_WRITE | DESC_F_NEXT), (uint16_t)(head + 2)},
            {MockFrontend::status_off(SLOT), 1, DESC_F_WRITE, 0},
        };
        st = fe.do_raw(head, SLOT, hdr, chain, 3, &ulen);
        if (st < 0) return EIO;
        // the rejection must not cost us the session
        auto w = pattern(0x2b, 4096);
        if (fe.write_dev(8 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> rb(w.size());
        if (fe.read_dev(8 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0xff, st);   // broke at the payload desc, never reached the status byte
    EXPECT_EQ(0u, ulen);
    EXPECT_EQ(0, verify_backend(8 << 20, pattern(0x2b, 4096)));
}

// A WRITE past EOF on a regular-file backend EXTENDS it, so without the capacity
// gate a guest grows the image without bound -- and that growth is what makes the
// guard assertable. Every "past the old size" case in resize_and_config_change
// happens AFTER a resize and expects success, so this branch had never run.
TEST_F(VhostUserTest, lba_past_capacity) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint64_t OFF = IMG_SIZE + 4096;
    auto buf = pattern(0x99, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.write_dev(OFF, buf.data(), buf.size()) != S_IOERR) return EIO;
        std::vector<char> rb(buf.size());
        if (fe.read_dev(OFF, rb.data(), rb.size()) != S_IOERR) return EIO;
        // the last legal block is still served, so this is a bound and not a wedge
        if (fe.write_dev(IMG_SIZE - 4096, buf.data(), buf.size()) != S_OK) return EIO;
        return 0;
    });
    ASSERT_EQ(0, rc);
    struct stat sb;
    ASSERT_EQ(0, file->fstat(&sb));
    EXPECT_EQ((uint64_t)IMG_SIZE, (uint64_t)sb.st_size);   // it did not grow
    EXPECT_EQ(0, verify_backend(IMG_SIZE - 4096, buf));
}

// num reaches two modulo divisors (avail->ring[last_avail % num] and
// used->ring[used_idx % num]) and sizes the in-flight coroutine cap. 65536 used to
// sail through vq_may_dispatch() -- which rejects only 0 -- and truncate to 0 in
// vring_used_append's uint16_t parameter: SIGFPE on the first completion. A
// rejected message must also leave the live queue exactly as it was.
TEST_F(VhostUserTest, bad_vring_num) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x44, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        // 0 and 1 are below the floor, 6 is not a power of two, 65536 is the
        // uint16 truncation that used to divide by zero, and 1<<20 is a valid
        // power of two past MAX_VRING_NUM -- the magnitude bound on its own
        for (uint32_t n : {0u, 1u, 6u, 65536u, 1u << 20}) {
            uint64_t ack = 0;
            if (!fe.set_vring_num(n, &ack)) return EPROTO;
            if (ack == 0) {
                LOG_ERROR("SET_VRING_NUM ` was accepted", n);
                return EINVAL;
            }
        }
        if (fe.write_dev(3 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> rb(w.size());
        if (fe.read_dev(3 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(3 << 20, w));
}

// SET_VRING_ADDR's qva2va proves only that the DECLARED length fits a region;
// the returned pointer is then indexed by num. A desc_qva one descriptor short of
// the region end makes the 256-entry array run off it, so the retranslation must
// fail, addr_set must drop -- and the frontend must be told, or it believes the
// queue moved. The session itself is a message-loop thing and has to survive.
TEST_F(VhostUserTest, vring_addr_that_does_not_fit) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x55, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        uint64_t ack = 0;
        uint64_t tail = (uint64_t)(fe.mem + MEM_SIZE - sizeof(vdesc));
        if (!fe.set_vring_addr(tail, (uint64_t)(fe.mem + L_AVAIL),
                               (uint64_t)(fe.mem + L_USED), &ack))
            return EPROTO;
        if (ack == 0) return EINVAL;
        // the message loop is independent of the vring: it must still answer
        if (fe.config_capacity() != IMG_SIZE / 512) return EPROTO;
        // putting the real addresses back has to resume dispatch
        if (!fe.restore_vring_addr(&ack)) return EPROTO;
        if (ack != 0) return EPROTO;
        if (fe.write_dev(5 << 20, w.data(), w.size()) != S_OK) return EIO;
        return 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(5 << 20, w));
}

// A blocking kickfd is not hypothetical. loop() drains it with
// `while (::read(kickfd, &n, 8) == 8);`, which on a blocking fd parks the whole
// serving vcpu until the next kick -- silently defeating KICK_FALLBACK_US, and
// with it the recovery the in-flight cap depends on. QEMU always sends
// EFD_NONBLOCK, which is why this half of harden_recv_fd had never run.
// SCM_RIGHTS shares the open file description and both ends live in this process,
// so the flag is directly observable on our own descriptor rather than inferred
// from behaviour. FD_CLOEXEC is per-descriptor and is NOT observable this way.
TEST_F(VhostUserTest, received_fd_hardening) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x66, 4096);
    bool nonblock = false;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!fe.kickfd_is_nonblock()) return EBADF;   // negotiate sent EFD_NONBLOCK
        if (!fe.replace_kickfd_blocking()) return EPROTO;
        nonblock = fe.kickfd_is_nonblock();
        // and the replacement must actually drive IO through the new descriptor
        if (fe.write_dev(6 << 20, w.data(), w.size()) != S_OK) return EIO;
        return 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_TRUE(nonblock);
    EXPECT_EQ(0, verify_backend(6 << 20, w));
}

// A SET_VRING_KICK carrying NOFD is protocol-legal and means "there is no kick
// fd", not "the ring is not ready". Two defects rode on that reading. loop()
// waited on fd -1, which returns EINVAL WITHOUT yielding -- the rest of the
// iteration is yield-free by design, so the serving vcpu hot-spun at 100%,
// logged two lines per pass and starved msg_loop and accept_loop, which share
// it; in SERVER role the listener never returned to accept, so every other
// frontend was locked out until a local detach. And vq_start() gated on
// kickfd >= 0, so a NOFD kick BEFORE start left the ring unstarted and the
// device silently served nothing forever. With no fd there is no event source,
// so the KICK_FALLBACK_US re-scan is the only way work can be found: this
// asserts that it is, and within collect()'s bounded window.
TEST_F(VhostUserTest, kickfd_revoked_still_serves) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x6b, 4096);
    bool revoked = false;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!fe.drop_kickfd_nofd()) return EPROTO;
        revoked = true;
        // from here the frontend cannot notify at all; the device must find this
        // by itself, and the session must still answer -- a hot spin would not
        if (fe.write_dev(7 << 20, w.data(), w.size()) != S_OK) return EIO;
        if (fe.read_dev(7 << 20, w.data(), w.size()) != S_OK) return EIO;
        return 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_TRUE(revoked);
    EXPECT_EQ(0, verify_backend(7 << 20, w));
}

// dispatch_avail caps in-flight at `num` and leaves the rest pending without
// advancing last_avail; loop()'s KICK_FALLBACK_US re-read is what picks them up.
// So ONE kick for more than `num` requests must still complete all of them -- the
// cap alone would silently strand the overflow. Measured on the free-running used
// INDEX rather than by counting elements, because 264 completions legitimately lap
// the 256-entry used ring.
TEST_F(VhostUserTest, dispatch_cap_recovery) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr int N = VQ_NUM + 8;   // over the in-flight cap AND over the avail ring
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        constexpr uint16_t SLOT = SLOTS - 1;
        auto* desc = (vdesc*)(fe.mem + L_DESC);
        desc[0] = vdesc{MockFrontend::hdr_off(SLOT), sizeof(blk_outhdr), DESC_F_NEXT, 1};
        desc[1] = vdesc{MockFrontend::data_off(SLOT), 512,
                        (uint16_t)(DESC_F_WRITE | DESC_F_NEXT), 2};
        desc[2] = vdesc{MockFrontend::status_off(SLOT), 1, DESC_F_WRITE, 0};
        *(blk_outhdr*)(fe.mem + MockFrontend::hdr_off(SLOT)) = blk_outhdr{T_IN, 0, 0};
        *(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT)) = 0xff;

        // every avail entry names the SAME head: we publish N > VQ_NUM entries into
        // a VQ_NUM-slot ring, so the later ones overwrite the earlier ones. That is
        // harmless precisely because they are indistinguishable.
        uint16_t u0 = fe.used_idx_now();
        for (int i = 0; i < N; i++)
            fe.publish(0);
        if (!fe.kick()) return EIO;   // ONE kick for all N
        if (!fe.wait_used_advance(u0, (uint16_t)N, 20000)) return ETIMEDOUT;
        return *(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT)) == S_OK ? 0 : EIO;
    });
    EXPECT_EQ(0, rc);
}

// detach(true) promises to wait out the pending work, and the teardown order that
// delivers it -- drain the avail backlog WHILE the queue's loop is still live, then
// stop the queue -- was invisible to every other case here: nothing else holds work
// in the ring while a detach runs, so dropping the guard that keeps the loop alive
// on the stop_session path left the whole suite green.
//
// The gate is what makes this deterministic instead of a race we hope to win. With
// every backend IO parked, the VQ_NUM requests the dispatch cap let through never
// retire, so in_flight stays AT the cap and the 8 entries behind it stay unconsumed
// for as long as we like -- not for one KICK_FALLBACK_US interval. Draining first
// therefore completes all N: the queue is still enabled while the drain runs, so the
// parked requests publish their completions and the loop goes on to dispatch the
// rest. Stopping the queue first clears `enabled`, which both stops completions
// being published and strands the backlog for good, so the used ring advances by 0.
// N vs 0 is decided by the ORDER, never by how long R sleeps: those 50 ms only set
// WHEN the gate opens, not what the final count is.
TEST_F(VhostUserTest, detach_waits_for_the_avail_backlog) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    // Declared before `dev`: RecordingFile does not own the backend, so its
    // destruction has to come after the device's shutdown DEFER and its delete.
    test::RecordingFile rf(file);
    rf.gated = true;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    // Set only on the hung-detach path below: a coroutine still inside
    // stop_session reads this device's per-queue state, so deleting it under that
    // would stack a use-after-free on top of the failure being reported.
    bool leak_dev = false;
    DEFER(if (!leak_dev) delete dev);
    ASSERT_EQ(0, dev->start(&rf));
    // A no-op by the time it runs: detach(true) below clears `started`, and
    // shutdown() returns early on it -- so the SERVER-role socket is NOT unlinked
    // here. TearDown unlinks it and SetUp empties the whole directory.
    DEFER(if (!leak_dev) dev->shutdown());

    // The frontend runs on its own OS thread, so both handoffs are explicit: a
    // photon::semaphore it signals (signal() is documented callable from any std
    // thread) to let the detach start, and an eventfd the detach writes once it
    // returned. D must not run before that signal -- a detach that precedes the
    // session finds a queue with no loop, has nothing to drain, and returns at once.
    photon::semaphore armed(0);
    int donefd = ::eventfd(0, 0);
    ASSERT_GE(donefd, 0);
    DEFER(::close(donefd));
    int detach_rc = -1;

    auto rth = photon::thread_create11([&] {
        photon::thread_usleep(50 * 1000);
        rf.release_gate(4096);   // comfortably over the VQ_NUM IOs the cap admitted
    });
    auto dth = photon::thread_create11([&] {
        armed.wait(1);
        detach_rc = dev->detach(true);
        uint64_t one = 1;
        ssize_t w = ::write(donefd, &one, sizeof(one));
        (void)w;
    });
    photon::thread_enable_join(rth);
    photon::thread_enable_join(dth);

    constexpr int N = VQ_NUM + 8;   // over the in-flight cap AND over the avail ring
    uint16_t u0 = 0, u1 = 0;
    uint8_t status = 0xff;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        constexpr uint16_t SLOT = SLOTS - 1;
        auto* desc = (vdesc*)(fe.mem + L_DESC);
        desc[0] = vdesc{MockFrontend::hdr_off(SLOT), sizeof(blk_outhdr), DESC_F_NEXT, 1};
        desc[1] = vdesc{MockFrontend::data_off(SLOT), 512,
                        (uint16_t)(DESC_F_WRITE | DESC_F_NEXT), 2};
        desc[2] = vdesc{MockFrontend::status_off(SLOT), 1, DESC_F_WRITE, 0};
        *(blk_outhdr*)(fe.mem + MockFrontend::hdr_off(SLOT)) = blk_outhdr{T_IN, 0, 0};
        *(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT)) = 0xff;

        // the same head for all N, as in dispatch_cap_recovery: the entries are
        // indistinguishable, so the later ones overwriting the earlier ones in the
        // VQ_NUM-slot ring is harmless
        u0 = fe.used_idx_now();
        for (int i = 0; i < N; i++)
            fe.publish(0);
        if (!fe.kick()) return EIO;   // ONE kick for all N
        armed.signal(1);
        // Read the ring from here, not after run_frontend returns: this thread owns
        // the mapping and destroys it on the way out. Bounded, because an orderly
        // detach that never comes back is a failure this case must REPORT rather
        // than become.
        fe.wait_used_advance(u0, (uint16_t)N, 5000);
        u1 = fe.used_idx_now();
        status = *(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT));
        pollfd pfd{donefd, POLLIN, 0};
        if (::poll(&pfd, 1, 30000) <= 0) return ETIMEDOUT;
        return 0;
    });

    // Both joined before anything they write is read: R touches rf's gate, D owns
    // detach_rc, and neither may outlive the scope that holds them.
    photon::thread_join((photon::join_handle*)rth);
    EXPECT_EQ(0, rc);
    if (rc == 0) {
        photon::thread_join((photon::join_handle*)dth);
        EXPECT_EQ(0, detach_rc);
    } else {
        leak_dev = true;
        ADD_FAILURE() << "detach(true) never returned: the orderly teardown hung";
    }
    EXPECT_EQ((uint16_t)N, (uint16_t)(u1 - u0));
    EXPECT_EQ(S_OK, status);
}

// SET_MEM_TABLE used to clear the live mappings BEFORE validating, so a rejected
// message left addr_set true over unmapped regions and the next SET_VRING_ENABLE
// dispatched into freed memory. The real assertion is therefore not the ack -- it
// is that IO STILL WORKS afterwards.
TEST_F(VhostUserTest, bad_mem_table) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x88, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        struct { uint32_t nregions; int nfds; } bad[] = {
            {2, 1},   // fewer fds than declared regions
            {9, 1},   // past the 8-region payload
            {0, 1},   // and the other direction
        };
        for (auto& b : bad) {
            uint64_t ack = 0;
            if (!fe.set_mem_table_mismatched(b.nregions, b.nfds, &ack)) return EPROTO;
            if (ack == 0) {
                LOG_ERROR("SET_MEM_TABLE `/` was accepted", b.nregions, b.nfds);
                return EINVAL;
            }
        }
        if (fe.write_dev(7 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> rb(w.size());
        if (fe.read_dev(7 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(7 << 20, w));
}

// recvmsg put every attached fd in OUR table, so they are ours from that point on.
// A request we reject, tolerate as a no-op, or do not recognise must still close
// them, or a frontend exhausts the backend's fd table by attaching one to each
// message. Both ends are in this process, so the leak is directly countable.
TEST_F(VhostUserTest, unrecognised_message_fds) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    int leaked = -1;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        int before = MockFrontend::count_fds();
        if (before <= 0) return ENOSYS;
        for (int i = 0; i < 8; i++) {
            int efd = ::eventfd(0, EFD_CLOEXEC);
            if (efd < 0) return EMFILE;
            mu_msg m, r;
            memset(&m, 0, sizeof(m));
            m.request = 9999;   // no such request: handle_msg's default arm
            m.size = 8;
            // transact() waits for the REPLY_ACK, which the default arm sends
            // after its DEFER ran -- so the round trip is the synchronisation
            bool ok = fe.transact(&m, &r, &efd, 1);
            ::close(efd);       // our copy; the device's is what is being counted
            if (!ok) return EPROTO;
        }
        leaked = MockFrontend::count_fds() - before;
        if (leaked)
            LOG_ERROR("` fds survived ` unrecognised messages", leaked, 8);
        return 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, leaked);
}

// QEMU does not wait for a reply before writing the next message: a traced real
// handshake shows SET_OWNER and the following GET_FEATURES going out back to back.
// recv_msg used to ask recvmsg for sizeof(vhost_user_msg) -- the header plus the
// whole payload union -- so on this SOCK_STREAM connection one read swallowed both
// messages, answered the first and discarded the second's bytes past `size`. Both
// ends then blocked forever: the frontend on a reply we never knew to send, us in
// wait_for_fd_readable. The mock could not see it because transact() is strictly
// request/reply, so this writes the messages as one byte stream instead.
TEST_F(VhostUserTest, pipelined_messages) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x91, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;

        mu_msg a, b, c;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        memset(&c, 0, sizeof(c));
        a.request = MU_SET_OWNER;      a.size = 0;   // asks for nothing
        b.request = MU_GET_FEATURES;   b.size = 0;   b.flags = MU_NEED_REPLY;
        c.request = MU_GET_QUEUE_NUM;  c.size = 0;   c.flags = MU_NEED_REPLY;
        if (!fe.send_pipeline({&a, &b, &c})) return EPROTO;

        mu_msg r;
        int got = 0;
        // the two replies must both arrive, and in order: under the over-read the
        // second one never came at all and this timed out
        if (!fe.recv(&r, nullptr, &got, 3000)) return ETIMEDOUT;
        if (r.request != MU_GET_FEATURES || !(r.flags & MU_REPLY_MASK)) return EPROTO;
        if (r.size != 8 || r.payload.u64 != fe.features) return EPROTO;
        if (!fe.recv(&r, nullptr, &got, 3000)) return ETIMEDOUT;
        if (r.request != MU_GET_QUEUE_NUM || !(r.flags & MU_REPLY_MASK)) return EPROTO;
        if (r.size != 8 || r.payload.u64 != 1) return EPROTO;
        // SET_OWNER asked for no reply, so anything still readable here means we
        // answered a message we should have stayed silent on
        pollfd pfd{fe.fd, POLLIN, 0};
        if (::poll(&pfd, 1, 300) > 0) return EPROTO;

        // and the session must still serve IO afterwards
        if (fe.write_dev(3 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> rb(w.size());
        if (fe.read_dev(3 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(3 << 20, w));
}

// recv_msg now reads exactly m->size payload bytes, so a header declaring more
// than the protocol's largest payload would leave us waiting for bytes that never
// come -- a hang where the old "short message" check used to fail the session by
// accident. The bound must turn it back into a closed session, and the device must
// survive that: the listener stays up for the frontend's reconnect.
TEST_F(VhostUserTest, oversized_payload) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0xA7, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;

        mu_msg m;
        memset(&m, 0, sizeof(m));
        m.request = MU_SET_MEM_TABLE;
        m.flags = MU_VERSION;
        m.size = 0xFFFFFFF0u;
        // the 12-byte header alone, deliberately: send() would try to write
        // m.size bytes of payload that does not exist
        size_t hdr = offsetof(mu_msg, payload);
        if (::send(fe.fd, &m, hdr, 0) != (ssize_t)hdr) return EPROTO;

        // The backend must end the session ITSELF, and promptly. Polling for EOF
        // rather than for a missing reply is the whole test: an unbounded payload
        // read also produces no reply, it just blocks forever waiting for bytes
        // that never come, so "no reply" cannot tell the two apart.
        bool eof = false;
        for (int i = 0; i < 50 && !eof; i++) {
            pollfd pfd{fe.fd, POLLIN, 0};
            if (::poll(&pfd, 1, 100) < 0) return errno;
            char b;
            ssize_t r = ::recv(fe.fd, &b, 1, MSG_DONTWAIT);
            if (r == 0) { eof = true; break; }
            if (r < 0 && errno != EAGAIN) return errno;
        }
        if (!eof) return ETIMEDOUT;   // still blocked on the phantom payload
        fe.close_conn();

        // a fresh connection must be accepted and served
        MockFrontend fe2;
        if (!fe2.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe2.negotiate(false)) return EPROTO;
        if (fe2.write_dev(5 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> rb(w.size());
        if (fe2.read_dev(5 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(5 << 20, w));
}

// The backend signals the callfd once when SET_VRING_CALL installs it, so that a
// frontend reconnecting into a ring whose completions were already served is not
// left waiting on an interrupt that died with the old connection. virtio 1.2
// §2.7.7.1 requires the driver to handle spurious notifications, so the extra
// signal is permitted and costs nothing. QEMU's vhost-user-blk idx test waits for
// exactly that ISR before it sends its first request. Nothing else has run by the
// end of negotiate(), and the signal is synchronous inside handle_msg, so the
// eventfd counter is the whole assertion -- and without it the read fails with
// EAGAIN, since the mock's callfd is non-blocking.
TEST_F(VhostUserTest, vring_call_signals_once) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        uint64_t n = 0;
        ssize_t r = ::read(fe.callfd, &n, sizeof(n));
        if (r != (ssize_t)sizeof(n)) return errno ? errno : EPROTO;
        if (n != 1) {
            LOG_ERROR("callfd counter after SET_VRING_CALL is `, expected exactly 1", n);
            return EPROTO;
        }
        return 0;
    });
    EXPECT_EQ(0, rc);
}

// The kick half of EVENT_IDX: §2.7.10.1 lets the driver stay silent unless the
// index it wrote equals avail_event, so a backend that negotiates bit 29 but
// never publishes avail_event leaves it at its setup value 0 and gets kicked for
// the first request only. Every later request then rides the 5 ms fallback poll,
// which is why the IO cases cannot see this -- they still pass, just slower.
// QEMU's qtest cannot see it either: that 5 ms fallback is our own loop()'s, not
// the peer's, so it masks a wrong avail_event for every frontend alike and none of
// them times out. Reading the uint16 directly is the only oracle anywhere.
TEST_F(VhostUserTest, avail_event_published) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    // The event-idx cases below assert with gtest macros, which need a void
    // context -- run_frontend()'s errno-returning lambda cannot host them, so
    // they drive the mock on the raw off-vcpu helper and let failures surface as
    // assertion messages rather than errno codes. `fe` therefore lives out here:
    // a fatal ASSERT returns from the lambda, so anything written at the end of
    // the lambda body is skipped exactly when the diagnostics are wanted.
    MockFrontend fe;
    test::run_off_vcpu([&] {
        ASSERT_TRUE(fe.connect_to(SOCK_PATH));
        ASSERT_TRUE(fe.negotiate(false));
        ASSERT_TRUE(fe.features & F_RING_EVENT_IDX)
            << "bit 29 was not negotiated; this case would pass vacuously on the flags fallback";

        // one request through the normal path, so the backend has consumed one head
        char buf[512];
        ASSERT_EQ(0, fe.write_dev(0, buf, sizeof(buf)));

        // avail_event must equal the index we are about to be given next, i.e. the
        // mock's own avail_idx after one submit
        EXPECT_EQ(fe.avail_idx, fe.get_avail_event())
            << "avail_event is not tracking last_avail; a conformant driver "
               "(virtio 1.2 §2.7.10.1) would stop kicking after the first request";

        // and it must keep tracking across a second request
        ASSERT_EQ(0, fe.read_dev(0, buf, sizeof(buf)));
        EXPECT_EQ(fe.avail_idx, fe.get_avail_event());
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

// The interrupt half of EVENT_IDX. §2.7.7.2: notify iff the used index that
// picked the slot equals used_event, otherwise SHOULD NOT.
//
// DO NOT "simplify" this to use collect(). collect() writes used_event =
// used_idx before every poll (that is what makes it a conformant driver), which
// would overwrite the value this case deliberately plants and turn the
// suppression half into a tautology. It polls the status byte instead -- the
// shape QEMU's own vhost-user-blk idx qtest uses -- and touches callfd
// only through a strict, non-consuming poll.
TEST_F(VhostUserTest, interrupt_suppressed_by_used_event) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    MockFrontend fe;
    test::run_off_vcpu([&] {
        ASSERT_TRUE(fe.connect_to(SOCK_PATH));
        ASSERT_TRUE(fe.negotiate(false));
        ASSERT_TRUE(fe.features & F_RING_EVENT_IDX)
            << "bit 29 was not negotiated; this case would pass vacuously on the flags fallback";

        char buf[512];

        // Request 1 goes through the normal path. Two things happen here that the
        // rest of the case depends on: the backend's first-decision unconditional
        // notify is consumed, and collect() leaves used_event at the index it has
        // consumed to.
        ASSERT_EQ(0, fe.write_dev(0, buf, sizeof(buf)));
        (void)fe.callfd_drain();   // SET_VRING_CALL signals once on install; clear it

        // Request 2, with used_event planted where §2.7.7.2's equality does NOT
        // hold: old will be 1 and new 2, so need_event(e, 2, 1) is (2-e-1) < 1,
        // true only at e == 1. Plant 100.
        //
        // Deliberately an UNCONDITIONAL kick(), not kick_if_needed(): this case is
        // about the interrupt half only, and isolating it that way is what makes it
        // fail for the right reason before the wiring lands (avail_event is still 0
        // there, so kick_if_needed would refuse to kick and this would die on "not
        // served" instead of on the suppression assertion below). §2.7.10.2: "The
        // device MUST handle spurious notifications from the driver." The kick half
        // has its own case, avail_event_published.
        fe.set_used_event(100);
        uint16_t slot2 = 1;
        ASSERT_EQ(0, fe.submit(slot2, T_OUT, 0, sizeof(buf), false));
        ASSERT_TRUE(fe.kick());

        // The request must still be SERVED -- suppression is about the notification,
        // not the work. Poll the status byte, which submit() presets to 0xff.
        bool served = false;
        for (int i = 0; i < 2000 && !served; i++) {
            served = (*(uint8_t*)(fe.mem + fe.status_off(slot2)) != 0xff);
            if (!served) ::usleep(1000);
        }
        ASSERT_TRUE(served) << "the request was not served at all";
        EXPECT_EQ(0, (int)*(uint8_t*)(fe.mem + fe.status_off(slot2)))
            << "served, but with a nonzero virtio-blk status";
        EXPECT_FALSE(fe.callfd_readable(50))
            << "interrupted despite used_event suppressing it (§2.7.7.2 SHOULD NOT)";

        // Request 3, with used_event planted where the equality DOES hold: old will
        // be 2 and new 3, so e == 2. Exactly one notification.
        (void)fe.callfd_drain();
        fe.set_used_event(2);
        uint16_t slot3 = 2;
        ASSERT_EQ(0, fe.submit(slot3, T_OUT, 0, sizeof(buf), false));
        ASSERT_TRUE(fe.kick());   // unconditional, same reason as request 2
        for (int i = 0; i < 2000; i++) {
            if (*(uint8_t*)(fe.mem + fe.status_off(slot3)) != 0xff) break;
            ::usleep(1000);
        }
        ASSERT_TRUE(fe.callfd_readable(1000))
            << "no interrupt although used_event asked for one (§2.7.7.2 MUST)";
        EXPECT_EQ(1u, fe.callfd_drain()) << "expected exactly one notification";
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

// Handover guard. A resumed ring starts used_idx at an arbitrary value while
// used_event is whatever the driver left behind for the previous daemon; those
// two need not satisfy §2.7.7.2's equality, so without an unconditional first
// notification the very first completion after adoption is never reported.
// §2.7.7.1 permits the spurious notification this costs ("The driver MUST handle
// spurious notifications from the device").
//
// Discriminating by construction: with used->idx preset to 10 and used_event at
// 100, need_event(100, 11, 10) is (uint16)(11-100-1)=65446 < 1 -> false, so the
// equality rule alone would NOT notify. Only the first-decision clause makes
// this pass. NOTE it is green on arrival once Task 3's wiring lands -- its teeth
// come from the `nofirstnotify` mutant in Task 5, not from a red run here.
TEST_F(VhostUserTest, first_completion_always_notifies) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    MockFrontend fe;
    test::run_off_vcpu([&] {
        ASSERT_TRUE(fe.connect_to(SOCK_PATH));
        ASSERT_TRUE(fe.negotiate(false));
        ASSERT_TRUE(fe.features & F_RING_EVENT_IDX)
            << "bit 29 was not negotiated; this case would pass vacuously on the flags fallback";

        ASSERT_TRUE(fe.set_vring_enable(false));   // vq_stop() JOINS the loop
                                                   // coroutine, so after this the
                                                   // ring is ours alone

        // Plant a ring that looks adopted: used index 10, used_event a stale 100.
        // avail->idx must be planted in the RING too, not just in the mock's
        // counter: vq_start() resumes last_avail at 10 and dispatch runs until
        // the two are equal as free-running uint16s, so a ring-side 0 (memset)
        // would have the device consume 65526 stale entries -- each completing
        // and notifying -- before wrapping back to quiescence.
        ((vused*)(fe.mem + L_USED))->idx = 10;
        ((vavail*)(fe.mem + L_AVAIL))->idx = 10;
        fe.set_used_event(100);
        fe.used_idx = 10;
        fe.avail_idx = 10;
        ASSERT_TRUE(fe.restart_with_base(0));   // vq_start() picks used_idx up from
                                                // the ring, so last_avail becomes 10
        (void)fe.callfd_drain();                // clear the SET_VRING_CALL signal and
                                                // anything the restart produced

        char buf[512];
        uint16_t slot = 3;
        ASSERT_EQ(0, fe.submit(slot, T_OUT, 0, sizeof(buf), false));
        ASSERT_TRUE(fe.kick());   // unconditional: this case is about notify_valid,
                                  // not about the kick half (§2.7.10.2 permits the
                                  // spurious notification)

        for (int i = 0; i < 2000; i++) {
            if (*(uint8_t*)(fe.mem + fe.status_off(slot)) != 0xff) break;
            ::usleep(1000);
        }
        ASSERT_TRUE(fe.callfd_readable(1000))
            << "the first completion on a resumed ring was not notified";
        EXPECT_EQ(1u, fe.callfd_drain()) << "expected exactly one notification";
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

// uint16 modular arithmetic, exercised. Both avail_idx and used_idx are
// free-running 16-bit counters; the normal cases never approach 65536, so the
// wraparound branch of vring_need_event -- the whole reason it is written as a
// modular comparison rather than an equality -- is otherwise never executed.
// Base 65534 puts four requests across the turn: 65534, 65535, 0, 1.
// This one is RED before the wiring: avail_event is still the setup 0, so the
// first expectation (65535) fails. Its teeth are the `noavailevent` mutant --
// NOT `wrongneed`/`wrongevent`, which it cannot detect: it goes through
// write_dev -> collect(), and collect() polls the used ring, so whether an
// interrupt fired is invisible to it. See SPEC §4.2.
TEST_F(VhostUserTest, event_idx_wrap) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    MockFrontend fe;
    test::run_off_vcpu([&] {
        ASSERT_TRUE(fe.connect_to(SOCK_PATH));
        ASSERT_TRUE(fe.negotiate(false));
        ASSERT_TRUE(fe.features & F_RING_EVENT_IDX)
            << "bit 29 was not negotiated; this case would pass vacuously on the flags fallback";

        ASSERT_TRUE(fe.set_vring_enable(false));   // vq_stop() JOINS the loop
                                                   // coroutine, so after this the
                                                   // ring is ours alone

        const uint16_t BASE = 65534;
        ((vused*)(fe.mem + L_USED))->idx = BASE;
        // ring-side avail->idx too, same reason as first_completion_always_notifies
        ((vavail*)(fe.mem + L_AVAIL))->idx = BASE;
        fe.used_idx = BASE;
        fe.avail_idx = BASE;
        fe.set_used_event(BASE);
        ASSERT_TRUE(fe.restart_with_base(BASE));
        (void)fe.callfd_drain();

        char buf[512];
        for (int i = 0; i < 4; i++) {
            // collect() keeps used_event at the consumed index, so §2.7.7.2 fires on
            // every one of these -- including across the turn
            ASSERT_EQ(0, fe.write_dev((uint64_t)i * 512, buf, sizeof(buf)))
                << "request " << i << " failed, avail_idx=" << fe.avail_idx;
            uint16_t expect = (uint16_t)(BASE + i + 1);
            EXPECT_EQ(expect, fe.get_avail_event())
                << "avail_event did not wrap correctly after request " << i;
            EXPECT_EQ(expect, fe.avail_idx);
        }
        // the turn itself: 65534 + 4 requests lands at 2, having passed through 0
        EXPECT_EQ(2u, (unsigned)fe.avail_idx);
        EXPECT_EQ(2u, (unsigned)((vused*)(fe.mem + L_USED))->idx);
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

}  // namespace blk
}  // namespace photon

int main(int argc, char** argv) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
