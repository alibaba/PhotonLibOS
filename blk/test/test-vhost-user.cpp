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
// vring, kicks, and checks the used ring. The wire protocol constants and
// structs are shared with the backend via vhost-user-wire.h.
//
// The mock runs entirely on a std::thread (blocking syscalls); the photon
// vcpu stays free to run the backend's coroutines -- the vduse/tcmu lesson.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../blk.h"
#include "../vhost-user-wire.h"

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
#include <sys/file.h>
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

// vhost-user wire protocol constants and structs are in vhost-user-wire.h

// ---- virtio-blk (mirrors the transport) ----
#define T_IN 0
#define T_OUT 1
#define T_FLUSH 4
#define T_GET_ID 8
#define ID_BYTES 20        // the fixed width a GET_ID fills, however short the serial
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
// virtio 1.2 §5.2.3: seg_max is the DATA-segment count, and a driver frames those
// with a header and a status descriptor before it counts what it publishes. Read back
// through GET_CONFIG by config_seg_max() below.
#define F_BLK_SEG_MAX     (1ULL << 2)
// virtio 1.2 §2.7.5.3: one ring descriptor names an array of descriptors elsewhere in
// guest memory. Spelled as a mask here, unlike DESC_F_INDIRECT below which is the
// flags-field value -- the same split this file already makes between F_* and DESC_F_*.
#define F_RING_INDIRECT_DESC (1ULL << 28)
#define F_RING_EVENT_IDX  (1ULL << 29)
// vhost-user protocol feature constants are in vhost-user-wire.h
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
// Rewritten here rather than included from blk/utils.h -- the file's standing rule,
// see the blk_config note above.
#define DESC_F_INDIRECT 4
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

// Rings for every queue past the first. They go in the 0x10000 of slack MEM_SIZE
// already reserves past the status slots, so the region is exactly as large as it
// was and nothing else that reads MEM_SIZE moves -- the tail address in
// vring_addr_that_does_not_fit, the fit static_asserts in oob_descriptor_index and
// wrapping_buffer_address. Queue 0 keeps L_DESC/L_AVAIL/L_USED where they are,
// which is what leaves every existing call site reading the same three constants.
static constexpr uint32_t MQ_QUEUES = 4;   // qids 1..MQ_QUEUES, so five in all
static constexpr uint64_t L_MQ = L_STATUS + SLOTS * 64;
// One block per queue with each of the three rings on its own 4 KiB boundary. The
// device sizes them in vq_retranslate (blk/vhost-user.cpp) as num*16, 2*(3+num) and
// 6+8*num, so the block has to hold all three; putting them on separate boundaries
// turns "does it fit" into the three asserts below instead of a sum to recheck by
// hand every time VQ_NUM moves.
static constexpr uint64_t MQ_STRIDE = 0x3000;
static constexpr uint64_t MQ_DESC  = 0x0000;
static constexpr uint64_t MQ_AVAIL = 0x1000;
static constexpr uint64_t MQ_USED  = 0x2000;
static_assert(MQ_DESC + VQ_NUM * sizeof(vdesc) <= MQ_AVAIL,
              "mq descriptor array must fit its block");
// The 6 in each of these is the ring's own 4 bytes of flags+idx plus the 2-byte
// event-index slot that follows its `num` entries, so both slots are inside the fit.
static_assert(MQ_AVAIL + 2 * VQ_NUM + 6 <= MQ_USED, "mq avail ring must fit its block");
static_assert(MQ_USED + 8 * VQ_NUM + 6 <= MQ_STRIDE, "mq used ring must fit its block");
static_assert(L_MQ + MQ_QUEUES * MQ_STRIDE <= MEM_SIZE, "mq rings must fit the declared region");

// Indirect tables. They go in what is left of the 0x10000 slack MEM_SIZE already
// reserves past the status slots, AFTER the mq ring blocks -- so MEM_SIZE does not
// move. That is deliberate and worth more than it looks: the comment above the mq
// blocks records that they were put in this same slack for the same reason, because
// three things read MEM_SIZE and each would need re-checking if it moved -- the tail
// address in vring_addr_that_does_not_fit, the fit static_asserts in
// oob_descriptor_index, and wrapping_buffer_address's proof that its address plus its
// length overflows. All three compare in the direction growing would preserve, so
// growing MEM_SIZE is not WRONG -- it is just a blast radius this feature does not
// need to take.
//
// The design rule the L_AVAIL/L_USED comment states applies to this region unchanged:
// they sit past the whole descriptor array so a chain can never write into them
// however the heads are allocated, and a table is exactly such a region -- the device
// READS it, and nothing the device writes may land in it.
static constexpr uint64_t L_TBL = L_MQ + MQ_QUEUES * MQ_STRIDE;
// Per-slot tables, so concurrent indirect requests do not share one. 8 entries is
// header + data + status with room to split the header or hang a few extras.
static constexpr uint32_t TBL_PER_SLOT = 8;
static constexpr uint64_t TBL_SLOT_BYTES = TBL_PER_SLOT * sizeof(vdesc);
static constexpr uint64_t L_TBL_BIG = L_TBL + SLOTS * TBL_SLOT_BYTES;
// One shared over-cap table, for the single case that needs more entries than a slot
// holds: MAX_INDIRECT_ENTRIES + 1, spelled out rather than read out of the code under
// test (this suite family's standing rule).
static constexpr uint32_t TBL_BIG_ENTRIES = 65;
static_assert(L_TBL_BIG + TBL_BIG_ENTRIES * sizeof(vdesc) <= MEM_SIZE,
              "indirect tables must fit the declared region");

#define USED_F_NO_NOTIFY 1
// The guest's counterpart to the device's bit above, and the only suppression
// channel a session that did not negotiate VIRTIO_RING_F_EVENT_IDX has: §2.7.7.2
// tells the device to ignore this bit once bit 29 IS negotiated, which is to say
// the index replaces the bit rather than sitting beside it. Spelled out here rather
// than taken from blk/utils.h -- the file's standing rule, see PEER_MAX_QUEUES below.
#define AVAIL_F_NO_INTERRUPT 1
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
    int kickfd = -1, callfd = -1;   // queue 0's; the per-queue twins are in mq[]
    int backend_fd = -1;         // our end of the SET_BACKEND_REQ_FD pair
    // Queue 0's cursors, likewise. Everything the ring protocol keeps per queue --
    // the two cursors, the descriptor allocator and the pair of eventfds -- exists
    // once more here for qids 1..MQ_QUEUES, rather than in one array indexed by qid,
    // because cases read and WRITE the qid-0 names directly (fe.avail_idx,
    // fe.used_idx, fe.callfd, fe.kickfd) and folding them in would churn tests that
    // have nothing to do with multiqueue. The *_of(qid) accessors are the only place
    // that has to know which half a queue index lands in, so the split cannot drift.
    uint16_t avail_idx = 0, used_idx = 0, desc_head = 0;
    struct VqState {
        uint16_t avail_idx = 0, used_idx = 0, desc_head = 0;
        int kickfd = -1, callfd = -1;
    };
    VqState mq[MQ_QUEUES];

    // Bounded once, at the five places a case can hand the mock a queue index of its
    // own -- setup_queue, submit, collect, kick and do_request -- so the accessors
    // index without re-checking. A qid past MQ_QUEUES has no ring in the guest
    // mapping either, so the alternative to rejecting it there is a write into the
    // padding past the last block.
    static bool qid_valid(uint32_t qid) { return qid <= MQ_QUEUES; }
    uint16_t& avail_idx_of(uint32_t qid) { return qid ? mq[qid - 1].avail_idx : avail_idx; }
    uint16_t& used_idx_of(uint32_t qid)  { return qid ? mq[qid - 1].used_idx  : used_idx; }
    uint16_t& desc_head_of(uint32_t qid) { return qid ? mq[qid - 1].desc_head : desc_head; }
    int& kickfd_of(uint32_t qid)         { return qid ? mq[qid - 1].kickfd : kickfd; }
    int& callfd_of(uint32_t qid)         { return qid ? mq[qid - 1].callfd : callfd; }
    uint64_t features = 0;
    // Bits this frontend declines, masked off the offered word as soon as it
    // arrives -- so `features` always means "the word this frontend settled on",
    // which is what the assertions in the EVENT_IDX cases read. Zero by default:
    // a permissive guest accepts everything offered.
    uint64_t decline = 0;
    // The same knob for the PROTOCOL word, which is negotiated separately and is a
    // different set of bits. Declining VHOST_USER_PROTOCOL_F_MQ models a primary that will not
    // drive more than one queue, and the observable consequence is that it never
    // asks the count -- so this is how the gate in negotiate() gets teeth.
    uint64_t proto_decline = 0;
    // What SET_PROTOCOL_FEATURES settled on, and the queue count this frontend
    // ended up with. The count keeps its default 1 when VHOST_USER_PROTOCOL_F_MQ was not
    // negotiated, because the query that would have raised it is gated on that bit
    // -- which is exactly the cap a real primary applies, and the reason a case
    // reading `queue_num` has to be paired with one that saw the bit settled.
    uint64_t proto_features = 0;
    uint32_t queue_num = 1;
    // Models a frontend that declines bit 30 of the DEVICE word and therefore never
    // negotiates protocol features at all. That is legal -- the bit is an offer, not
    // a requirement -- and it changes two things this mock has to follow: no reply
    // ever comes back for a setter, because REPLY_ACK lives in the protocol word it
    // never settled, and no SET_VRING_ENABLE is sent, because the spec says that
    // request "should be sent only when VHOST_USER_F_PROTOCOL_FEATURES has been
    // negotiated". A backend that waits for the enable then serves nothing forever.
    // negotiate() masks the bit off the feature word itself when this is set, so the
    // two halves of the mock cannot disagree.
    bool no_protocol_features = false;
    // Models a frontend that never sends SET_FEATURES at all. No conformant peer does
    // this -- that message is how a backend learns anything about the session -- but
    // nothing in the protocol compels it, and the device has no gate that refuses to
    // serve without it: SET_VRING_ENABLE brings a ring up on its own, so a peer that
    // sends the ring messages and then kicks is served. That is the peer a reset path
    // has to be defended against, because whatever a per-queue feature flag holds when
    // it arrives is what that session runs with.
    //
    // `features` reads 0 from the point negotiate() omits the message, and has to: it is
    // documented as the word this frontend settled on, and no word was settled. Leaving
    // the offer standing would put the mock in EVENT_IDX mode against a device that is in
    // flags mode, and the two would then disagree about which suppression channel is
    // live -- the disagreement event_idx_negotiated() exists to keep observable.
    //
    // Mutually exclusive with no_protocol_features, and negotiate() refuses the pair
    // rather than modelling it: together they send neither SET_FEATURES nor
    // SET_VRING_ENABLE, and the device's "enable all rings immediately" fallback lives
    // INSIDE its SET_FEATURES handler. That is a limit on this MOCK, not on the device --
    // the device runs its ENABLE arm whether or not it owes a reply, so a peer willing to
    // send one silently could still bring a ring up -- but every ENABLE helper here goes
    // through transact(), which cannot complete without REPLY_ACK. So negotiate() would
    // bring no ring up, and a case would die on an I/O timeout instead of on an assertion.
    bool skip_set_features = false;
    std::string err;

    bool fail(const char* what) {
        if (err.empty()) err = std::string(what) + ": " + strerror(errno);
        return false;
    }

    // ---- raw message IO (blocking; runs off the photon vcpu) ----
    bool send(vhost_user_msg* m, const int* fds = nullptr, int nfds = 0) {
        m->flags |= VHOST_USER_VERSION;
        size_t total = offsetof(vhost_user_msg, payload) + m->size;
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
    bool send_pipeline(const std::vector<vhost_user_msg*>& ms) {
        std::vector<char> wire;
        for (auto* m : ms) {
            m->flags |= VHOST_USER_VERSION;
            size_t total = offsetof(vhost_user_msg, payload) + m->size;
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
    // delivered along the way. The mock used to ask recvmsg for sizeof(vhost_user_msg) in
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
    bool recv(vhost_user_msg* m, int* fds, int* nfds, int ms = 5000) {
        *nfds = 0;
        if (!recv_exact(m, offsetof(vhost_user_msg, payload), fds, nfds, ms)) return false;
        if (m->size > sizeof(m->payload)) { errno = EPROTO; return fail("oversized reply"); }
        memset(&m->payload, 0, sizeof(m->payload));
        if (m->size && !recv_exact(&m->payload, m->size, fds, nfds, ms)) return false;
        return true;
    }
    // a request that expects one reply (GET_* or NEED_REPLY acks). `check_ack`
    // opts into requiring the REPLY_ACK payload to be 0 (accepted); it defaults
    // off so callers that read the ack themselves, or do not care, are untouched.
    bool transact(vhost_user_msg* m, vhost_user_msg* reply, const int* fds = nullptr, int nfds = 0,
                  bool check_ack = false) {
        m->flags |= VHOST_USER_NEED_REPLY_MASK;
        if (!send(m, fds, nfds)) return false;
        int got = 0;
        if (!recv(reply, nullptr, &got)) return false;
        if (!(reply->flags & VHOST_USER_REPLY_MASK) || reply->request != m->request) {
            errno = EPROTO;
            return fail("unexpected reply");
        }
        if (check_ack && reply->payload.u64 != 0) {
            errno = EPROTO;
            return fail("backend error-acked a protocol-legal request");
        }
        return true;
    }

    // Settles a SET_* message the way this frontend's negotiated word allows. With
    // REPLY_ACK settled it is transact(); without it the back-end sends no reply at
    // all, so waiting for one would block until the socket dies. Queries keep using
    // transact() directly: GET_* is answered whether or not REPLY_ACK was settled,
    // and routing them through here would hide a backend that stopped answering them.
    // `check_ack` cannot be honored on the silent path -- there is no ack to read --
    // so a case that needs an ack assertion has to settle REPLY_ACK first.
    bool settle(vhost_user_msg* m, const int* fds = nullptr, int nfds = 0, bool check_ack = false) {
        if (proto_features & (1ULL << VHOST_USER_PROTOCOL_F_REPLY_ACK)) {
            vhost_user_msg r;
            return transact(m, &r, fds, nfds, check_ack);
        }
        return send(m, fds, nfds);
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
        // Refused before a message goes out, so a case that asked for the combination
        // hears about it here instead of timing out on I/O later. See the knob.
        if (skip_set_features && no_protocol_features) {
            errno = EINVAL;
            return fail("skip_set_features and no_protocol_features together leave "
                        "negotiate() no way to enable a ring");
        }
        vhost_user_msg m, r;
        int fds[8], nfds;
        memset(&m, 0, sizeof(m));

        m.request = VHOST_USER_GET_FEATURES; m.size = 0;
        if (!transact(&m, &r)) return false;
        features = r.payload.u64 & ~decline;
        // One knob, not two: declining bit 30 and skipping the protocol exchange
        // are the same decision, and letting a caller make only half of it would
        // leave SET_FEATURES carrying a bit this frontend then behaves as though it
        // had never negotiated.
        if (no_protocol_features)
            features &= ~(1ULL << VHOST_USER_F_PROTOCOL_FEATURES);

        // A conformant frontend negotiates protocol features ONLY if the device
        // feature word offered bit 30: vhost-user.rst defines that bit, in both
        // its GET_FEATURES and SET_FEATURES entries, as the one that "signals
        // back-end support for VHOST_USER_GET_PROTOCOL_FEATURES and
        // VHOST_USER_SET_PROTOCOL_FEATURES". Modelling that gate is what lets this
        // suite see a backend that advertises PROTOCOL_F_* but forgets bit 30: the
        // mock used to send the two messages below unconditionally, so REPLY_ACK
        // got negotiated and every ack-based test passed against a frontend no
        // real one would be.
        //
        // Declining the bit is a third thing again, and a legal one: an offer is
        // not an obligation, and no_protocol_features models the frontend that
        // declines. It sends neither message below and settles no protocol word,
        // which is why every setter further down goes through settle().
        if (!(features & (1ULL << VHOST_USER_F_PROTOCOL_FEATURES)) && !no_protocol_features) {
            errno = EPROTONOSUPPORT;
            return fail("GET_FEATURES did not offer bit 30 (VHOST_USER_F_PROTOCOL_FEATURES), "
                        "so the protocol features are unreachable");
        }
        if (no_protocol_features) {
            // neither protocol message is sent; proto_features stays 0
        } else {
            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_GET_PROTOCOL_FEATURES; m.size = 0;
            if (!transact(&m, &r)) return false;
            // Settled word, not the raw offer, and the settled word is what goes back
            // in SET_PROTOCOL_FEATURES: sending the offer after masking bits off it
            // locally would be a frontend lying about what it negotiated.
            proto_features = r.payload.u64 & ~proto_decline;

            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_SET_PROTOCOL_FEATURES; m.size = 8;
            m.payload.u64 = proto_features;
            if (!transact(&m, &r)) return false;
        }

        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_OWNER; m.size = 0;
        if (!settle(&m)) return false;

        if (skip_set_features) {
            // ONE message omitted and nothing else moved. That is a judgement rather
            // than a spec quotation -- no conformant frontend omits SET_FEATURES, so
            // nothing says what such a one does next -- and what it buys is a peer the
            // backend can still serve: the protocol exchange above is what settles
            // REPLY_ACK for settle(), and SET_VRING_ENABLE below is the only message
            // left that can bring a ring up, the device's own enable-all fallback
            // living inside the SET_FEATURES handler this frontend never triggers.
            // No word was settled, so `features` reads as none from here on.
            features = 0;
        } else {
            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_SET_FEATURES; m.size = 8;
            m.payload.u64 = features;   // the word this frontend settled on above,
                                        // which is the offer minus whatever it declined
            if (!settle(&m)) return false;
        }

        // Asked ONLY once protocol MQ is settled, which is what a primary does:
        // without the bit the backend's maximum queue count is taken to be 1 and
        // this query never happens. Sending it unconditionally is what let this
        // suite pass against a device that offered virtio F_MQ, published a
        // truthful num_queues and answered GET_QUEUE_NUM with the real count, yet
        // could not be driven as multiqueue -- the mock was reading a number no
        // conformant peer would have asked for, so the two disagreed and only the
        // peer was right.
        if (proto_features & (1ULL << VHOST_USER_PROTOCOL_F_MQ)) {
            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_GET_QUEUE_NUM; m.size = 0;
            if (!transact(&m, &r)) return false;
            if (r.payload.u64 < 1) { errno = EPROTO; return fail("queue num"); }
            queue_num = (uint32_t)r.payload.u64;
        }

        // guest memory: one memfd region, GPA base 0, QVA = our mapping
        memfd = ::memfd_create("vhu-guest", 0);
        if (memfd < 0) return fail("memfd_create");
        if (::ftruncate(memfd, MEM_SIZE) < 0) return fail("ftruncate");
        mem = (char*)::mmap(nullptr, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (mem == MAP_FAILED) return fail("mmap guest");
        memset(mem, 0, MEM_SIZE);
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_MEM_TABLE;
        m.size = offsetof(vhost_user_memory, regions) + sizeof(vhost_user_memory_region);
        m.payload.memory.nregions = 1;
        m.payload.memory.regions[0] = vhost_user_memory_region{0, MEM_SIZE, (uint64_t)mem, 0};
        if (!settle(&m, &memfd, 1)) return false;

        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_NUM; m.size = sizeof(vhost_vring_state);
        m.payload.state = {0, VQ_NUM};
        // Require ack == 0: this NUM is protocol-legal, so a backend that
        // error-acks it must fail the negotiation rather than be silently
        // tolerated (the mock never used to inspect the ack payload). A frontend
        // that declined REPLY_ACK gets no ack to inspect, so the check is skipped
        // for it -- the case that drives that frontend asserts on I/O instead.
        if (!settle(&m, nullptr, 0, /*check_ack=*/true)) return false;

        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_BASE; m.size = sizeof(vhost_vring_state);
        m.payload.state = {0, 0};
        if (!settle(&m)) return false;

        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_ADDR; m.size = sizeof(vhost_vring_addr);
        m.payload.addr = vhost_vring_addr{0, 0, (uint64_t)(mem + L_DESC),
                                       (uint64_t)(mem + L_USED),
                                       (uint64_t)(mem + L_AVAIL), 0};
        // Same: these addresses fit the region, so a correct backend acks 0.
        if (!settle(&m, nullptr, 0, /*check_ack=*/true)) return false;

        kickfd = ::eventfd(0, EFD_NONBLOCK);
        callfd = ::eventfd(0, EFD_NONBLOCK);
        if (kickfd < 0 || callfd < 0) return fail("eventfd");
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_KICK; m.size = 8; m.payload.u64 = 0;   // idx 0
        if (!settle(&m, &kickfd, 1)) return false;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_CALL; m.size = 8; m.payload.u64 = 0;
        if (!settle(&m, &callfd, 1)) return false;

        if (with_backend_channel) {
            int sp[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) return fail("socketpair");
            backend_fd = sp[0];
            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_SET_BACKEND_REQ_FD; m.size = 0;
            if (!settle(&m, &sp[1], 1)) { ::close(sp[1]); return false; }
            ::close(sp[1]);
        }

        // the device config: capacity check happens in the test body
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_CONFIG;
        m.size = offsetof(vhost_user_config, region) + sizeof(blk_config);
        m.payload.config.offset = 0;
        m.payload.config.size = sizeof(blk_config);
        if (!transact(&m, &r)) return false;

        // Not sent at all without bit 30: the spec says this request "should be
        // sent only when VHOST_USER_F_PROTOCOL_FEATURES has been negotiated", and
        // says of the same condition that the "back-end must enable all rings
        // immediately". So this is the message whose absence the backend has to
        // notice -- sending it anyway would test nothing.
        if (!no_protocol_features) {
            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_SET_VRING_ENABLE; m.size = sizeof(vhost_vring_state);
            m.payload.state = {0, 1};
            if (!transact(&m, &r)) return false;
        }

        (void)fds; (void)nfds;
        return true;
    }

    // Bring up ONE more virtqueue, qid 1..MQ_QUEUES, with its own ring in the guest
    // mapping and its own pair of eventfds. The message sequence is negotiate()'s own
    // for qid 0 verbatim -- NUM, BASE, ADDR, KICK, CALL, ENABLE -- because that order
    // is what a primary sends and what the backend's handlers are written against:
    // SET_VRING_NUM's "does the ring fit" check is gated on an already-translated
    // vring, and the comment there records QEMU sending NUM before ADDR. Same
    // check_ack on NUM and ADDR, for the same reason negotiate() gives: both are
    // protocol-legal here, so an error ack is the backend refusing a queue it should
    // be serving, and swallowing it would leave the case to die later on a timeout
    // instead of on the message that was actually rejected.
    //
    // Not folded into negotiate(): almost every case in this file wants exactly one
    // queue, and building rings nobody drives would only add teardown to walk.
    bool setup_queue(uint32_t qid) {
        if (qid == 0 || !qid_valid(qid)) {
            errno = EINVAL;
            return fail("setup_queue takes qid 1..MQ_QUEUES; negotiate() already set up 0");
        }
        if (kickfd_of(qid) >= 0) {
            errno = EALREADY;
            return fail("queue already set up");
        }
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_NUM; m.size = sizeof(vhost_vring_state);
        m.payload.state = {qid, VQ_NUM};
        if (!settle(&m, nullptr, 0, /*check_ack=*/true)) return false;

        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_BASE; m.size = sizeof(vhost_vring_state);
        m.payload.state = {qid, 0};
        if (!settle(&m)) return false;

        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_ADDR; m.size = sizeof(vhost_vring_addr);
        m.payload.addr = vhost_vring_addr{qid, 0, (uint64_t)(mem + ring_desc_off(qid)),
                                          (uint64_t)(mem + ring_used_off(qid)),
                                          (uint64_t)(mem + ring_avail_off(qid)), 0};
        if (!settle(&m, nullptr, 0, /*check_ack=*/true)) return false;

        // Created before the two messages that hand them over: SCM_RIGHTS gives the
        // backend its own descriptor for the same open file description, so ours
        // stays usable as the kick source and the interrupt sink.
        int& kf = kickfd_of(qid);
        int& cf = callfd_of(qid);
        kf = ::eventfd(0, EFD_NONBLOCK);
        cf = ::eventfd(0, EFD_NONBLOCK);
        if (kf < 0 || cf < 0) return fail("eventfd");
        memset(&m, 0, sizeof(m));
        // The index rides in the low 8 bits of the u64 payload, not in a
        // vhost_vring_state: vhost-user-wire.h's VHOST_USER_VRING_IDX_MASK, with
        // VHOST_USER_VRING_NOFD_MASK above it, are the two halves of that u64. This
        // sends an fd, so the NOFD bit stays clear.
        m.request = VHOST_USER_SET_VRING_KICK; m.size = 8; m.payload.u64 = qid;
        if (!settle(&m, &kf, 1)) return false;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_CALL; m.size = 8; m.payload.u64 = qid;
        if (!settle(&m, &cf, 1)) return false;

        // ENABLE last, and through transact() rather than settle(), exactly as
        // negotiate() does for qid 0: the request is only sent once bit 30 is
        // settled, and NEED_REPLY is answered whether or not REPLY_ACK is.
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_ENABLE; m.size = sizeof(vhost_vring_state);
        m.payload.state = {qid, 1};
        if (!transact(&m, &r)) return false;
        return true;
    }

    uint64_t config_capacity() {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_CONFIG;
        m.size = offsetof(vhost_user_config, region) + sizeof(blk_config);
        m.payload.config.offset = 0;
        m.payload.config.size = sizeof(blk_config);
        if (!transact(&m, &r)) return UINT64_MAX;
        blk_config bc;
        memcpy(&bc, r.payload.config.region, sizeof(bc));
        return bc.capacity;
    }

    // seg_max as the wire reports it, so the value AND the offset it sits at are both
    // read from outside this process's idea of the struct. UINT32_MAX on a failed
    // transact, which no legal config can produce.
    uint32_t config_seg_max() {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_CONFIG;
        m.size = offsetof(vhost_user_config, region) + sizeof(blk_config);
        m.payload.config.offset = 0;
        m.payload.config.size = sizeof(blk_config);
        if (!transact(&m, &r)) return UINT32_MAX;
        blk_config bc;
        memcpy(&bc, r.payload.config.region, sizeof(bc));
        return bc.seg_max;
    }

    // size_max sits next to seg_max in the same struct, and this reads it for the same
    // reason: VIRTIO_BLK_F_SIZE_MAX (bit 1) is not offered, so the field has to stay 0
    // and a seg_max written one field early shows up here rather than nowhere.
    uint32_t config_size_max() {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_CONFIG;
        m.size = offsetof(vhost_user_config, region) + sizeof(blk_config);
        m.payload.config.offset = 0;
        m.payload.config.size = sizeof(blk_config);
        if (!transact(&m, &r)) return UINT32_MAX;
        blk_config bc;
        memcpy(&bc, r.payload.config.region, sizeof(bc));
        return bc.size_max;
    }

    // ---- request slots (batch submission) ----
    // The used ring reports a request by its descriptor HEAD, so the mock keeps
    // the head -> slot mapping to find the buffers a completion belongs to.
    int16_t slot_of_head[VQ_NUM] = {};
    // One map PER RING, because a head is an index into a descriptor array and every
    // queue has its own: head 0 exists on all of them. A single shared map would let
    // a completion the backend appended to the wrong used ring read back as the slot
    // this side meant, which is the exact defect the map exists to catch. The slots
    // themselves stay in one pool -- hdr/data/status are just guest buffers, and two
    // queues sharing a slot would be a real driver bug, not a backend one, so keeping
    // them distinct is the caller's job.
    int16_t mq_slot_of_head[MQ_QUEUES][VQ_NUM] = {};
    int16_t* slot_of_head_of(uint32_t qid) {
        return qid ? mq_slot_of_head[qid - 1] : slot_of_head;
    }
    uint16_t slot_seq = 0;

    static uint64_t hdr_off(uint16_t slot)    { return L_HDR    + (uint64_t)slot * 64; }
    static uint64_t data_off(uint16_t slot)   { return L_DATA   + (uint64_t)slot * DATA_SLOT; }
    static uint64_t status_off(uint16_t slot) { return L_STATUS + (uint64_t)slot * 64; }
    // A slot's indirect table. GPA, not QVA: connect_to() gives the region a GPA base
    // of 0, so a guest offset IS its GPA -- the same identity hdr_off, data_off and
    // status_off already rely on, and the opposite space from what SET_VRING_ADDR
    // carries.
    static constexpr uint64_t tbl_off(uint16_t slot) {
        return L_TBL + (uint64_t) slot * TBL_SLOT_BYTES;
    }

    // Where `qid`'s three rings live. qid 0 is the legacy triple the whole file
    // already reads, at the addresses it has always had; qid >= 1 gets the block
    // L_MQ reserves for it. The two event-index slots hang off these the same way
    // USED_EVENT_OFF and AVAIL_EVENT_OFF hang off L_AVAIL and L_USED. constexpr so
    // the two asserts that follow this struct can hold the generalisation to
    // account: moving queue 0 by accident would not break the multiqueue cases, it
    // would silently break every EVENT_IDX case in the file, and those all still
    // pass. They follow the struct rather than sitting here because a constexpr
    // member function is not usable in a constant expression until its enclosing
    // class is complete.
    static constexpr uint64_t ring_desc_off(uint32_t qid) {
        return qid ? L_MQ + (qid - 1) * MQ_STRIDE + MQ_DESC : L_DESC;
    }
    static constexpr uint64_t ring_avail_off(uint32_t qid) {
        return qid ? L_MQ + (qid - 1) * MQ_STRIDE + MQ_AVAIL : L_AVAIL;
    }
    static constexpr uint64_t ring_used_off(uint32_t qid) {
        return qid ? L_MQ + (qid - 1) * MQ_STRIDE + MQ_USED : L_USED;
    }
    static constexpr uint64_t used_event_off(uint32_t qid) {
        return ring_avail_off(qid) + 4 + 2 * VQ_NUM;
    }
    static constexpr uint64_t avail_event_off(uint32_t qid) {
        return ring_used_off(qid) + 4 + 8 * VQ_NUM;
    }

    // a chain never straddles the end of the descriptor array (a real driver
    // allocates from a free list and would refuse to split one). The bound has
    // to leave room for all THREE descriptors: reject a next head whose own
    // chain would not fit, i.e. head + 3 + 2 > VQ_NUM - 1. The old
    // `head + 3 >= VQ_NUM` still handed out 254 and 255, whose chains reach
    // 256/257 -- past the ring this mock declares in SET_VRING_NUM. Both sides
    // used to agree on that (the device indexed the descriptor array without a
    // bound, and the shared mapping made the over-read succeed), so it stayed
    // invisible until the device started checking.
    // Per queue: each ring has its own free list, so each has its own cursor.
    uint16_t alloc_head(uint32_t qid = 0) {
        uint16_t& cursor = desc_head_of(qid);
        uint16_t head = cursor;
        cursor = (uint16_t)(head + 6 > VQ_NUM ? 0 : head + 3);
        return head;
    }

    // An indirect request takes ONE ring slot, which is the whole point of the feature:
    // the same ring depth carries more requests, and a request can carry more buffers
    // than the ring is deep. A separate allocator rather than a parameter on
    // alloc_head(), because the two steps differ -- three there, one here -- and
    // folding them would put the three back into the indirect path by accident, which
    // is the shape of the defect the comment above alloc_head() records.
    uint16_t alloc_head_indirect(uint32_t qid = 0) {
        uint16_t& cursor = desc_head_of(qid);
        uint16_t head = cursor;
        cursor = (uint16_t)(head + 1 >= VQ_NUM ? 0 : head + 1);
        return head;
    }

    // build one request into `slot` and publish it to `qid`'s avail ring; no kick
    int submit(uint16_t slot, uint32_t type, uint64_t sector, uint32_t len, bool data_write,
               uint32_t qid = 0) {
        if (!qid_valid(qid)) { errno = EINVAL; fail("queue index"); return -1; }
        uint16_t head = alloc_head(qid);
        auto* desc = (vdesc*)(mem + ring_desc_off(qid));
        auto* avail = (vavail*)(mem + ring_avail_off(qid));
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

        uint16_t& ai = avail_idx_of(qid);
        avail->ring[ai % VQ_NUM] = head;
        __sync_synchronize();
        avail->idx = ++ai;
        __sync_synchronize();
        slot_of_head_of(qid)[head] = (int16_t)slot;
        return 0;
    }

    // Build one request as a single INDIRECT ring descriptor plus a table holding
    // header / data / status, and publish it to `qid`'s avail ring; no kick. The
    // completion path is NOT touched and must not be: slot_of_head, collect(), and
    // do_request's head check and used-len check all work off the RING head, which is
    // what the used element's id reports for an indirect request too. That those four
    // need no change is the observable half of "the used id and the used len are
    // unchanged by layout".
    //
    // `n_extra` hangs that many zero-length entries behind the status, chained, for the
    // over-cap case. They go AFTER the status rather than before it, so a table walked
    // correctly still ends at the status.
    int submit_indirect(uint16_t slot, uint32_t type, uint64_t sector, uint32_t len,
                        bool data_write, uint32_t n_extra = 0, uint32_t qid = 0) {
        if (!qid_valid(qid)) { errno = EINVAL; fail("queue index"); return -1; }
        // A table is built in the slot's own region, so it has to fit there; the one
        // case that needs more entries than a slot holds builds in L_TBL_BIG instead.
        if (3 + n_extra > TBL_PER_SLOT) {
            errno = E2BIG;
            fail("table does not fit its slot");
            return -1;
        }
        uint16_t head = alloc_head_indirect(qid);
        auto* desc = (vdesc*)(mem + ring_desc_off(qid));
        auto* avail = (vavail*)(mem + ring_avail_off(qid));
        auto* hdr = (blk_outhdr*)(mem + hdr_off(slot));
        hdr->type = type;
        hdr->ioprio = 0;
        hdr->sector = sector;
        *(uint8_t*)(mem + status_off(slot)) = 0xff;

        auto* t = (vdesc*)(mem + tbl_off(slot));
        const uint32_t n = 3 + n_extra;
        t[0] = vdesc{hdr_off(slot), sizeof(blk_outhdr), DESC_F_NEXT, 1};
        t[1] = vdesc{data_off(slot), len,
                     (uint16_t)(DESC_F_NEXT | (data_write ? DESC_F_WRITE : 0)), 2};
        t[2] = vdesc{status_off(slot), 1,
                     (uint16_t)(DESC_F_WRITE | (n_extra ? DESC_F_NEXT : 0)),
                     (uint16_t)(n_extra ? 3 : 0)};
        for (uint32_t i = 0; i < n_extra; i++)
            t[3 + i] = vdesc{data_off(slot), 0,
                             (uint16_t)(i + 1 < n_extra ? DESC_F_NEXT : 0),
                             (uint16_t)(i + 4)};
        // ONE ring descriptor, carrying the flag and no NEXT.
        desc[head] = vdesc{tbl_off(slot), n * (uint32_t)sizeof(vdesc), DESC_F_INDIRECT, 0};

        uint16_t& ai = avail_idx_of(qid);
        avail->ring[ai % VQ_NUM] = head;
        __sync_synchronize();
        avail->idx = ++ai;
        __sync_synchronize();
        slot_of_head_of(qid)[head] = (int16_t)slot;
        return 0;
    }

    bool kick(uint32_t qid = 0) {
        if (!qid_valid(qid)) { errno = EINVAL; return fail("queue index"); }
        uint64_t one = 1;
        if (::write(kickfd_of(qid), &one, 8) != 8) { errno = EIO; return fail("kick"); }
        return true;
    }

    // ---- event index: the mock is the driver, so it owns used_event and reads
    // ---- avail_event. Both predicates are rewritten here rather than shared
    // ---- with blk/utils.h -- see the file's standing rule.
    // ---- Both slots are per ring (§2.7.7.2 and §2.7.10.1 are stated per
    // ---- virtqueue), so every accessor here takes the queue index.

    void set_used_event(uint16_t v, uint32_t qid = 0) {
        __sync_synchronize();
        *(uint16_t*)(mem + used_event_off(qid)) = v;
    }
    uint16_t get_used_event(uint32_t qid = 0) { return *(uint16_t*)(mem + used_event_off(qid)); }
    uint16_t get_avail_event(uint32_t qid = 0) {
        __sync_synchronize();
        return *(uint16_t*)(mem + avail_event_off(qid));
    }

    // ---- the regime selector, and the flags-mode half of the mock's side of the
    // ---- ring: it owns avail->flags and reads used->flags, which is the mirror
    // ---- image of owning used_event and reading avail_event above.

    // Which regime this session settled on, read from `features` -- the word
    // negotiate() settled and the mock's own record of it, never from anything the
    // device published. That is the point: a backend that derived its per-queue
    // state from the wrong word cannot make the mock agree with it, so the two
    // disagreeing stays observable instead of becoming a shared mistake. A case
    // that re-sends SET_FEATURES behind the mock's back leaves this stale, and
    // stale is the safe direction -- it keeps the mock publishing used_event,
    // which a flags-mode device ignores rather than acts on.
    bool event_idx_negotiated() const { return (features & F_RING_EVENT_IDX) != 0; }

    // Set or clear the driver's interrupt-suppression bit. §2.7.7.2's "the device
    // MUST ignore the lower bit of flags" is conditional on bit 29 having been
    // negotiated, so in a flags-mode session this is the only channel the guest has
    // for telling the device not to interrupt, and blk/utils.cpp's vring_need_irq is
    // the code that reads it.
    void set_avail_no_interrupt(bool on, uint32_t qid = 0) {
        auto* avail = (vavail*)(mem + ring_avail_off(qid));
        avail->flags = on ? (uint16_t)(avail->flags | AVAIL_F_NO_INTERRUPT)
                          : (uint16_t)(avail->flags & ~AVAIL_F_NO_INTERRUPT);
        // After the store, not before: what has to be ordered is this flag against
        // the avail->idx publish that follows in submit(), and a barrier ahead of
        // the store would order nothing that matters. It pairs with the SEQ_CST
        // fence should_notify() runs before reading the flag back.
        __sync_synchronize();
    }

    // §2.7.10.1: notify iff the index that determined where the descriptor
    // landed equals avail_event, and the used ring's flags low bit is clear.
    // `submit()` has already incremented avail_idx, so the index that picked the
    // slot is avail_idx - 1; the uint16 form below is that equality written to
    // survive wraparound. Both the flags it reads and the avail_event it consults
    // belong to `qid`'s own ring.
    bool kick_if_needed(uint32_t qid = 0) {
        if (!qid_valid(qid)) { errno = EINVAL; return fail("queue index"); }
        if (kickfd_of(qid) < 0)
            return true;   // no kick fd (revoked by a NOFD SET_VRING_KICK): there
                           // is nothing to notify with, so the device can only
                           // find the work by its own fallback re-scan
        auto* used = (vused*)(mem + ring_used_off(qid));
        uint16_t flags = used->flags;
        uint16_t idx = (uint16_t)(avail_idx_of(qid) - 1);
        __sync_synchronize();
        // Without bit 29 there is no avail_event to consult, and consulting it
        // anyway is not merely redundant: publish_avail_event() writes that slot
        // only when the engine is in EVENT_IDX mode, so a flags-mode device leaves
        // it at its memset 0 and the equality below fires on the first kick alone.
        // Every later request would then ride the device's 5 ms fallback re-scan --
        // still served, so nothing would fail, which is precisely why the branch
        // cannot be left to the conjunction above. §2.7.10.1 tells the driver to
        // ignore the used ring's flags low bit only once bit 29 is negotiated, so
        // without it that bit is the whole decision -- and it is provably clear,
        // because nothing in blk/ ever writes used->flags while §2.7.10.1 has this
        // side initialize it to 0. Tested rather than folded into a constant so a
        // device that started setting it shows up here instead of as a hot-spinning
        // kick loop.
        if (!event_idx_negotiated())
            return (flags & USED_F_NO_NOTIFY) ? true : kick(qid);
        uint16_t ae = get_avail_event(qid);
        if ((flags & USED_F_NO_NOTIFY) == 0 && (uint16_t)(idx - ae) < 1)
            return kick(qid);
        return true;
    }

    // strict poll: does NOT consume. collect() drains, so it cannot be used to
    // assert that a notification did or did not happen.
    bool callfd_readable(int ms, uint32_t qid = 0) {
        pollfd pfd{callfd_of(qid), POLLIN, 0};
        return ::poll(&pfd, 1, ms) > 0;
    }
    // eventfd accumulates, so one read returns the total since the last read
    uint64_t callfd_drain(uint32_t qid = 0) {
        uint64_t v = 0;
        while (::read(callfd_of(qid), &v, 8) == 8) {}
        return v;
    }

    // Did the device serve `slot`, watched through the status byte submit() presets
    // to 0xff. This and not collect(), for the reason interrupt_suppressed_by_used_event
    // gives: collect() polls the used ring and drains the callfd, so a case whose
    // subject IS the interrupt cannot use it without destroying its own oracle.
    // The bound is generous and can be: the device writes the status byte inside
    // serve_chain and then appends the used element and signals in complete_req,
    // with no yield between the append and the signal -- so once the byte is
    // visible the interrupt is a few non-yielding instructions away, and the
    // callfd_readable() that follows this needs no slack of its own.
    bool served_within(uint16_t slot, int ms = 2000) {
        for (int i = 0; i < ms; i++) {
            if (*(uint8_t*)(mem + status_off(slot)) != 0xff)
                return true;
            ::usleep(1000);
        }
        return false;
    }

    // wait for the next used element (completions come back in ARBITRARY order).
    // Check the ring BEFORE polling: one eventfd read consumes the whole
    // accumulated counter, so a poll-first loop would burn a full timeout on
    // every completion after the first.
    // Both outputs are written only on the success path, which gcc cannot see
    // across the call, so every caller initialises the locals it passes in.
    bool collect(uint32_t* head_out, uint32_t* len_out, int ms = 20000, uint32_t qid = 0) {
        if (!qid_valid(qid)) { errno = EINVAL; return fail("queue index"); }
        auto* used = (vused*)(mem + ring_used_off(qid));
        uint16_t& ui = used_idx_of(qid);
        for (int i = 0; i <= ms / 10; i++) {
            // A conformant driver keeps used_event at the index it has consumed
            // to, so §2.7.7.2's equality fires on the next element the device
            // appends (old == U, new == U+1 -> (U+1-U-1)=0 < 1 -> notify), and
            // also when the device appends several at once (old == U, new == U+3
            // -> 2 < 3 -> notify). Without this the slot stays at its memset 0,
            // the device stops interrupting after the first completion, and this
            // loop still finds every result by polling the ring -- a silent
            // false green for the whole suite.
            //
            // Conditional on bit 29 because writing the slot in a flags-mode
            // session supplies a side-channel no real flags-mode frontend writes.
            // That is not a tidiness point: an unconditional write here is what
            // let a backend deriving its per-queue event_idx from the OFFER rather
            // than from the NEGOTIATED word go undetected, because the used_event
            // it then read was the one this line had just put there. With the
            // write gated, a flags-mode session leaves the slot at its memset 0
            // and the device that reads it anyway suppresses every interrupt past
            // the first -- which is what a_flags_mode_session_notifies_from_avail_flags_alone
            // asserts on.
            if (event_idx_negotiated()) {
                set_used_event(ui, qid);
                __sync_synchronize();
            }
            if (used->idx != ui) {
                auto elem = used->ring[ui % VQ_NUM];
                ui++;
                *head_out = elem.id;
                *len_out = elem.len;
                return true;
            }
            pollfd pfd{callfd_of(qid), POLLIN, 0};
            int pr = ::poll(&pfd, 1, 10);
            if (pr > 0) {
                uint64_t v;
                while (::read(callfd_of(qid), &v, 8) == 8) {}
            }
        }
        errno = ETIMEDOUT;
        return fail("used ring");
    }

    // `used_len` is optional and defaults off, so the four wrappers below and
    // every existing caller are untouched. Written as soon as a completion comes
    // back and NOT only on the success path, because a FAILED completion's length
    // is the one the length check below deliberately skips -- and it is the only
    // thing that says how much of the guest's device-writable buffer the device
    // claims to have filled. As with collect()'s own outputs, it is left alone
    // when no completion arrived, so a caller initialises the local it passes in.
    // `qid` defaults to 0 for the same reason and in the same shape.
    int do_request(uint32_t type, uint64_t sector, void* data, size_t len, bool data_write,
                   uint32_t* used_len = nullptr, uint32_t qid = 0, bool indirect = false) {
        if (!qid_valid(qid)) { errno = EINVAL; fail("queue index"); return -1; }
        if (len > DATA_SLOT) { errno = E2BIG; fail("len > DATA_SLOT"); return -1; }
        uint16_t slot = (uint16_t)(slot_seq++ % SLOTS);
        if (data && len && !data_write) memcpy(mem + data_off(slot), data, len);
        // Only the SUBMIT half differs. Everything below -- the head check, the used-len
        // check, the status read -- is shared, which is the observable half of "the used
        // id and the used len are unchanged by layout": a second completion path here
        // would be a place for a layout change to hide.
        if (indirect)
            submit_indirect(slot, type, sector, (uint32_t)len, data_write, 0, qid);
        else
            submit(slot, type, sector, (uint32_t)len, data_write, qid);
        // a conformant driver: kick only when §2.7.10.1 says to. The
        // unconditional kick() stays for cases that are not about notification.
        if (!kick_if_needed(qid)) return -1;

        uint32_t head = 0, ulen = 0;
        if (!collect(&head, &ulen, 20000, qid)) return -1;
        if (used_len) *used_len = ulen;
        if (head >= VQ_NUM || slot_of_head_of(qid)[head] != (int16_t)slot) {
            errno = EPROTO;
            fail("used elem id");
            return -1;
        }
        // virtio-blk requires the used element's len to reflect what the device
        // actually wrote. For successful reads and GET_IDs that is data + status;
        // for successful writes and flushes it is just the status byte. Error
        // responses may write only the status byte regardless of request type,
        // so the length check applies only to successful completions.
        uint8_t st = *(uint8_t*)(mem + status_off(slot));
        if (st == 0) {
            uint32_t expected = (type == T_IN || type == T_GET_ID) ? (uint32_t)len + 1 : 1;
            if (ulen != expected) {
                errno = EPROTO;
                fail("used elem len");
                return -1;
            }
        }
        if (data && len && data_write) memcpy(data, mem + data_off(slot), len);
        return *(uint8_t*)(mem + status_off(slot));
    }

    int write_dev(uint64_t off, const void* buf, size_t len, uint32_t* used_len = nullptr,
                  uint32_t qid = 0) {
        return do_request(T_OUT, off >> 9, (void*)buf, len, false, used_len, qid);
    }
    int read_dev(uint64_t off, void* buf, size_t len, uint32_t qid = 0) {
        return do_request(T_IN, off >> 9, buf, len, true, nullptr, qid);
    }
    int flush_dev() {
        return do_request(T_FLUSH, 0, nullptr, 0, false);
    }
    // `len` is the writable buffer this guest offers, which for a GET_ID must be the
    // whole fixed-width field: the device fills all of it and NUL-pads past the serial.
    int get_id(char* buf, size_t len) {
        return do_request(T_GET_ID, 0, buf, len, true);
    }

    // ---- raw and malformed messages ----
    // Everything below exists to send what a well-behaved frontend never would.
    // negotiate() and submit() only build valid sequences, which is precisely why
    // the device's rejection paths had never executed: a guard that never runs is
    // indistinguishable from a guard that is not there.
    // These all address vring index 0 and stay that way: each one is about a single
    // malformed message, and the index bound has its own case
    // (vring_index_out_of_range_is_rejected). setup_queue() above is the only thing
    // here that builds a per-queue sequence.

    // transact() checks that a REPLY_ACK arrived, and with check_ack also that
    // the ack payload is 0 (accepted). Left at its default it does not look at
    // the payload, so a non-zero ack -- rejected -- is the caller's to interpret.
    bool set_vring_num(uint32_t n, uint64_t* ack) {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_NUM; m.size = sizeof(vhost_vring_state);
        m.payload.state = {0, n};
        if (!transact(&m, &r)) return false;
        *ack = r.payload.u64;
        return true;
    }

    // same shape as set_vring_num above: the reply carries the REPLY_ACK payload
    bool set_vring_base(uint32_t base, uint64_t* ack) {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_BASE; m.size = sizeof(vhost_vring_state);
        m.payload.state = {0, base};
        if (!transact(&m, &r)) return false;
        *ack = r.payload.u64;
        return true;
    }

    bool set_vring_enable(bool on) {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_ENABLE; m.size = sizeof(vhost_vring_state);
        m.payload.state = {0, (uint32_t)(on ? 1 : 0)};
        return transact(&m, &r);
    }

    // stop the vq, move its base, start it again. Used by the two cases that
    // need a ring whose used_idx does not start at 0 -- a fresh setup() cannot
    // produce one, and both handover and uint16 wraparound need it.
    bool restart_with_base(uint16_t base) {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_ENABLE; m.size = sizeof(vhost_vring_state);
        m.payload.state = {0, 0};
        if (!transact(&m, &r)) return false;
        uint64_t ack = 0;
        if (!set_vring_base(base, &ack)) return false;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_ENABLE; m.size = sizeof(vhost_vring_state);
        m.payload.state = {0, 1};
        return transact(&m, &r);
    }

    bool set_vring_addr(uint64_t desc_qva, uint64_t avail_qva, uint64_t used_qva, uint64_t* ack) {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_ADDR; m.size = sizeof(vhost_vring_addr);
        m.payload.addr = vhost_vring_addr{0, 0, desc_qva, used_qva, avail_qva, 0};
        if (!transact(&m, &r)) return false;
        *ack = r.payload.u64;
        return true;
    }
    bool restore_vring_addr(uint64_t* ack) {
        return set_vring_addr((uint64_t)(mem + L_DESC), (uint64_t)(mem + L_AVAIL),
                              (uint64_t)(mem + L_USED), ack);
    }

    // declare `nregions` but attach only `nfds` of them
    // Send a SET_MEM_TABLE whose three numbers disagree, so each of the handler's
    // agreement checks can be violated on its own: `nregions` is what the payload
    // declares, `nfds` is how many descriptors actually travel with it, and
    // `payload_regions` is how many of them the declared length covers. All the fds
    // are the same memfd -- nothing here is meant to be mapped, only rejected --
    // and the region itself is the mock's real one, so a message that got as far as
    // mapping would succeed and the rejection is the only thing under test.
    bool set_mem_table_mismatched(uint32_t nregions, int nfds, uint64_t* ack,
                                  uint32_t payload_regions = 1) {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_MEM_TABLE;
        m.size = offsetof(vhost_user_memory, regions) + payload_regions * sizeof(vhost_user_memory_region);
        m.payload.memory.nregions = nregions;
        m.payload.memory.regions[0] = vhost_user_memory_region{0, MEM_SIZE, (uint64_t)mem, 0};
        int fds[8];
        for (int i = 0; i < nfds && i < 8; i++) fds[i] = memfd;
        if (!transact(&m, &r, fds, nfds)) return false;
        *ack = r.payload.u64;
        return true;
    }

    // Send a SET_MEM_TABLE that declares `nregions` but maps one of them with a
    // length of zero, which is the only way to make the backend's mmap fail from
    // this side of a socket: every descriptor arriving over SCM_RIGHTS is a valid
    // one by construction, and an offset past the end of a memfd still maps. The
    // count, the fd count and the payload length all agree, so this reaches the
    // mapping and nothing else.
    bool set_mem_table_unmappable(uint32_t nregions, uint64_t* ack) {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_MEM_TABLE;
        m.size = offsetof(vhost_user_memory, regions) + nregions * sizeof(vhost_user_memory_region);
        m.payload.memory.nregions = nregions;
        m.payload.memory.regions[0] = vhost_user_memory_region{0, 0, (uint64_t)mem, 0};
        int fds[8];
        for (uint32_t i = 0; i < nregions && i < 8; i++) fds[i] = memfd;
        if (!transact(&m, &r, fds, (int)nregions)) return false;
        *ack = r.payload.u64;
        return true;
    }

    // Send a request whose declared payload is SHORTER than the fields its handler
    // reads, without waiting for a reply -- the point is that none comes. `size`
    // bytes of the zeroed union go out after the header, which is what a truncated
    // message looks like on the wire and, before recv_msg grew a minimum, exactly
    // what a well-formed message declaring zero also looked like.
    bool send_truncated(int32_t request, uint32_t size) {
        vhost_user_msg m;
        memset(&m, 0, sizeof(m));
        m.request = request;
        m.flags = VHOST_USER_NEED_REPLY_MASK;   // send() adds the version
        m.size = size;
        return send(&m);
    }

    // Wait for the peer to close, bounded, and report WHICH outcome arrived: 0 on
    // a close, EPROTO if a reply came instead, ETIMEDOUT if the session simply
    // stayed up. A refusal that ends the session and one that error-acks are
    // different behaviours and a test for one must not accept the other, so "no
    // reply yet" is not the answer -- and the wait has to be bounded, because a
    // backend stuck reading a payload that never arrives also sends no reply and
    // would otherwise be indistinguishable from one that closed.
    int expect_eof(int ms = 5000) {
        for (int waited = 0; waited < ms; waited += 100) {
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, 100) < 0) return errno;
            char b;
            ssize_t r = ::recv(fd, &b, 1, MSG_DONTWAIT);
            if (r == 0) return 0;
            if (r > 0) return EPROTO;
            // ECONNRESET is the peer closing too, and not a second outcome to
            // distinguish: a refusal that rejects a message WITHOUT reading its
            // payload leaves those bytes unread, and closing a socket with unread
            // data makes the kernel send RST where it would otherwise send FIN. So
            // which of the two this sees depends only on whether the rejected
            // message declared a payload at all -- measured, not assumed: the rows
            // declaring zero bytes read as a clean EOF and the rows declaring four
            // or eight read as ECONNRESET. Both mean the session is over; only a
            // reply means it is not.
            if (errno == ECONNRESET) return 0;
            if (errno != EAGAIN) return errno;
        }
        return ETIMEDOUT;
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
        uint32_t got = 0;
        if (!collect(&got, used_len)) return -1;
        if (got != head) { errno = EPROTO; fail("used elem id"); return -1; }
        return *(uint8_t*)(mem + status_off(slot));
    }

    // append `head` to `qid`'s avail ring without building a chain
    void publish(uint16_t head, uint32_t qid = 0) {
        auto* avail = (vavail*)(mem + ring_avail_off(qid));
        uint16_t& ai = avail_idx_of(qid);
        avail->ring[ai % VQ_NUM] = head;
        __sync_synchronize();
        avail->idx = ++ai;
        __sync_synchronize();
    }

    uint16_t used_idx_now(uint32_t qid = 0) {
        __sync_synchronize();
        return ((vused*)(mem + ring_used_off(qid)))->idx;
    }
    // Wait for the used ring to advance by `n`. Measured on the free-running
    // 16-bit index, NOT by counting elements: the device may legitimately lap
    // the 256-entry ring when more than VQ_NUM completions are outstanding.
    bool wait_used_advance(uint16_t from, uint16_t n, int ms, uint32_t qid = 0) {
        for (int i = 0; i <= ms / 10; i++) {
            if ((uint16_t)(used_idx_now(qid) - from) >= n)
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
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_KICK; m.size = 8; m.payload.u64 = 0;
        if (!transact(&m, &r, &nfd, 1)) { ::close(nfd); return false; }
        if (kickfd >= 0) ::close(kickfd);
        kickfd = nfd;   // SCM_RIGHTS gave the device its own descriptor
        return true;
    }

    // Same shape for the interrupt fd, but OUR descriptor for the old one is
    // handed back instead of closed: it still refers to the same eventfd, so a
    // caller can read the counter the device left in it and tell which of the two
    // descriptors a completion was signalled on. The device closes its own
    // descriptor for the old one either way, and the destructor now owns only the
    // replacement.
    bool replace_callfd(int* old_callfd) {
        int nfd = ::eventfd(0, EFD_NONBLOCK);
        if (nfd < 0) return fail("eventfd");
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_CALL; m.size = 8; m.payload.u64 = 0;
        if (!transact(&m, &r, &nfd, 1)) { ::close(nfd); return false; }
        *old_callfd = callfd;
        callfd = nfd;   // SCM_RIGHTS gave the device its own descriptor
        return true;
    }

    // Revoke the kick fd the protocol's way: SET_VRING_KICK with the NOFD flag
    // and no fd attached. The device closes what it had and is left with none, so
    // from here on it can only find work by its own fallback re-scan -- which is
    // what a NOFD kick means. Our own end goes too, so submit() cannot kick.
    bool drop_kickfd_nofd() {
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_VRING_KICK; m.size = 8;
        m.payload.u64 = 0 | VHOST_USER_VRING_NOFD_MASK;   // idx 0, no fd
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
        for (auto& q : mq) {
            if (q.kickfd >= 0) ::close(q.kickfd);
            if (q.callfd >= 0) ::close(q.callfd);
        }
        if (backend_fd >= 0) ::close(backend_fd);
        if (mem) ::munmap(mem, MEM_SIZE);
        if (memfd >= 0) ::close(memfd);
    }
};

// The generalisation's two invariants, stated where MockFrontend is complete so the
// constexpr accessors above are usable in a constant expression. Both are about
// qid 0: the per-queue rings were added beside it, not underneath it, and every
// pre-existing case in this file still addresses the legacy triple by name.
static_assert(MockFrontend::ring_desc_off(0) == L_DESC &&
              MockFrontend::ring_avail_off(0) == L_AVAIL &&
              MockFrontend::ring_used_off(0) == L_USED,
              "qid 0 must keep the legacy ring addresses");
static_assert(MockFrontend::used_event_off(0) == USED_EVENT_OFF &&
              MockFrontend::avail_event_off(0) == AVAIL_EVENT_OFF,
              "and its two event-index slots");

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

    // a second object must not steal a LIVE backend's socket path. It is refused with
    // EEXIST and not EBUSY: do_listen() binds and reports what bind() said, and it no
    // longer connects to the node to ask whether a live backend is behind it -- that
    // answer never changed the refusal, and getting it cost a window in which the
    // probe could be wrong. The failed start's rollback must leave dev's socket file
    // in place, which is the half that was always the point.
    auto dev2 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    errno = 0;
    EXPECT_EQ(-1, dev2->start(file));
    EXPECT_EQ(EEXIST, errno);
    EXPECT_EQ(0, ::access(SOCK_PATH, F_OK));

    EXPECT_EQ(0, dev->shutdown());
    EXPECT_NE(0, ::access(SOCK_PATH, F_OK));   // SERVER shutdown unlinks
}

// The capability half of BlkDevInfo: what lets a caller branch on behaviour instead of
// inferring it from which factory built the object. Every axis pinned here is a property
// of the transport, so all of them are already correct on a constructed device, before
// any frontend exists.
TEST_F(VhostUserTest, capabilities_descriptor) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    const BlkDevInfo& i = dev->get_info();

    // The rings live in the frontend's memory and detach() closes the connection, so
    // whatever survives a detach is held by the peer and comes back only when it
    // reconnects -- there is nothing on this side for a later start() to harvest.
    // shutdown() is detach(true) plus unlinking the socket: it ends the session rather
    // than refusing it. A resize() that cannot deliver its config-change notification
    // still returns success, which is what BestEffortNotify records. And there is no
    // kernel-side registration, so there is nothing to adopt and nothing to drift.
    EXPECT_EQ(BlkBacklog::PeerSide, i.backlog);
    EXPECT_EQ(BlkShutdownRefusal::Disconnects, i.shutdown_refusal);
    EXPECT_EQ(BlkResizeEffect::BestEffortNotify, i.resize_effect);
    EXPECT_EQ(BlkAdoption::NoRegistration, i.adoption);
    // stop_session() drains the requests it already dispatched on BOTH paths; only the
    // avail backlog is what wait_pending controls
    EXPECT_EQ(false, i.detach_no_wait);

    // This transport's virtio command set serves IN/OUT/FLUSH/GET_ID, so FLUSH is all it
    // can offer -- and it accepts the other two in a config without serving them. That
    // gap is the point of keeping `offered` independent of what was requested.
    EXPECT_EQ(FEATURE_FLUSH, i.offered);
    EXPECT_EQ(0ull, i.negotiated);   // no frontend has settled anything yet
}

// The requested/offered split as a caller sees it: asking for DISCARD and WRITE_ZEROES
// is accepted at construction, and the descriptor is what says they will not be served.
TEST_F(VhostUserTest, offered_exposes_requests_the_transport_cannot_honour) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.info.features = FEATURE_FLUSH | FEATURE_DISCARD | FEATURE_WRITE_ZEROES;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);   // accepted: validate_info checks geometry, not features
    DEFER(delete dev);
    const BlkDevInfo& i = dev->get_info();
    EXPECT_EQ(FEATURE_FLUSH, i.offered);
    EXPECT_EQ(cfg.info.features, i.features);   // the request is recorded, not rewritten
    EXPECT_EQ(FEATURE_DISCARD | FEATURE_WRITE_ZEROES, i.features & ~i.offered);
}

// blk.h's start() contract, the half test::CountingFile exists to witness: an OWNED
// backend is deleted on shutdown, not only by the destructor. No frontend takes part
// -- this is about the pointer, not about serving.
TEST_F(VhostUserTest, shutdown_releases_a_backend_it_owns) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    std::atomic<int> destroyed{0};
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), /*ownership=*/true));
    EXPECT_EQ(0, destroyed.load());
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_EQ(1, destroyed.load());

    // Usable again, which is the transition the leak hid behind: a second start()
    // serves a second backend instead of overwriting a pointer to a live one.
    ASSERT_EQ(0, dev->start(new test::CountingFile(file, &destroyed), true));
    EXPECT_EQ(1, destroyed.load());
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_EQ(2, destroyed.load());

    // An UNOWNED backend stays the caller's to delete, through shutdown and through
    // the destructor alike.
    fs::IFile* mine = new test::CountingFile(file, &destroyed);
    ASSERT_EQ(0, dev->start(mine));
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_EQ(2, destroyed.load());
    delete mine;
    EXPECT_EQ(3, destroyed.load());
}

// detach(true) deliberately retains backend and own_backend, so the next start()
// can be handed the SAME pointer while the device still owns it. The release that
// guards a start() against overwriting an owned pointer must not fire on that one:
// deleting the backend and storing the freed pointer back as owned leaves a
// dangling pointer that the next release deletes again. The counter is what tells
// the two apart -- exactly one destruction, at the shutdown that ends the session.
TEST_F(VhostUserTest, repassing_an_owned_backend_after_detach_never_deletes_it_twice) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);

    std::atomic<int> destroyed{0};
    auto* bk = new test::CountingFile(file, &destroyed);
    ASSERT_EQ(0, dev->start(bk, /*ownership=*/true));
    ASSERT_EQ(0, dev->detach(true));   // retains backend AND own_backend
    // detach left the socket node behind and bind() refuses it, so the dead node
    // goes first -- the product's own recovery path (destroy_orphan refuses a live
    // one), not a test-only unlink.
    BlkDevInfo orphan;
    orphan.identity = SOCK_PATH;
    ASSERT_EQ(0, ctl->destroy_orphan(orphan));
    ASSERT_EQ(0, dev->start(bk, /*ownership=*/true));   // the SAME pointer again
    EXPECT_EQ(0, destroyed.load()) << "a re-passed backend was deleted on re-entry";
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_EQ(1, destroyed.load()) << "the owned backend must die exactly once";
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

// #208: the serial a guest reads back must identify THIS device, not the transport.
// A fixed per-transport string made every device one daemon serves report the same
// serial to its guest, which is what a guest uses to tell two disks apart. The
// identity hashed is the FULL socket path: a basename is unique only inside one flat
// directory, and two controllers may legally scope /scope-a/disk.sock and
// /scope-b/disk.sock, which a basename hash answered with one ID -- the two-scope
// case right below pins the difference. The value is spelled out rather than derived
// from SOCK_PATH: deriving it would repeat the rule the implementation uses and so
// agree with itself if the rule were wrong. What the fixed-width fill itself does --
// all 20 bytes written, NUL past the end, used length covering the field -- is
// witnessed against the shared engine in test-blk-vq.cpp, so this case is about the
// value the transport supplies to it.
TEST_F(VhostUserTest, get_id_reports_this_device_not_the_transport) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    // GET_ID returns an FNV-1a hash of the identity, not the raw path
    char want[ID_BYTES] = {};
    {
        uint64_t h = 14695981039346656037ULL;
        for (const char* p = "/tmp/photon-blk-vhu-dir/vhu.sock"; *p; p++) {
            h ^= (uint8_t)*p;
            h *= 1099511628211ULL;
        }
        snprintf(want, sizeof(want), "%016llx", (unsigned long long)h);
    }
    char got[ID_BYTES];
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        memset(got, 0xA5, sizeof(got));   // so an unwritten tail cannot read as NUL
        if (fe.get_id(got, sizeof(got)) != S_OK) return EIO;
        return 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0, memcmp(want, got, strlen(want)));
}

// Two scopes, one basename. inside() confines each controller to its own directory
// and nothing else separates the two, so <SOCK_DIR>-a/same.sock and
// <SOCK_DIR>-b/same.sock are both legal and both can be served by one daemon. The
// GET_ID answer is the whole of what a guest has to tell two disks apart, so the
// two IDs must differ -- under a basename identity they were byte-identical. No
// value is pinned here on purpose: the case above pins what one full path hashes
// to, and this one only asks the two answers to be distinct, which is the property
// the choice of identity exists for.
TEST_F(VhostUserTest, get_id_separates_two_scopes_sharing_a_basename) {
    char dir_a[96], dir_b[96], path_a[128], path_b[128];
    snprintf(dir_a, sizeof(dir_a), "%s-a", SOCK_DIR);
    snprintf(dir_b, sizeof(dir_b), "%s-b", SOCK_DIR);
    snprintf(path_a, sizeof(path_a), "%s/same.sock", dir_a);
    snprintf(path_b, sizeof(path_b), "%s/same.sock", dir_b);
    if (::mkdir(dir_a, 0755) != 0) {
        ASSERT_EQ(EEXIST, errno) << dir_a;
    }
    if (::mkdir(dir_b, 0755) != 0) {
        ASSERT_EQ(EEXIST, errno) << dir_b;
    }
    // Swept rather than named: a started device leaves its identity lock beside
    // the socket, and a sweep does not have to know what either is called.
    DEFER({
        for (const char* d : {dir_a, dir_b}) {
            if (DIR* dd = ::opendir(d)) {
                struct dirent* e;
                char p[PATH_MAX];
                while ((e = readdir(dd))) {
                    if (e->d_name[0] == '.') continue;
                    snprintf(p, sizeof(p), "%s/%s", d, e->d_name);
                    ::unlink(p);
                }
                ::closedir(dd);
            }
            ::rmdir(d);
        }
    });

    auto ctl_a = new_vhost_user_controller(dir_a);
    ASSERT_NE(nullptr, ctl_a);
    DEFER(delete ctl_a);
    auto ctl_b = new_vhost_user_controller(dir_b);
    ASSERT_NE(nullptr, ctl_b);
    DEFER(delete ctl_b);

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = path_a;
    auto dev_a = ctl_a->new_device(cfg);
    ASSERT_NE(nullptr, dev_a);
    DEFER(delete dev_a);
    cfg.sock_path = path_b;
    auto dev_b = ctl_b->new_device(cfg);
    ASSERT_NE(nullptr, dev_b);
    DEFER(delete dev_b);
    // One backend, two devices: GET_ID never touches it, so sharing cannot leak
    // into either answer -- only the socket path can.
    ASSERT_EQ(0, dev_a->start(file));
    DEFER(dev_a->shutdown());
    ASSERT_EQ(0, dev_b->start(file));
    DEFER(dev_b->shutdown());

    char id_a[ID_BYTES] = {}, id_b[ID_BYTES] = {};
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(path_a)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.get_id(id_a, sizeof(id_a)) != S_OK) return EIO;
        return 0;
    });
    ASSERT_EQ(0, rc) << "scope a: " << path_a;
    rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(path_b)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.get_id(id_b, sizeof(id_b)) != S_OK) return EIO;
        return 0;
    });
    ASSERT_EQ(0, rc) << "scope b: " << path_b;
    EXPECT_NE(0, memcmp(id_a, id_b, ID_BYTES))
        << "two scopes sharing a basename reported one guest-visible ID";
}

// The control for the case below, and the reason that case's count is a
// measurement rather than a blind instrument. FLUSH is accepted here, so a write
// is not persisted on its own account and the FLUSH the frontend sends afterwards
// is what asks for it: exactly one sync for the two requests. Had write-through
// been switched on by mistake, the write would have added a second.
TEST_F(VhostUserTest, accepting_flush_leaves_the_device_in_write_back) {
    test::BackendProbe probe(file);
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());

    auto wbuf = pattern(0x5a);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        // A precondition, not a claim about the device: make_info() offers FLUSH,
        // and declining a bit that was never offered would change nothing -- which
        // is exactly what would make the case below stop testing the negotiated
        // word while still passing.
        if (!(fe.features & F_BLK_FLUSH)) return EPROTONOSUPPORT;
        if (fe.write_dev(1 << 20, wbuf.data(), wbuf.size()) != S_OK) return EIO;
        if (fe.flush_dev() != S_OK) return EIO;
        return 0;
    });
    ASSERT_EQ(0, rc);
    // The caller's view of the same settlement: the descriptor reports the word this
    // frontend accepted, which is the word that decided write-back above. Asserted here
    // rather than inside the frontend because SET_FEATURES is only guaranteed processed
    // once the requests that follow it have been served.
    EXPECT_EQ(FEATURE_FLUSH, dev->get_info().negotiated);
    EXPECT_EQ(0, verify_backend(1 << 20, wbuf));
    EXPECT_EQ(1, probe.datasyncs.load());   // the FLUSH's, not the write's
    EXPECT_EQ(0, probe.syncs.load());
}

// The same device and the same offer, with one difference: the frontend declines
// FLUSH. A driver that did so has no command with which to ask for persistence, so
// the device has to make the write durable before it completes it. The count is the
// whole oracle here -- the bytes that land are identical in both modes, and the
// engine-level cases in test-blk-vq are what pin the flag's meaning; this one pins
// where the flag comes from.
TEST_F(VhostUserTest, declining_flush_puts_the_device_in_write_through) {
    test::BackendProbe probe(file);
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());

    auto wbuf = pattern(0x37);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        fe.decline = F_BLK_FLUSH;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.features & F_BLK_FLUSH) return EPROTO;   // the mask did not take
        if (fe.write_dev(1 << 20, wbuf.data(), wbuf.size()) != S_OK) return EIO;
        return 0;
    });
    ASSERT_EQ(0, rc);
    // The descriptor's half of the same fact, and the reason `offered` and `negotiated`
    // are two fields rather than one: this transport still OFFERS FLUSH, while what the
    // frontend accepted is a subset that excludes it. A caller that could read only the
    // request or only the offer would conclude the device is in write-back.
    EXPECT_EQ(FEATURE_FLUSH, dev->get_info().offered);
    EXPECT_EQ(0ull, dev->get_info().negotiated);
    EXPECT_EQ(0, verify_backend(1 << 20, wbuf));
    // no FLUSH was sent, so the only thing that could have asked for this sync is
    // the write itself
    EXPECT_EQ(1, probe.datasyncs.load());
    EXPECT_EQ(0, probe.syncs.load());
}

// A backend write that lands SHORT, seen from the guest's side of the socket.
// test-blk-vq's a_write_that_landed_short_does_not_persist pins the engine's own
// verdict by calling serve_chain directly; what only a transport-level case can see
// is that the verdict survives the trip -- written into the guest's status byte and
// published into the used ring, rather than dropped by one of handle_req's teardown
// gates -- and that the used element's length is 1. The chain offers exactly one
// device-writable buffer (submit() marks the data descriptor readable for a T_OUT),
// so a length above 1 would be a claim about bytes written into a buffer this guest
// never offered as writable.
//
// Declining F_BLK_FLUSH is what makes the durability half assertable instead of
// trivially true: in write-back no write-time sync happens at all, so
// `datasyncs == 0` would also pass for a device that persisted a write it was about
// to report as failed -- the exact guard the engine case exists for. Write-through is
// also the mode this transport derives from the negotiated word, so the decline does
// double duty and the descriptor read below is the caller's view of the same fact.
TEST_F(VhostUserTest, a_short_backend_write_reaches_the_guest_as_ioerr_unpersisted) {
    test::BackendProbe probe(file);
    probe.short_write_by = 1;   // 65535 of the 65536 bytes asked for
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());

    auto wbuf = pattern(0x4c);
    int st = -1;
    uint32_t ulen = ~0u;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        fe.decline = F_BLK_FLUSH;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.features & F_BLK_FLUSH) return EPROTO;   // the mask did not take, so
                                                        // write_through was never derived
        st = fe.write_dev(1 << 20, wbuf.data(), wbuf.size(), &ulen);
        return st < 0 ? EIO : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(S_IOERR, st);
    EXPECT_EQ(1u, ulen);
    // The write REACHED pwritev, which is what separates a short count from an
    // earlier refusal: the read-only, sector-granularity and capacity gates all
    // answer S_IOERR with a used length of 1 as well, so without this the case would
    // pass green against a request that never got near the backend.
    EXPECT_EQ(1, probe.writes.load());
    EXPECT_EQ(0, probe.datasyncs.load());
    EXPECT_EQ(0, probe.syncs.load());
    EXPECT_EQ(0ull, dev->get_info().negotiated);
    // The bytes DID land -- short_write_by fakes the COUNT only (harness.h says so),
    // so a mismatch assertion here would be a claim about the probe rather than about
    // the device. Recorded so that the two counts above are not misread as "the range
    // is absent": the "not persisted" this case pins is the missing sync.
    EXPECT_EQ(0, verify_backend(1 << 20, wbuf));
}

// The same write-through mode, with the persist ASKED FOR and FAILED. test-blk-vq's
// a_write_whose_persist_fails_is_reported_as_failed pins the engine's verdict; the
// transport's half is that deriving write_through from the NEGOTIATED word is what
// puts the device in the mode where that verdict can arise at all, and that the
// verdict then reaches the used ring. `datasyncs` is what separates the two halves,
// and it is an equality rather than the `> 0` the distinction strictly needs:
// fail_syncs still counts (harness.h), so 1 says "tried and failed" where 0 says
// "never tried" -- and "never tried" is exactly what a device deriving write_through
// from its own OFFER produces, because make_info() asks for FLUSH and the offer
// therefore always carries it. Such a device answers S_OK for a write that never
// reached stable storage, and every byte-count assertion in this suite stays green
// under it: the bytes are in the page cache, so the read-back matches.
TEST_F(VhostUserTest, a_failed_write_through_persist_reaches_the_guest_as_ioerr) {
    test::BackendProbe probe(file);
    probe.fail_syncs = true;
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&probe));
    DEFER(dev->shutdown());

    auto wbuf = pattern(0x6d);
    int st = -1;
    uint32_t ulen = ~0u;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        fe.decline = F_BLK_FLUSH;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.features & F_BLK_FLUSH) return EPROTO;   // as above: without the
                                                        // decline there is no
                                                        // write-through persist to fail
        st = fe.write_dev(1 << 20, wbuf.data(), wbuf.size(), &ulen);
        return st < 0 ? EIO : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(S_IOERR, st);
    EXPECT_EQ(1u, ulen);
    EXPECT_EQ(1, probe.writes.load());      // the write itself reached the backend
    EXPECT_EQ(1, probe.datasyncs.load());   // and the persist was asked for, once --
                                            // paired with `writes` so that neither can
                                            // be satisfied by a device that synced once
                                            // and then stopped (harness.h)
    EXPECT_EQ(0, probe.syncs.load());       // fdatasync, not the heavier fsync
    EXPECT_EQ(0ull, dev->get_info().negotiated);
    // The bytes landed and only the persist failed, which is what makes reporting
    // success actively wrong here rather than merely optimistic: the completion is the
    // device's word that the data is where it promised, and in write-through that
    // promise is stable storage.
    EXPECT_EQ(0, verify_backend(1 << 20, wbuf));
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

// A dead listener's socket file. start() used to unlink it and rebind; it refuses
// now, because the only way to tell "crashed" from "about to listen" is to connect,
// and a node some other start has bound but not yet listened on answers exactly like
// a crashed one. Recovery moves to destroy_orphan(), a call the caller makes on
// purpose -- and the second half here is that it still recovers, so what changed is
// who decides, not whether the path can be reused.
TEST_F(VhostUserTest, a_stale_socket_is_refused_until_the_caller_destroys_it) {
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
    struct stat before;
    ASSERT_EQ(0, ::stat(SOCK_PATH, &before));

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EEXIST, errno);
    // A refusal is worth nothing if the removal already happened, so this is on the
    // inode: the same node, and not one this start() bound.
    struct stat after;
    ASSERT_EQ(0, ::stat(SOCK_PATH, &after));
    EXPECT_EQ(before.st_ino, after.st_ino) << "the stale node was replaced, not refused";

    // The explicit path still clears it, and the very same start() then succeeds and
    // serves -- which is the difference between refusing and being unusable.
    BlkDevInfo orphan;
    orphan.identity = SOCK_PATH;
    ASSERT_EQ(0, ctl->destroy_orphan(orphan));
    EXPECT_NE(0, ::access(SOCK_PATH, F_OK));

    ASSERT_EQ(0, dev->start(file));
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

// ---------------------------------------------------------------------------
// What start() leaves alone
//
// start() removes nothing, so each of these is a refusal plus the node surviving
// it. They used to hold apart three answers from a probe -- a live listener, a
// provably dead one, and no verdict -- and the probe is gone: bind() answers
// EADDRINUSE for all three and start() reports EEXIST. What the cases still pin is
// the half that was always the point, that nothing was deleted on the way to the
// answer. destroy_orphan() below is the call that does probe, and does refuse a
// live listener.
// ---------------------------------------------------------------------------

// The reviewer's first repro. A caller's ordinary file at the configured path was
// deleted and a socket bound in its place, because a connect to a regular file reads
// as "no listener". Nothing connects now, so the file is not preserved by a type
// check that had to be gotten right -- there is no path through start() that removes
// anything at all.
TEST_F(VhostUserTest, a_regular_file_at_the_socket_path_is_refused_and_preserved) {
    static const char WANT[] = "not a socket, and not ours to remove";
    int fd = ::open(SOCK_PATH, O_CREAT | O_TRUNC | O_RDWR, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ((ssize_t) sizeof(WANT), ::write(fd, WANT, sizeof(WANT)));
    ::close(fd);

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EEXIST, errno);

    // survived, same type, same bytes -- not merely "a node is still there"
    struct stat st;
    ASSERT_EQ(0, ::stat(SOCK_PATH, &st));
    EXPECT_TRUE(S_ISREG(st.st_mode));
    EXPECT_EQ(sizeof(WANT), (size_t) st.st_size);
    fd = ::open(SOCK_PATH, O_RDONLY);
    ASSERT_GE(fd, 0);
    DEFER(::close(fd));
    char back[sizeof(WANT)] = {};
    EXPECT_EQ((ssize_t) sizeof(WANT), ::read(fd, back, sizeof(back)));
    EXPECT_EQ(0, memcmp(WANT, back, sizeof(WANT)));
}

// Same shape. The errno here has been through three answers: EADDRINUSE from a bind
// reached after an unlink had failed EISDIR, then EINVAL from the type check added to
// stop that unlink, now EEXIST from a bind preceded by nothing at all. What all three
// share, and what this case was always about, is the second half: still a directory.
TEST_F(VhostUserTest, a_directory_at_the_socket_path_is_refused_and_preserved) {
    ::rmdir(SOCK_PATH);
    ASSERT_EQ(0, ::mkdir(SOCK_PATH, 0755));
    DEFER(::rmdir(SOCK_PATH));   // the fixture's cleanup unlinks, which a dir survives

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EEXIST, errno);

    struct stat st;
    ASSERT_EQ(0, ::stat(SOCK_PATH, &st));
    EXPECT_TRUE(S_ISDIR(st.st_mode));
}

// The reviewer's second repro: a bound, listening socket whose mode bits deny a
// connect. The old code probed it, got a denial, read that as "dead", and replaced a
// socket another process was serving.
//
// The mode bits no longer matter, and that is the improvement: nothing connects, so
// there is no probe to be denied and no answer that depends on which uid this suite
// happens to run under. Root and an ordinary user now get the same EEXIST, where
// before they got EBUSY and EACCES respectively and the case had to accept either.
// What it still pins is the inode -- the node was not replaced.
TEST_F(VhostUserTest, a_live_listener_is_refused_whatever_its_mode_bits) {
    int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(lfd, 0);
    DEFER(::close(lfd));
    sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", SOCK_PATH);
    ASSERT_EQ(0, ::bind(lfd, (sockaddr*)&un, sizeof(un)));
    ASSERT_EQ(0, ::listen(lfd, 1));
    ASSERT_EQ(0, ::chmod(SOCK_PATH, 0000));
    DEFER(::chmod(SOCK_PATH, 0600));   // so the fixture's cleanup can remove it

    struct stat before;
    ASSERT_EQ(0, ::stat(SOCK_PATH, &before));

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(file));
    EXPECT_EQ(EEXIST, errno);

    struct stat after;
    ASSERT_EQ(0, ::stat(SOCK_PATH, &after));
    EXPECT_TRUE(S_ISSOCK(after.st_mode));
    EXPECT_EQ(before.st_ino, after.st_ino);   // not replaced
}

// The identity lock, witnessed without naming it: what a start leaves behind in
// the controller's directory is discovered by looking rather than by restating
// the naming rule, so a change to that rule cannot quietly make this case
// vacuous. Holding a POSIX exclusive lock on the discovered entry then has to
// make the next start of the same identity fail, which is the whole of what stops
// two daemons -- both told the same path is confirmed dead -- from each unlinking
// the other's fresh node.
TEST_F(VhostUserTest, start_claims_an_identity_lock_and_refuses_a_held_one) {
    auto nonsockets = [](std::vector<std::string>& out) {
        out.clear();
        if (DIR* d = ::opendir(SOCK_DIR)) {
            struct dirent* e;
            char p[PATH_MAX];
            struct stat st;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                snprintf(p, sizeof(p), "%s/%s", SOCK_DIR, e->d_name);
                if (::stat(p, &st) == 0 && !S_ISSOCK(st.st_mode))
                    out.push_back(p);
            }
            ::closedir(d);
        }
    };
    std::vector<std::string> before, after;
    nonsockets(before);
    ASSERT_TRUE(before.empty());   // the fixture cleared the directory

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    nonsockets(after);
    ASSERT_EQ((size_t) 1, after.size());   // exactly one claim, and it is no socket
    std::string lock = after[0];
    EXPECT_EQ(0, dev->shutdown());
    EXPECT_EQ(0, ::access(lock.c_str(), F_OK));   // the claim outlives the session

    // Held the way another daemon would hold it: a bare POSIX exclusive lock, not
    // a call into the implementation, so this is an independent peer of it rather
    // than a restatement.
    int lfd = ::open(lock.c_str(), O_RDWR);
    ASSERT_GE(lfd, 0);
    DEFER(::close(lfd));
    ASSERT_EQ(0, ::flock(lfd, LOCK_EX | LOCK_NB));

    auto dev2 = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev2);
    DEFER(delete dev2);
    errno = 0;
    EXPECT_EQ(-1, dev2->start(file));
    EXPECT_EQ(EBUSY, errno);
    // and the refused start bound nothing: the loser leaves the path alone
    EXPECT_NE(0, ::access(SOCK_PATH, F_OK));
}

// The default is a fixed 0600, not a umask-derived 0666. The observation point
// is the inode's own permission bits as the kernel recorded them from our
// chmod, so this cannot be satisfied by the device reporting its own config.
// It discriminates under either umask this suite runs with: 0666 & ~0022 is
// 0644 and 0666 & ~0002 is 0664, neither of which is 0600.
TEST_F(VhostUserTest, sock_mode_default_is_0600) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    // sock_mode deliberately untouched: 0600 IS the default under test
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    struct stat sb;
    ASSERT_EQ(0, ::stat(SOCK_PATH, &sb));
    EXPECT_EQ(0600u, (unsigned)(sb.st_mode & 0777));
}

// The other half of the same contract: a caller whose guest process runs as a
// different user has to be able to widen the node, and what it asks for is
// what lands. 0640 is not the 0600 default, so this case cannot pass by way
// of sock_mode being ignored.
TEST_F(VhostUserTest, sock_mode_explicit_is_honored) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.sock_mode = 0640;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());
    struct stat sb;
    ASSERT_EQ(0, ::stat(SOCK_PATH, &sb));
    EXPECT_EQ(0640u, (unsigned)(sb.st_mode & 0777));
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

// Bind a listener at `path` and then close it, so the socket inode survives with
// nobody behind it: exactly the tombstone list_orphans() reports. Returns the
// bound fd's former listen state as 0, or an errno.
static int make_dead_socket(const char* path) {
    ::unlink(path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return errno;
    sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
    if (::bind(fd, (sockaddr*)&un, sizeof(un)) != 0) {
        int e = errno;
        ::close(fd);
        return e;
    }
    if (::listen(fd, 1) != 0) {
        int e = errno;
        ::close(fd);
        return e;
    }
    ::close(fd);   // dead: a connect probe now gets ECONNREFUSED
    return 0;
}

// The scan and the destroy agree, and the destroy is witnessed by the kernel
// rather than by us: two dead sockets go in, two inodes are gone afterwards, and
// the counter is what tells a run that did the work from a run that did nothing.
TEST_F(VhostUserTest, destroy_orphan_removes_dead_sockets) {
    char d1[96], d2[96];   // bounded: they go into sun_path (108)
    snprintf(d1, sizeof(d1), "%s/dead1.sock", SOCK_DIR);
    snprintf(d2, sizeof(d2), "%s/dead2.sock", SOCK_DIR);
    ASSERT_EQ(0, make_dead_socket(d1));
    ASSERT_EQ(0, make_dead_socket(d2));

    // the "before" half of the observation point: real socket inodes, recorded
    struct stat s1, s2;
    ASSERT_EQ(0, ::stat(d1, &s1));
    ASSERT_EQ(0, ::stat(d2, &s2));
    ASSERT_TRUE(S_ISSOCK(s1.st_mode) && S_ISSOCK(s2.st_mode));

    auto orphans = ctl->list_orphans();
    ASSERT_EQ(2u, orphans.size());   // SetUp emptied SOCK_DIR, so these are ours

    int destroyed = 0;
    for (auto& o : orphans) {
        errno = 0;
        int rc = ctl->destroy_orphan(o);
        // errno is only contracted on the -1 path. Asserting it is 0 here would
        // fail on a SUCCESS: the liveness probe that lets the destroy proceed is
        // a connect() that got ECONNREFUSED, and that is what a dead listener
        // looks like -- so 111 is left in errno on the path that returns 0.
        int e = errno;
        EXPECT_EQ(0, rc) << o.identity << " (errno " << e << ")";
        if (rc == 0)
            destroyed++;
    }
    EXPECT_EQ(2, destroyed);   // positive work counter: both, not "at least one"

    // the "after" half, read from the inode table and not from our own state.
    // errno is captured on the line after each stat: a gtest macro between the
    // call and the read could clobber it, which would turn this into a check of
    // whatever gtest last did.
    struct stat gone;
    int r1 = ::stat(d1, &gone);
    int e1 = errno;
    int r2 = ::stat(d2, &gone);
    int e2 = errno;
    EXPECT_NE(0, r1);
    EXPECT_EQ(ENOENT, e1);
    EXPECT_NE(0, r2);
    EXPECT_EQ(ENOENT, e2);
    EXPECT_EQ(0u, ctl->list_orphans().size());
}

// A live endpoint must survive a destroy asked for by a caller whose BlkDevInfo
// predates the daemon that now holds the path. list_orphans() would never have
// offered this one -- that is the point: the gate has to hold for a stale record,
// which is what a recovery loop actually carries.
TEST_F(VhostUserTest, destroy_orphan_refuses_a_live_endpoint) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    EXPECT_EQ(0u, ctl->list_orphans().size());   // live, so the scan skips it

    BlkDevInfo stale;
    stale.identity = SOCK_PATH;   // a record from before start(), or from a peer
    errno = 0;
    int rc = ctl->destroy_orphan(stale);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EBUSY, e);

    // still there, still a socket, still not an orphan ...
    struct stat sb;
    ASSERT_EQ(0, ::stat(SOCK_PATH, &sb));
    EXPECT_TRUE(S_ISSOCK(sb.st_mode));
    EXPECT_EQ(0u, ctl->list_orphans().size());

    // ... and still SERVING, which "the file exists" cannot tell us
    auto wbuf = pattern(0x3c);
    std::vector<char> rbuf(wbuf.size());
    EXPECT_EQ(0, run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.write_dev(1 << 19, wbuf.data(), wbuf.size()) != S_OK) return EIO;
        if (fe.read_dev(1 << 19, rbuf.data(), rbuf.size()) != S_OK) return EIO;
        return memcmp(wbuf.data(), rbuf.data(), wbuf.size()) ? EILSEQ : 0;
    }));
}

// The window do_listen() walks and destroy_orphan() must not unlink inside: a
// socket bound but not yet listening answers a connect probe with ECONNREFUSED,
// byte for byte what a dead listener answers, so the probe cannot tell a start
// mid-window from a crash -- only the identity lock can, and destroy_orphan() takes
// it before it probes. Without that lock this call returned 0 and deleted the node,
// and the starter's listen() then succeeded on an unnamed inode no frontend could
// ever connect to, while a second start was free to bind the name again.
TEST_F(VhostUserTest, destroy_orphan_refuses_a_bound_socket_whose_starter_holds_the_lock) {
    // Discover the lock the way start_claims_an_identity_lock_and_refuses_a_held_one
    // does: a start leaves exactly one non-socket claim in the scope directory and
    // it outlives the session. Discovered rather than spelled, so a change to the
    // naming rule cannot quietly leave this case holding a file nothing contends on.
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    std::string lock;
    if (DIR* d = ::opendir(SOCK_DIR)) {
        struct dirent* e;
        char p[PATH_MAX];
        struct stat st;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof(p), "%s/%s", SOCK_DIR, e->d_name);
            if (::stat(p, &st) == 0 && !S_ISSOCK(st.st_mode))
                lock = p;
        }
        ::closedir(d);
    }
    ASSERT_FALSE(lock.empty());
    ASSERT_EQ(0, dev->shutdown());   // unlinks the node; the claim stays

    // Hold the claim the way a concurrent starter's do_listen() would -- a bare
    // POSIX exclusive lock, an independent peer of devlock_acquire rather than a
    // call into it -- and stand in the window: bound, NOT listening. The window is
    // a state of the node, not a timing, so no second thread is needed to pin it.
    int lfd = ::open(lock.c_str(), O_RDWR);
    ASSERT_GE(lfd, 0);
    DEFER(::close(lfd));
    ASSERT_EQ(0, ::flock(lfd, LOCK_EX | LOCK_NB));
    int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(sfd, 0);
    DEFER(::close(sfd));
    sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", SOCK_PATH);
    ASSERT_EQ(0, ::bind(sfd, (sockaddr*)&un, sizeof(un)));

    // The window's defining property, witnessed against the kernel rather than
    // through our own probe: this connect is REFUSED, exactly like one to a dead
    // listener -- which is why the probe below cannot be the gate.
    {
        int pfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        ASSERT_GE(pfd, 0);
        DEFER(::close(pfd));
        errno = 0;
        ASSERT_EQ(-1, ::connect(pfd, (sockaddr*)&un, sizeof(un)));
        ASSERT_EQ(ECONNREFUSED, errno);
    }

    BlkDevInfo o;
    o.identity = SOCK_PATH;
    errno = 0;
    int rc = ctl->destroy_orphan(o);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EBUSY, e);
    EXPECT_EQ(0, ::access(SOCK_PATH, F_OK))
        << "the node was unlinked out from under the starter holding its identity";

    // Positive control: release the claim and the same call removes the same node
    // -- still bound, still not listening -- so the EBUSY above came from the lock
    // and not from anything this path always answers.
    ASSERT_EQ(0, ::flock(lfd, LOCK_UN));
    errno = 0;
    EXPECT_EQ(0, ctl->destroy_orphan(o));
    EXPECT_NE(0, ::access(SOCK_PATH, F_OK));
}

// The identity is caller-supplied and names something to delete, so scope is the
// only thing between this API and an unlink of anything the caller can spell.
// Three escapes: a path in another directory, the same path spelled as a
// traversal, and a non-socket sitting in our own directory. Each sentinel is
// asserted to still exist afterwards -- "it returned EINVAL" alone would pass
// just as happily if the delete happened first.
TEST_F(VhostUserTest, destroy_orphan_refuses_out_of_scope_and_non_sockets) {
    // (1) a dead socket OUTSIDE SOCK_DIR. Dead, so nothing but scope protects it.
    char outside[96];
    snprintf(outside, sizeof(outside), "/tmp/photon-blk-vhu-outside.sock");
    ASSERT_EQ(0, make_dead_socket(outside));
    DEFER(::unlink(outside));

    BlkDevInfo o;
    o.identity = outside;
    errno = 0;
    int rc = ctl->destroy_orphan(o);
    int e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EINVAL, e);
    EXPECT_EQ(0, ::access(outside, F_OK)) << "the out-of-scope socket was deleted";

    // (2) the same target spelled as a traversal out of SOCK_DIR
    char trav[160];
    snprintf(trav, sizeof(trav), "%s/../photon-blk-vhu-outside.sock", SOCK_DIR);
    o.identity = trav;
    errno = 0;
    rc = ctl->destroy_orphan(o);
    e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EINVAL, e);
    EXPECT_EQ(0, ::access(outside, F_OK)) << "the traversal spelling got through";

    // (3) a regular file INSIDE SOCK_DIR: scope passes, so S_ISSOCK is the gate
    char reg[96];
    snprintf(reg, sizeof(reg), "%s/not-a-socket", SOCK_DIR);
    int fd = ::open(reg, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(1, ::write(fd, "x", 1));
    ::close(fd);
    DEFER(::unlink(reg));

    o.identity = reg;
    errno = 0;
    rc = ctl->destroy_orphan(o);
    e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(EINVAL, e);
    EXPECT_EQ(0, ::access(reg, F_OK)) << "the regular file was deleted";

    // (4) and nothing there at all is ENOENT, not success
    o.identity = std::string(SOCK_DIR) + "/never-existed.sock";
    errno = 0;
    rc = ctl->destroy_orphan(o);
    e = errno;
    EXPECT_EQ(-1, rc);
    EXPECT_EQ(ENOENT, e);
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
                uint32_t head = 0, len = 0;
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
                uint32_t head = 0, len = 0;
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
        {VHOST_USER_SET_VRING_NUM,    true,  false, "SET_VRING_NUM"},
        {VHOST_USER_SET_VRING_ADDR,   false, true,  "SET_VRING_ADDR"},
        {VHOST_USER_SET_VRING_BASE,   true,  false, "SET_VRING_BASE"},
        {VHOST_USER_GET_VRING_BASE,   true,  false, "GET_VRING_BASE"},
        {VHOST_USER_SET_VRING_KICK,   false, false, "SET_VRING_KICK"},
        {VHOST_USER_SET_VRING_CALL,   false, false, "SET_VRING_CALL"},
        {VHOST_USER_SET_VRING_ENABLE, true,  false, "SET_VRING_ENABLE"},
    };
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        // negotiate() settles REPLY_ACK, so every request below gets a u64 ack
        if (!fe.negotiate(false)) return EPROTO;
        for (const auto& t : msgs) {
            // GET_VRING_BASE only at 0xffff -- see the comment above
            const uint32_t idxs[2] = {1u, 0xffffu};
            for (int k = (t.req == VHOST_USER_GET_VRING_BASE ? 1 : 0); k < 2; k++) {
                vhost_user_msg m, r;
                memset(&m, 0, sizeof(m));
                m.request = t.req;
                if (t.state) {
                    m.size = sizeof(vhost_vring_state);
                    // num is legal in every case: a power of two in range for NUM,
                    // 0 for BASE, and 1 (enable) for ENABLE -- so a rejection can
                    // only have been caused by the index
                    m.payload.state = {idxs[k], t.req == VHOST_USER_SET_VRING_NUM ? VQ_NUM
                                       : (t.req == VHOST_USER_SET_VRING_ENABLE ? 1u : 0u)};
                } else if (t.addr) {
                    m.size = sizeof(vhost_vring_addr);
                    // Real QVAs, not the bare L_* offsets: negotiate() declared one
                    // region whose qva base is fe.mem, so a bare offset does not
                    // resolve. Sending an address that cannot be translated would
                    // make the backend reject this message for a DIFFERENT reason,
                    // the ack would be 1 either way, and deleting the index guard
                    // would no longer turn this case red.
                    vhost_vring_addr a{idxs[k], 0, (uint64_t)(fe.mem + L_DESC),
                                    (uint64_t)(fe.mem + L_USED),
                                    (uint64_t)(fe.mem + L_AVAIL), 0};
                    memcpy(&m.payload.addr, &a, sizeof(a));
                } else {
                    // KICK/CALL: the index is the low 8 bits of the u64 and NOFD
                    // says "no fd attached", so no SCM_RIGHTS is needed
                    m.size = 8;
                    m.payload.u64 = idxs[k] | VHOST_USER_VRING_NOFD_MASK;
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
// listening on, so its requests vanish.
//
// A fourth channel has to agree with those three, and it is the one a real
// primary gates on: bit 0 of the vhost-user PROTOCOL feature word. The count
// query above is only sent once that bit is settled, so a device that offers
// virtio F_MQ, publishes a truthful num_queues and answers GET_QUEUE_NUM with
// the real count can still be driven as a single queue -- the peer never asked.
// Asserting all four together is what stops them drifting apart, and the
// `queue_num` assertion at the end reads the consequence rather than the bit.
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

        uint64_t want = c.want, got_qn = 0, got_feat = 0, got_proto = 0;
        uint16_t got_nq = 0;
        uint32_t fe_queues = 0;
        int rc = run_frontend([&](MockFrontend& fe) -> int {
            if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
            // Negotiate first, the way a conformant frontend does: this suite was
            // deliberately hardened to model the protocol's own gates, and reading
            // the device config before PROTOCOL_F_CONFIG is settled is not
            // something a real peer does. It also leaves vq 0 running, so the
            // teardown at the end of the iteration exercises every queue slot --
            // including the ones this frontend never addressed.
            if (!fe.negotiate(false)) return EPROTO;
            got_proto = fe.proto_features;
            fe_queues = fe.queue_num;
            vhost_user_msg m, r;
            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_GET_FEATURES; m.size = 0;
            if (!fe.transact(&m, &r)) return EPROTO;
            got_feat = r.payload.u64;

            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_GET_QUEUE_NUM; m.size = 0;
            if (!fe.transact(&m, &r)) return EPROTO;
            got_qn = r.payload.u64;

            memset(&m, 0, sizeof(m));
            m.request = VHOST_USER_GET_CONFIG;
            m.size = offsetof(vhost_user_config, region) + sizeof(blk_config);
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
        // The fourth channel, and the one a primary actually gates on. Same column
        // as F_MQ, because the two have to agree: virtio F_MQ is what tells the
        // guest driver the device has N queues, and protocol MQ is what lets the
        // frontend build them. Offering one without the other is the
        // incompatibility this case exists to catch -- a device that offers F_MQ
        // and answers GET_QUEUE_NUM truthfully, and that a real primary still
        // drives as a single queue.
        EXPECT_EQ(c.mq, !!(got_proto & (1ULL << VHOST_USER_PROTOCOL_F_MQ)))
            << "PROTOCOL_F_MQ, queues=" << c.ask;
        // And the consequence, read off the mock's own gated query rather than
        // asserted about it: with the bit settled the primary learns the true
        // count, and without it the count it ends up with is 1 no matter how many
        // queues the device serves. Both halves are in this table, so a gate that
        // never fired and one that always fired would each be caught by a row.
        EXPECT_EQ(c.mq ? (uint32_t)want : 1u, fe_queues) << "queues=" << c.ask;
    }
}

// The table above asserts the bit is offered at the right counts. This asserts
// what the bit is FOR, on one device, with the negotiated protocol word as the
// only variable: a primary that settles it learns the true queue count, and one
// that does not ends up with 1 -- while the device serves the same four queues
// and answers a direct GET_QUEUE_NUM with 4 in both halves.
//
// The second half is not a hypothetical. It is what a conformant primary does
// when the backend fails to offer the bit, which is the state this device was in:
// virtio F_MQ offered, num_queues truthful, GET_QUEUE_NUM answered, and a real
// frontend still refusing to instantiate more than one queue because the count
// query that would have told it otherwise is gated on a bit that was never
// offered. Asserting only the declined half would pass against exactly that
// broken device, so the accepted half is here too and the two are compared.
TEST_F(VhostUserTest, protocol_mq_is_what_lets_a_frontend_learn_the_queue_count) {
    constexpr uint32_t QUEUES = 4;
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.queues = QUEUES;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    uint64_t offered = 0, served = 0;
    uint32_t learned = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        offered = fe.proto_features;
        learned = fe.queue_num;
        // Asked straight from the device, so the count it SERVES is on the record
        // independently of what the gated query inside negotiate() concluded.
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_QUEUE_NUM; m.size = 0;
        if (!fe.transact(&m, &r)) return EPROTO;
        served = r.payload.u64;
        return 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_NE(0u, offered & (1ULL << VHOST_USER_PROTOCOL_F_MQ))
        << "a device serving " << QUEUES << " queues must offer protocol MQ";
    EXPECT_EQ((uint64_t)QUEUES, served);
    EXPECT_EQ(QUEUES, learned);

    // Same device, same four queues, one bit declined.
    uint64_t served_again = 0;
    uint32_t capped = 0;
    rc = run_frontend([&](MockFrontend& fe) -> int {
        fe.proto_decline = (1ULL << VHOST_USER_PROTOCOL_F_MQ);
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        capped = fe.queue_num;
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_QUEUE_NUM; m.size = 0;
        if (!fe.transact(&m, &r)) return EPROTO;
        served_again = r.payload.u64;
        return 0;
    });
    ASSERT_EQ(0, rc);
    // The cap, and the fact that it is the frontend's doing and not the device's.
    EXPECT_EQ(1u, capped);
    EXPECT_EQ((uint64_t)QUEUES, served_again);
    EXPECT_EQ(served, served_again);
}

// The two cases above pin the multiqueue CONTROL plane. Nothing in this file had ever
// driven a request on a queue above zero: every submit(), collect() and do_request()
// here targeted the rings at L_DESC/L_AVAIL/L_USED, which negotiate() hands to index
// 0. So a backend that configured queues 1..N correctly and then served them all from
// queue 0's ring, that bound every queue's hooks to vqs[0], or that signalled a
// completion on a constant index's callfd, passed the whole suite green.
//
// This is one non-zero queue's DATA path end to end: its own avail ring is consumed,
// its own descriptors are walked into its own guest buffers, its completion lands in
// its own used ring, and its own callfd is what fires. Queue 1 is driven in the same
// session so that "queue 3 works" cannot be satisfied by a backend that merely aliases
// whatever index it is handed to the one queue it really serves.
//
// The discriminating assertion is the callfd triple, and specifically the zero on
// queue 0 -- NOT the used ring. collect() polls the ring and uses the eventfd only as
// a wait hint, so a completion that landed in the right ring behind a notification
// that went to the wrong fd is invisible to every other assertion in this case. Queue
// 0 is given no work at all here, so any count on its fd is a notify bound to a
// constant index: notify_thunk passing something other than q->qid, or the
// SET_VRING_CALL handler storing every queue's fd in one slot.
TEST_F(VhostUserTest, a_queue_above_zero_serves_io_end_to_end) {
    constexpr uint32_t QUEUES = 4;
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.queues = QUEUES;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint64_t OFF1 = 12 << 20, OFF3 = 13 << 20;
    constexpr size_t LEN = 4096;
    // Hand-driven requests take slots from the top, clear of the ones do_request's
    // slot_seq hands out from the bottom in the same session.
    constexpr uint16_t SLOT3 = SLOTS - 1;
    auto w1 = pattern(0x1e, LEN), w3 = pattern(0xa7, LEN);
    std::vector<char> r1(LEN), r3(LEN), x1(LEN), x3(LEN);
    uint64_t sig0 = 0, sig1 = 0, sig3 = 0;

    // The raw off-vcpu helper rather than run_frontend(), for the reason
    // avail_event_published gives: the assertions below are gtest macros and need a
    // void context, not an errno. `fe` therefore outlives the body, so a fatal ASSERT
    // that returns early still leaves here what it wrote before it did.
    MockFrontend fe;
    test::run_off_vcpu([&] {
        ASSERT_TRUE(fe.connect_to(SOCK_PATH));
        ASSERT_TRUE(fe.negotiate(false));
        // Preconditions, and what stops this case passing vacuously. negotiate() asks
        // GET_QUEUE_NUM only once PROTOCOL_F_MQ is settled and leaves queue_num at 1
        // otherwise, so against a device serving one queue these two fire and
        // setup_queue(3) is never reached. Without them the same device would fail
        // later as an error ack on vring index 3, which reads as a data-path failure
        // and is not one.
        ASSERT_NE(0u, fe.proto_features & (1ULL << VHOST_USER_PROTOCOL_F_MQ))
            << "protocol MQ was not settled, so this frontend has one queue";
        ASSERT_EQ(QUEUES, fe.queue_num)
            << "the peer learned a count that cannot address queue 3";

        ASSERT_TRUE(fe.setup_queue(1));
        ASSERT_TRUE(fe.setup_queue(3));
        // SET_VRING_CALL signals the fd it installs exactly once -- the behaviour
        // vring_call_signals_once pins -- and it is sent for every queue set up here.
        // Clear all three so the counts below can only have come from a completion.
        (void)fe.callfd_drain(0);
        (void)fe.callfd_drain(1);
        (void)fe.callfd_drain(3);

        // Queue 3 driven by hand rather than through write_dev(): do_request's
        // collect() drains the callfd it polls, which would eat the very signal this
        // half of the case is about.
        memcpy(fe.mem + MockFrontend::data_off(SLOT3), w3.data(), w3.size());
        ASSERT_EQ(0, fe.submit(SLOT3, T_OUT, OFF3 >> 9, (uint32_t)LEN, false, 3));
        ASSERT_TRUE(fe.kick(3));
        ASSERT_TRUE(fe.served_within(SLOT3))
            << "queue 3's avail ring was never consumed";
        EXPECT_TRUE(fe.callfd_readable(1000, 3))
            << "queue 3's own callfd was never signalled";
        // Drained in this order, and the 1 s wait above is what makes the two zeros
        // below mean something: a notify delivered to the wrong fd arrives instead of
        // this one, not as well as it, so if it went to queue 0 or queue 1 the wait
        // has already expired in full by the time that counter is read.
        sig3 = fe.callfd_drain(3);
        sig0 = fe.callfd_drain(0);
        sig1 = fe.callfd_drain(1);

        uint32_t head = VQ_NUM, ulen = 0;
        ASSERT_TRUE(fe.collect(&head, &ulen, 5000, 3));
        ASSERT_LT(head, VQ_NUM);
        EXPECT_EQ((int)SLOT3, (int)fe.slot_of_head_of(3)[head])
            << "queue 3's used element names a head that ring never published";
        EXPECT_EQ(1u, ulen) << "a successful T_OUT writes only the status byte";
        EXPECT_EQ(S_OK, (int)*(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT3)));

        // Read back through the same queue. This is the half that proves queue 3's
        // DESCRIPTORS were walked and not merely its avail ring drained: the payload
        // has to land in the guest buffer the chain this side built named.
        ASSERT_EQ(0, fe.read_dev(OFF3, r3.data(), r3.size(), 3));

        // A second queue in the same session, through the ordinary wrappers.
        ASSERT_EQ(0, fe.write_dev(OFF1, w1.data(), w1.size(), nullptr, 1));
        ASSERT_EQ(0, fe.read_dev(OFF1, r1.data(), r1.size(), 1));

        // Each queue reads what the OTHER wrote. One backing file, so this is not a
        // data-isolation claim; it is that both queues are still coherently bookkept
        // after the hand-driven sequence above, whose submit/collect pair moved queue
        // 3's two cursors behind do_request's back.
        ASSERT_EQ(0, fe.read_dev(OFF3, x1.data(), x1.size(), 1));
        ASSERT_EQ(0, fe.read_dev(OFF1, x3.data(), x3.size(), 3));
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);

    EXPECT_EQ(1u, sig3) << "queue 3's first completion must interrupt it exactly once";
    EXPECT_EQ(0u, sig0) << "queue 0 had no work, so its callfd cannot have been signalled";
    EXPECT_EQ(0u, sig1) << "queue 1 had no work yet when this was read";
    EXPECT_EQ(0, memcmp(r3.data(), w3.data(), LEN)) << "queue 3 did not read back its own write";
    EXPECT_EQ(0, memcmp(r1.data(), w1.data(), LEN)) << "queue 1 did not read back its own write";
    EXPECT_EQ(0, memcmp(x1.data(), w3.data(), LEN)) << "queue 1 could not see queue 3's write";
    EXPECT_EQ(0, memcmp(x3.data(), w1.data(), LEN)) << "queue 3 could not see queue 1's write";
    // And through to the backing file, not just back out of the ring: a queue that
    // completed a write it never issued would satisfy every assertion above.
    EXPECT_EQ(0, verify_backend(OFF1, w1));
    EXPECT_EQ(0, verify_backend(OFF3, w3));
}

// Independence, not just presence. The case above shows a non-zero queue works; it
// cannot separate a backend that serves queue N from queue N's own ring from one that
// serves it somewhere shared and happens to report back correctly. So: one request in
// flight on queue 1 and another on queue 2 at the same time, held there by the backend
// gate, and then ONE token released so that exactly one of the two retires. The other
// must still be untouched -- used ring unmoved, callfd silent -- which is the
// observation a crossed wire cannot survive, and the one thing about multiqueue that
// a sequence of single-queue requests can never show.
//
// Which of the two retires first is the gate semaphore's choice, so every assertion
// below is written against `first` and `other` rather than against a queue number.
//
// The discriminating assertions are the ones on `other`, and which of them has teeth
// depends on the mutation. sig_other (with the callfd_readable that precedes it) is
// the ONLY thing in the case that goes red on a notify bound to a constant index:
// the used rings, the collects, the lengths and the statuses all still come out right
// when the interrupt is merely delivered to the wrong eventfd. used_other is the
// first thing that goes red on a completion appended to the wrong used ring -- one of
// the two collects at the end would time out on that too, but only after the gate has
// opened and both rings hold an element, which says "something did not complete"
// rather than "a queue was credited with somebody else's completion". used_q0/sig_q0
// are the same two assertions aimed at the ring every other case in this file drives,
// so a backend that funnels other queues' completions to index 0 is caught as well.
TEST_F(VhostUserTest, two_queues_complete_independently) {
    constexpr uint32_t QUEUES = 3;
    constexpr uint64_t OFF1 = 20 << 20, OFF2 = 21 << 20;
    constexpr size_t LEN = 4096;
    constexpr uint16_t SLOT1 = SLOTS - 1, SLOT2 = SLOTS - 2;

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.queues = QUEUES;
    // Declared before `dev` and before `fe`: RecordingFile does not own the backend,
    // so its destruction has to come after the device's shutdown DEFER and its delete.
    test::RecordingFile rf(file);
    rf.gated = true;
    auto w1 = pattern(0x4b, LEN);
    auto w2 = pattern(0xd2, LEN);
    // Queue 2's request is a READ, so it needs content to come back with. Filled
    // through the fixture's own handle and not through the mock: the gate is already
    // shut, and every IO the DEVICE issues would park in it.
    {
        iovec iov{w2.data(), w2.size()};
        ASSERT_EQ((ssize_t)LEN, file->pwritev(&iov, 1, (off_t)OFF2));
    }
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rf));
    DEFER(dev->shutdown());
    // Runs BEFORE the shutdown above, and it is the backstop for an early ASSERT:
    // a request left parked here would make an orderly teardown wait out a timeout.
    DEFER(rf.release_gate(1024));

    uint32_t first = 0, other = 0;
    uint16_t used_other = 0, used_q0 = 0;
    uint64_t sig_first = 0, sig_other = 0, sig_q0 = 0;
    uint32_t h1 = VQ_NUM, l1 = 0, h2 = VQ_NUM, l2 = 0;
    int st1 = -1, st2 = -1, data2 = -1;

    MockFrontend fe;
    test::run_off_vcpu([&] {
        ASSERT_TRUE(fe.connect_to(SOCK_PATH));
        ASSERT_TRUE(fe.negotiate(false));
        // The same precondition as the case above, for the same reason.
        ASSERT_NE(0u, fe.proto_features & (1ULL << VHOST_USER_PROTOCOL_F_MQ))
            << "protocol MQ was not settled, so this frontend has one queue";
        ASSERT_EQ(QUEUES, fe.queue_num)
            << "the peer learned a count that cannot address queue 2";
        ASSERT_TRUE(fe.setup_queue(1));
        ASSERT_TRUE(fe.setup_queue(2));
        (void)fe.callfd_drain(0);
        (void)fe.callfd_drain(1);
        (void)fe.callfd_drain(2);

        memcpy(fe.mem + MockFrontend::data_off(SLOT1), w1.data(), w1.size());
        // A sentinel under the read buffer, so that "the device filled this" and "it
        // already held what we expect" cannot be the same observation.
        memset(fe.mem + MockFrontend::data_off(SLOT2), 0xcc, LEN);

        // A write on queue 1 and a read on queue 2. Different types on purpose: a
        // successful T_OUT's used length is 1 and a T_IN's is LEN+1, so the element
        // found in each ring says which request it belongs to.
        ASSERT_EQ(0, fe.submit(SLOT1, T_OUT, OFF1 >> 9, (uint32_t)LEN, false, 1));
        ASSERT_TRUE(fe.kick(1));
        ASSERT_EQ(0, fe.submit(SLOT2, T_IN, OFF2 >> 9, (uint32_t)LEN, true, 2));
        ASSERT_TRUE(fe.kick(2));

        // Both parked, which is what makes them concurrent rather than merely
        // consecutive. Bounded, and the bound is a precondition rather than a hope:
        // with `gated` set, `arrivals` counts what ENTERED the backend, so 2 is the
        // evidence that both were dispatched before either could complete. FLUSH is
        // negotiated on this connection, so a write is one recorded IO and not two --
        // which is what makes 2 the expected count rather than a lower bound.
        for (int i = 0; i < 5000 && rf.arrivals.load() < 2u; i++)
            ::usleep(1000);
        ASSERT_EQ(2u, (uint32_t)rf.arrivals.load())
            << "both requests have to be in flight before either is released";

        // One token, so exactly one of the two retires.
        rf.poke_gate(1);
        uint16_t u0 = 0;
        for (int i = 0; i < 500 && !first; i++) {
            u0 = fe.used_idx_now(0);
            uint16_t u1 = fe.used_idx_now(1), u2 = fe.used_idx_now(2);
            if (u1 || u2) first = u1 ? 1u : 2u;
            else ::usleep(10 * 1000);
        }
        ASSERT_TRUE(first == 1u || first == 2u)
            << "releasing one gated IO advanced no queue's used ring; queue 0's is " << u0;
        other = 3 - first;

        // Wait for the retirement to be fully REPORTED before reading anything else.
        // complete_req appends the used element and then writes the callfd with no
        // yield between, but this thread does neither, so the ring can be seen
        // advanced a few instructions before the eventfd is. Bounding the wait on the
        // positive is also what gives the negative that follows its force: without it
        // a silent callfd on `other` could mean "not yet" instead of "not yours".
        // EXPECT rather than ASSERT so that both counters below are still read and
        // reported when this one fails -- which is exactly the case where the split
        // between them is the interesting part.
        EXPECT_TRUE(fe.callfd_readable(1000, first))
            << "queue " << first << " retired without interrupting its own callfd";
        // The other queue has a request in flight and no token, so nothing that
        // belongs to it may have moved.
        EXPECT_FALSE(fe.callfd_readable(200, other))
            << "queue " << other << " was interrupted for a completion that is not its own";
        used_other = fe.used_idx_now(other);
        used_q0 = fe.used_idx_now(0);
        sig_first = fe.callfd_drain(first);
        sig_other = fe.callfd_drain(other);
        sig_q0 = fe.callfd_drain(0);

        rf.release_gate(1024);
        ASSERT_TRUE(fe.collect(&h1, &l1, 5000, 1));
        ASSERT_TRUE(fe.collect(&h2, &l2, 5000, 2));
        st1 = *(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT1));
        st2 = *(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT2));
        data2 = memcmp(w2.data(), fe.mem + MockFrontend::data_off(SLOT2), LEN);
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);

    ASSERT_TRUE(first == 1u || first == 2u);
    EXPECT_EQ(0, (int)used_q0) << "queue 0 was given no work in this case";
    EXPECT_EQ(0, (int)used_other)
        << "queue " << other << " completed a request that is still parked in the gate";
    EXPECT_EQ(1u, sig_first) << "the queue that retired was not interrupted exactly once";
    EXPECT_EQ(0u, sig_other)
        << "queue " << other << " was interrupted for a completion that is not its own";
    EXPECT_EQ(0u, sig_q0) << "queue 0 was interrupted for work it never had";

    ASSERT_LT(h1, VQ_NUM);
    ASSERT_LT(h2, VQ_NUM);
    EXPECT_EQ((int)SLOT1, (int)fe.slot_of_head_of(1)[h1]);
    EXPECT_EQ((int)SLOT2, (int)fe.slot_of_head_of(2)[h2]);
    // The two lengths differ, so each ring's element is identified as its own
    // request's. These are what go red on a dispatch that took one queue's head and
    // walked another's descriptor array: nothing is ever submitted on queue 0 here, so
    // its array is still the memset zeros the mock mapped, the walk ends at its first
    // descriptor and completes 0 bytes with the 0xff sentinel untouched -- and the
    // status pair below then says which of the two queues it happened to.
    EXPECT_EQ(1u, l1) << "queue 1's element does not describe the T_OUT it was given";
    EXPECT_EQ((uint32_t)LEN + 1, l2) << "queue 2's element does not describe the T_IN it was given";
    EXPECT_EQ(S_OK, st1);
    EXPECT_EQ(S_OK, st2);
    EXPECT_EQ(0, data2) << "queue 2's read did not fill the guest buffer its own chain named";
    EXPECT_EQ(0, verify_backend(OFF1, w1));
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
// used->ring[used_idx % num]) and bounds the in-flight coroutine cap from above.
// 65536 used to sail through vq_may_dispatch() -- which rejects only 0 -- and
// truncate to 0 in vring_used_append's uint16_t parameter: SIGFPE on the first
// completion. A rejected message must also leave the live queue exactly as it was.
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
// with it the only bound on how long the in-flight cap's overflow waits. QEMU
// always sends EFD_NONBLOCK, which is why this half of harden_recv_fd had never
// run.
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

// dispatch_avail caps in-flight at the lesser of `num` and the caller's
// queue_depth and leaves the rest pending without advancing last_avail. This case
// sets no queue_depth, so the cap here is `num` -- VQ_NUM -- and that is worth
// saying out loud, because mutate-vhost-user.py's `noprogress` record names this
// case as its detector and the detection depends on the cap BINDING: a smaller cap
// still binds, a larger one would let all 264 through at once and the record would
// go stale.
//
// What picks the overflow up is a completion freeing a slot (redispatch_backlog),
// with loop()'s KICK_FALLBACK_US re-read behind it. So ONE kick for more than the
// cap must still complete all of them -- the cap alone would silently strand the
// overflow. Measured on the free-running used INDEX rather than by counting
// elements, because 264 completions legitimately lap the 256-entry used ring.
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

// BlkConfig::queue_depth is a per-queue in-flight limit, and the cap itself is
// pinned at the engine level in test-blk-vq. This is the other half, the one only
// a transport can get wrong: a config field nobody copies into the engine is
// indistinguishable from one that is copied and then ignored. Until it was plumbed,
// the only bound vhost-user had was the frontend's SET_VRING_NUM -- VQ_NUM here --
// so a caller that asked for 2 got 256.
//
// Same gated-backend shape as the two cases around it, so the count is a state and
// not a race. The release runs inside the frontend body: semaphore::signal is
// documented as callable from any std thread, and the DEFER in the test body is the
// backstop for the paths that return before reaching it.
TEST_F(VhostUserTest, configured_queue_depth_caps_in_flight) {
    constexpr uint64_t DEPTH = 2;
    constexpr int N = 8;   // far under VQ_NUM, so only the depth can hold this at 2
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.queue_depth = (uint32_t) DEPTH;
    // Declared before `dev`: RecordingFile does not own the backend, so its
    // destruction has to come after the device's shutdown DEFER and its delete.
    test::RecordingFile rf(file);
    rf.gated = true;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rf));
    DEFER(dev->shutdown());
    // Runs BEFORE the shutdown above -- guards fire in reverse declaration order.
    // An orderly teardown drains the avail backlog, and a request parked in a gate
    // nobody is going to open makes that drain wait out a timeout instead of
    // finishing.
    DEFER(rf.release_gate(1024));

    uint64_t seen = 0;
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

        // every avail entry names the SAME head, as in dispatch_cap_recovery
        uint16_t u0 = fe.used_idx_now();
        for (int i = 0; i < N; i++)
            fe.publish(0);
        if (!fe.kick()) return EIO;   // ONE kick for all N

        for (int i = 0; i < 2000 && rf.arrivals.load() < DEPTH; i++)
            ::usleep(1000);
        // A settle window, and it can only strengthen the assertion: the gate is
        // shut, so nothing completes and nothing frees a slot, which means the
        // count can only ever rise here. Waiting can therefore never turn a device
        // that over-admitted into one that looks like it did not. 50 ms is ten of
        // the engine's own KICK_FALLBACK_US re-reads, so any further dispatch that
        // was going to happen has.
        ::usleep(50 * 1000);
        seen = rf.arrivals.load();
        rf.release_gate(1024);
        if (!fe.wait_used_advance(u0, (uint16_t) N, 20000)) return ETIMEDOUT;
        return *(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT)) == S_OK ? 0 : EIO;
    });
    // Asserted out here rather than inside the body: a count that came back wrong
    // should be reported as the number it was, not collapsed into an errno.
    EXPECT_EQ(0, rc);
    EXPECT_EQ(DEPTH, seen);
    EXPECT_EQ((uint64_t) N, rf.arrivals.load());
}

// A pause is not a teardown, and the difference is exactly the requests already
// dispatched. SET_VRING_ENABLE(false) stops the queue's loop, but the chains that
// loop took off the avail ring are still inside the backend, and nothing will ever
// offer them again: last_avail moved past them at dispatch, and the driver has no
// reason to re-publish a buffer it is waiting on. So a pause that also refuses
// their completions loses them outright -- the write reaches the image, the status
// byte is set to OK, and used->idx never advances, leaving the frontend waiting for
// an interrupt that cannot come. Re-enabling does not recover them: vq_start
// re-derives last_avail from the used ring, and the wrap-safe comparison there
// deliberately refuses to rewind it, so the chain stays consumed and unserved.
//
// The gate makes this a state rather than a race. The request is provably inside
// the backend before the pause lands and provably still there afterwards, so what
// the pause did to it is the only variable.
//
// The other half of the separation is asserted by the same case: a paused queue
// takes no NEW work either. Both halves together are what "separate stopping new
// dispatch from allowing existing requests to complete" has to mean, and a fix that
// delivered only the first would be indistinguishable from ignoring the pause.
TEST_F(VhostUserTest, a_paused_queue_completes_what_it_took_and_takes_nothing_more) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    // Declared before `dev`: RecordingFile does not own the backend, so its
    // destruction has to come after the device's shutdown DEFER and its delete.
    test::RecordingFile rf(file);
    rf.gated = true;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rf));
    DEFER(dev->shutdown());
    // Runs BEFORE the shutdown above -- guards fire in reverse declaration order.
    // An orderly teardown waits out in-flight work, and a request parked in a gate
    // nobody is going to open makes that wait out a timeout instead of finishing.
    DEFER(rf.release_gate(1024));

    constexpr uint64_t OFF1 = 9 << 20, OFF2 = 10 << 20;
    auto w1 = pattern(0x5a, 4096);
    auto w2 = pattern(0x3c, 4096);
    // Read inside the body and asserted outside it: the mapping dies with
    // run_frontend, and a count that came back wrong should be reported as the
    // number it was rather than collapsed into an errno.
    uint64_t arrivals_paused = 0;
    uint16_t used_paused = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        constexpr uint16_t SLOT = SLOTS - 1;
        memcpy(fe.mem + MockFrontend::data_off(SLOT), w1.data(), w1.size());
        fe.submit(SLOT, T_OUT, OFF1 >> 9, (uint32_t)w1.size(), false);
        if (!fe.kick()) return EIO;
        // Bounded, and the bound is the assertion's precondition rather than a
        // hope: a request that never reached the backend would make everything
        // below pass for the wrong reason.
        for (int i = 0; i < 5000 && !rf.arrivals.load(); i++)
            ::usleep(1000);
        if (!rf.arrivals.load()) return EIO;

        if (!fe.set_vring_enable(false)) return EPROTO;
        rf.release_gate(1024);

        // The completion has to land while the queue is STILL paused. Waiting for
        // a re-enable first would not separate the two halves of the pause at all:
        // a restarted loop re-reads the ring, and this chain is no longer in it.
        uint32_t head = VQ_NUM, ulen = 0;
        if (!fe.collect(&head, &ulen, 5000)) return ETIMEDOUT;
        // do_request's own errno for a used elem id that is not this slot's
        if (head >= VQ_NUM || fe.slot_of_head[head] != (int16_t)SLOT)
            return EPROTO;
        if (*(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT)) != S_OK) return EIO;

        // And a pause must leave the queue usable, which is the other way this
        // could have been broken: served but never restarted. The second chain goes
        // in while the queue is STILL paused, so the same case pins both halves of
        // the separation -- what was already taken completes, and nothing new is
        // taken -- instead of leaving the second half to be assumed.
        constexpr uint16_t SLOT2 = SLOTS - 2;
        memcpy(fe.mem + MockFrontend::data_off(SLOT2), w2.data(), w2.size());
        fe.submit(SLOT2, T_OUT, OFF2 >> 9, (uint32_t)w2.size(), false);
        if (!fe.kick()) return EIO;
        // 100 ms is twenty of the engine's own KICK_FALLBACK_US re-reads, so a loop
        // that was somehow still alive would have found this chain many times over.
        // A negative needs a window the positive would have fitted in, and this is
        // the window; the kick above is what makes the chain findable at all.
        ::usleep(100 * 1000);
        arrivals_paused = rf.arrivals.load();
        used_paused = fe.used_idx_now();

        if (!fe.set_vring_enable(true)) return EPROTO;
        head = VQ_NUM;
        ulen = 0;
        if (!fe.collect(&head, &ulen, 5000)) return ETIMEDOUT;
        if (head >= VQ_NUM || fe.slot_of_head[head] != (int16_t)SLOT2)
            return EPROTO;
        if (*(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT2)) != S_OK) return EIO;
        return 0;
    });
    EXPECT_EQ(0, rc);
    // The whole of the reported defect is that the backend and the guest disagree:
    // the write landed and the guest was never told. Both halves are checked.
    EXPECT_EQ(0, verify_backend(OFF1, w1));
    EXPECT_EQ(0, verify_backend(OFF2, w2));
    // Neither the backend nor the ring saw the second chain while the queue was
    // paused. FLUSH is negotiated on this connection, so a WRITE is one recorded IO
    // and not two, which is what makes 1 the number rather than a lower bound.
    EXPECT_EQ(1u, (uint32_t)arrivals_paused);
    EXPECT_EQ(1, (int)used_paused);
}

// The reconfiguration paths quiesce a queue before they replace something its
// in-flight requests are using, and "quiesce" only means what the requests do next.
// SET_VRING_CALL is the case where the difference is directly observable: the
// handler stops the loop, drains, and only then closes the interrupt fd and installs
// the replacement. A request that finishes during that drain must publish into the
// ring and signal the fd that was installed when it was DISPATCHED. Dropping the
// drain would not lose the completion -- the ring survives a call-fd swap -- but it
// would move the interrupt to a descriptor the request never belonged to, and a
// frontend that already closed its end of the old one would never see it.
//
// The gate is opened from a photon coroutine rather than from the frontend body,
// because the frontend is blocked inside transact() for as long as the handler
// drains: nothing on that thread can release the request the handler is waiting for.
TEST_F(VhostUserTest, in_flight_requests_finish_before_a_call_fd_swap) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    test::RecordingFile rf(file);
    rf.gated = true;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rf));
    DEFER(dev->shutdown());
    DEFER(rf.release_gate(1024));

    // Signalled by the frontend once the request is parked, so the 50 ms runs from
    // the moment the handler can possibly be inside its drain, not from the start
    // of the case.
    photon::semaphore armed(0);
    auto rth = photon::thread_create11([&] {
        armed.wait(1);
        photon::thread_usleep(50 * 1000);
        rf.release_gate(4096);
    });
    photon::thread_enable_join(rth);

    constexpr uint64_t OFF = 11 << 20;
    auto w = pattern(0x77, 4096);
    int old_callfd = -1;
    uint64_t old_signalled = 0, new_signalled = 0;
    // Ours from replace_callfd() on: the mock's destructor closes only the
    // replacement it now holds.
    DEFER(if (old_callfd >= 0) ::close(old_callfd));
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        // negotiate()'s own SET_VRING_CALL is answered with one signal on the fd it
        // installed, which is not this case's subject. Discard it so that the count
        // below can only have come from the completion.
        (void)fe.callfd_drain();
        constexpr uint16_t SLOT = SLOTS - 1;
        memcpy(fe.mem + MockFrontend::data_off(SLOT), w.data(), w.size());
        fe.submit(SLOT, T_OUT, OFF >> 9, (uint32_t)w.size(), false);
        if (!fe.kick()) return EIO;
        for (int i = 0; i < 5000 && !rf.arrivals.load(); i++)
            ::usleep(1000);
        if (!rf.arrivals.load()) return EIO;

        armed.signal(1);
        // Returns only once the handler has drained, swapped and restarted.
        if (!fe.replace_callfd(&old_callfd)) return EPROTO;
        // The handler drained before it replied, so the completion is already in the
        // ring and has already signalled whichever descriptor it belonged to. Read
        // BOTH counters before collect(): collect() drains the interrupt fd it polls,
        // which would eat the one signal that installing a call fd is itself
        // documented to send (see vring_call_signals_once).
        uint64_t v = 0;
        while (::read(old_callfd, &v, sizeof(v)) == (ssize_t)sizeof(v)) old_signalled += v;
        v = 0;
        while (::read(fe.callfd, &v, sizeof(v)) == (ssize_t)sizeof(v)) new_signalled += v;
        uint32_t head = VQ_NUM, ulen = 0;
        if (!fe.collect(&head, &ulen, 5000)) return ETIMEDOUT;
        // do_request's own errno for a used elem id that is not this slot's
        if (head >= VQ_NUM || fe.slot_of_head[head] != (int16_t)SLOT)
            return EPROTO;
        if (*(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT)) != S_OK) return EIO;
        return 0;
    });
    photon::thread_join((photon::join_handle*)rth);
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(OFF, w));
    ASSERT_GE(old_callfd, 0);
    // Two-sided, and both sides are equalities on an eventfd counter, so neither can
    // pass by never having been signalled at all. The completion belongs to the
    // descriptor that was installed when it was DISPATCHED: exactly one signal there.
    // The replacement gets exactly the one its own install sends, and no more -- a
    // handler that swapped the fd before draining would move the completion's signal
    // across, and both counts would be wrong at once.
    EXPECT_EQ(1u, (uint32_t)old_signalled);
    EXPECT_EQ(1u, (uint32_t)new_signalled);
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
// for as long as we like -- not for one KICK_FALLBACK_US interval. (This case sets
// no queue_depth, so the cap is the ring.) Draining first therefore completes all N,
// and each half of that depends on state the drain has not touched yet: the queue is
// still enabled, so the loop and every completion's redispatch go on admitting the
// rest, and the engine's `stopping` is still clear, so what they admit publishes.
// Stopping the queue first advances the used ring by 0 and not by VQ_NUM -- measured
// over five repeats of the order mutant, because the reasoning that predicted VQ_NUM
// was wrong. The stop sets the engine's `stopping` before it waits out in-flight
// work, and `stopping` is the one fact that legitimately retires a request, so the
// parked ones are declined as well: the 8 behind the cap are stranded and the VQ_NUM
// in front of it go unpublished.
// What the completion path's change DID move is which term produces that 0. It used
// to be the transport's enable flag too, so the wrong order was detected twice over;
// now it is `stopping` alone. Dropping that term from both gates is therefore not
// witnessed here -- with the order correct nothing sets `stopping` until after the
// drain -- and the teardown case in the engine's own suite is what pins it.
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
// is that IO STILL WORKS afterwards, which every row below is measured against.
//
// The rows disagree about one thing each, so a rejection says which check did it:
// the count against the fds, the count against the ceiling, the count against the
// floor, the count against the payload length that actually arrived.
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
        // Fields in declaration order: the region count the payload DECLARES, the
        // number of regions its declared LENGTH covers, and the number of
        // descriptors actually sent with it.
        struct { uint32_t nregions, payload_regions; int nfds; } bad[] = {
            {2, 1, 1},   // fewer fds than declared regions
            {9, 1, 1},   // past the 8-region payload
            {0, 1, 1},   // and the other direction
            // Zero regions, zero fds, and a payload exactly the size of the fixed
            // prefix: the literal empty memory table. Every count agrees with every
            // other count, so the floor on nregions is the ONLY thing that rejects
            // it -- this is the row that gives that floor a witness. Drop the floor
            // and the message is accepted, the live table is unmapped, and the IO
            // below never completes.
            {0, 0, 0},
            // Two regions declared, two fds sent, and a payload long enough for one.
            // Every count agrees, so this is the row that reads the declared count
            // against the length that actually arrived.
            {2, 1, 2},
        };
        for (auto& b : bad) {
            uint64_t ack = 0;
            if (!fe.set_mem_table_mismatched(b.nregions, b.nfds, &ack, b.payload_regions))
                return EPROTO;
            if (ack == 0) {
                LOG_ERROR("SET_MEM_TABLE was accepted with ` regions, ` fds and a ` region payload", b.nregions, b.nfds, b.payload_regions);
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
            vhost_user_msg m, r;
            memset(&m, 0, sizeof(m));
            m.request = 9999;   // no such request: handle_msg's default arm
            m.size = 8;
            bool ok = fe.transact(&m, &r, &efd, 1);
            ::close(efd);       // our copy; the device's is what is being counted
            if (!ok) return EPROTO;
        }
        // An ack is NOT the synchronisation this count needs: msg_loop registers
        // the DEFER that closes a message's fds inside its loop body, before
        // handle_msg, so it runs when the iteration ENDS -- after the default arm
        // has already written that iteration's ack. Rounds 1-7 are covered by the
        // next round's recv, but the last round's close is still in flight when we
        // count, and this body runs on its own OS thread, so it is a straight race
        // (measured: 16 of 50 repeats failed). One more unrecognised message
        // carrying NO fd is the barrier -- its ack cannot be written until the
        // previous iteration has ended, and it adds nothing of its own to count.
        vhost_user_msg b, br;
        memset(&b, 0, sizeof(b));
        b.request = 9999;
        b.size = 8;
        if (!fe.transact(&b, &br)) return EPROTO;
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

        vhost_user_msg a, b, c;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        memset(&c, 0, sizeof(c));
        a.request = VHOST_USER_SET_OWNER;      a.size = 0;   // asks for nothing
        b.request = VHOST_USER_GET_FEATURES;   b.size = 0;   b.flags = VHOST_USER_NEED_REPLY_MASK;
        c.request = VHOST_USER_GET_QUEUE_NUM;  c.size = 0;   c.flags = VHOST_USER_NEED_REPLY_MASK;
        if (!fe.send_pipeline({&a, &b, &c})) return EPROTO;

        vhost_user_msg r;
        int got = 0;
        // the two replies must both arrive, and in order: under the over-read the
        // second one never came at all and this timed out
        if (!fe.recv(&r, nullptr, &got, 3000)) return ETIMEDOUT;
        if (r.request != VHOST_USER_GET_FEATURES || !(r.flags & VHOST_USER_REPLY_MASK)) return EPROTO;
        if (r.size != 8 || r.payload.u64 != fe.features) return EPROTO;
        if (!fe.recv(&r, nullptr, &got, 3000)) return ETIMEDOUT;
        if (r.request != VHOST_USER_GET_QUEUE_NUM || !(r.flags & VHOST_USER_REPLY_MASK)) return EPROTO;
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

        vhost_user_msg m;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_MEM_TABLE;
        m.flags = VHOST_USER_VERSION;
        m.size = 0xFFFFFFF0u;
        // the 12-byte header alone, deliberately: send() would try to write
        // m.size bytes of payload that does not exist
        size_t hdr = offsetof(vhost_user_msg, payload);
        if (::send(fe.fd, &m, hdr, 0) != (ssize_t)hdr) return EPROTO;

        // The backend must end the session ITSELF, and promptly. Waiting for EOF
        // rather than for a missing reply is the whole test: an unbounded payload
        // read also produces no reply, it just blocks forever waiting for bytes
        // that never come, so "no reply" cannot tell the two apart and the wait has
        // to distinguish a close from a stall.
        int eofrc = fe.expect_eof();
        if (eofrc) return eofrc;   // ETIMEDOUT: still blocked on the phantom payload
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

// The finding's own repro: SET_MEM_TABLE declaring a zero-length payload. recv_msg
// bounded only the MAXIMUM, so the union arrived zeroed and nregions read back as
// 0 -- which agreed with the zero fds travelling with it, passed the handler's
// region/fd check, unmapped the live table, and was acked as a SUCCESS. The device
// was then left with nothing to translate a vring address through, and the frontend
// had been told the table was installed.
//
// A payload too short to hold the field about to be read is a disagreement about
// FRAMING and not a bad value, so it ends the session the way an oversized payload
// already did. The assertions are the two halves of that: no reply at all, and a
// reconnect that is served normally -- reading back, on the new session, what the
// old one wrote.
TEST_F(VhostUserTest, a_truncated_mem_table_ends_the_session_and_a_reconnect_still_serves) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x5C, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        // Live and serving FIRST: the unguarded backend did not merely reject this
        // message badly, it destroyed a device that was working.
        if (fe.write_dev(3 << 20, w.data(), w.size()) != S_OK) return EIO;
        if (!fe.send_truncated(VHOST_USER_SET_MEM_TABLE, 0)) return EPROTO;
        int e = fe.expect_eof();
        if (e) {
            LOG_ERROR("SET_MEM_TABLE with a zero-length payload did not end the session: `", e);
            return e;
        }
        fe.close_conn();

        MockFrontend fe2;
        if (!fe2.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe2.negotiate(false)) return EPROTO;
        std::vector<char> rb(w.size());
        if (fe2.read_dev(3 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(3 << 20, w));
}

// The same bound on every other message whose handler reads a fixed structure. Each
// row connects fresh, because the previous row ended its session, and sends a
// payload shorter than the fields the handler is about to read.
//
// Each row negotiates first, and not only to reach a realistic state: REPLY_ACK is
// what makes an accepted message answer at all, so without it an accepted row would
// sit silent and be indistinguishable from a backend that hung. Negotiated, the two
// refusals come apart cleanly -- a reply means the message was processed, EOF means
// the framing was rejected -- and only the second is correct for a message whose
// payload does not contain the field its handler reads.
//
// Rows whose message would have been REJECTED on its contents are in the table too,
// a vring num of zero and an address that translates to nothing among them. Their
// error ack is a different observable from a closed session, and it is the wrong one
// here.
TEST_F(VhostUserTest, truncated_payloads_end_the_session) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    struct Row { int32_t request; uint32_t size; const char* what; };
    static const Row rows[] = {
        {VHOST_USER_SET_FEATURES,     0, "the feature word"},
        {VHOST_USER_SET_VRING_NUM,    4, "the index but not the num"},
        {VHOST_USER_SET_VRING_ADDR,   8, "the index and flags but none of the addresses"},
        {VHOST_USER_SET_VRING_ENABLE, 0, "the index"},
        {VHOST_USER_GET_CONFIG,       4, "the offset but not the size"},
    };
    auto w = pattern(0xB4, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        for (auto& r : rows) {
            MockFrontend t;
            if (!t.connect_to(SOCK_PATH)) return ECONNREFUSED;
            if (!t.negotiate(false)) return EPROTO;
            if (!t.send_truncated(r.request, r.size)) return EPROTO;
            int e = t.expect_eof();
            if (e) {
                LOG_ERROR("` truncated to ` bytes did not end the session: `", r.what, r.size, e);
                return e;
            }
        }
        // Five sessions ended mid-message. The listener, the device and its queue
        // have to be untouched by all of them, which is what makes this a test of
        // the framing check rather than of the teardown that follows one.
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.write_dev(9 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> rb(w.size());
        if (fe.read_dev(9 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(9 << 20, w));
}

// The finding's second half. mem.clear() ran BEFORE the new regions were mapped, so
// an mmap failure could not honour the promise the validation above it was written
// to keep -- that a rejected message leaves the device serving exactly as it was. It
// stopped every queue, unmapped the live table, and then error-acked a message whose
// only defect was that ONE region of the replacement could not be mapped.
//
// The replacement is now built complete and switched in only once every region is
// mapped, so a failure costs nothing but itself: no queue is stopped, no mapping is
// dropped, and the error ack means what it says. The assertions are the ones
// bad_mem_table established as the real ones -- not the ack, but that IO still works
// afterwards, here on the SAME session and against the table it already had.
//
// A zero-length region is the lever, and it is deliberately not rejected earlier as
// a bad value: one failure path is easier to reason about than two, and this is the
// same path a real ENOMEM, or a process at its mapping limit, takes.
TEST_F(VhostUserTest, a_region_that_cannot_be_mapped_leaves_the_running_table_alone) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto first = pattern(0x31, 4096);
    auto second = pattern(0x62, 4096);
    int leaked = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.write_dev(11 << 20, first.data(), first.size()) != S_OK) return EIO;

        int fds_before = MockFrontend::count_fds();
        if (fds_before <= 0) return ENOSYS;
        uint64_t ack = 0;
        if (!fe.set_mem_table_unmappable(1, &ack)) return EPROTO;
        if (ack == 0) {
            LOG_ERROR("SET_MEM_TABLE with a zero-length region was accepted");
            return EINVAL;
        }
        // Counted here, with no wait: the descriptor is closed inside the handler,
        // which runs to completion before this ack is written, so receiving the ack
        // is already the synchronisation the count needs. That is NOT true of a
        // message closed by msg_loop's own cleanup, which runs after the reply --
        // see unrecognised_message_fds for why that one has to wait.
        leaked = MockFrontend::count_fds() - fds_before;

        if (fe.write_dev(12 << 20, second.data(), second.size()) != S_OK) return EIO;
        std::vector<char> rb(first.size());
        if (fe.read_dev(11 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(first.data(), rb.data(), rb.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, leaked);
    EXPECT_EQ(0, verify_backend(11 << 20, first));
    EXPECT_EQ(0, verify_backend(12 << 20, second));
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
        char buf[512] = {};
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

        char buf[512] = {};

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

        char buf[512] = {};
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

        char buf[512] = {};
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

// The negotiation gate, from the side nothing else in this file looks at. Every
// other case here settles VIRTIO_RING_F_EVENT_IDX: `decline` is written exactly
// once in the whole file, to F_BLK_FLUSH, which is bit 9. So until this case the
// transport's per-queue event_idx store -- the one in the SET_FEATURES handler --
// had only ever been asked for the value it computes when bit 29 IS present, and
// deriving it from offer_features instead of from the negotiated word, or storing
// an unconditional true, was invisible to all of them: each kept reading a
// used_event that collect() obligingly wrote, and avail->flags -- the only input a
// flags-mode guest actually gives the device -- was never set to anything but its
// memset 0, so vring_need_irq answered true every time and looked identical to the
// EVENT_IDX arithmetic agreeing.
//
// What this adds over test-blk-vq's flags_mode_answers_from_the_driver_suppression_bit
// is the plumbing, not the predicate. That case stores srv.event_idx by hand and
// calls should_notify directly, so it cannot see which of the two words the
// transport read; this one negotiates over a real socket and observes the callfd.
//
// THREE requests, and only the third has teeth. Under an unconditional store(true)
// the device is in EVENT_IDX mode while the guest is in flags mode, so it reads
// used_event -- which nothing on this side writes any more -- as 0: request 1 still
// notifies (should_notify's first-decision rule, spent there), request 2 is
// suppressed by need_event(0, 2, 1) = (1 < 1) and request 3 by need_event(0, 3, 2)
// = (2 < 1). Requests 1 and 2 therefore read the SAME under both regimes and
// neither can discriminate, however right their assertions look; request 3 is the
// one that goes red, and it goes red on the notification rather than on "not
// served" -- the kick half is the mock's own and is unaffected by what the device
// believes, so the request is served either way and the failure is attributed.
TEST_F(VhostUserTest, a_flags_mode_session_notifies_from_avail_flags_alone) {
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

        // A precondition, not a claim about the device, and read BEFORE the settled
        // word is asserted on: declining a bit that was never offered changes
        // nothing, and the case would then be exercising flags mode for a reason
        // nobody negotiated while believing it had tested the gate. Same shape as
        // the FLUSH precondition in accepting_flush_leaves_the_device_in_write_back.
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_FEATURES; m.size = 0;
        ASSERT_TRUE(fe.transact(&m, &r));
        // Copied out before the macro sees it: a scalar read in place is legal per
        // the wire header's access rule, but `r` is a packed struct and nothing
        // here gains from letting a gtest macro bind to one of its members.
        const uint64_t offer = r.payload.u64;
        ASSERT_TRUE(offer & F_RING_EVENT_IDX)
            << "bit 29 was not offered, so declining it below is a no-op";

        fe.decline = F_RING_EVENT_IDX;
        ASSERT_TRUE(fe.negotiate(false));
        ASSERT_FALSE(fe.features & F_RING_EVENT_IDX)
            << "bit 29 survived the decline; this case would exercise EVENT_IDX and pass vacuously";

        // SET_VRING_CALL signals once on install, so from here on a readable callfd
        // means a completion notified and nothing else.
        (void)fe.callfd_drain();
        char buf[512] = {};

        // ---- request 1: interrupts enabled, so the device's vring_need_irq
        // ---- answers notify. §2.7.7.2 tells it to ignore this bit only once bit
        // ---- 29 is negotiated, which this session did not.
        fe.set_avail_no_interrupt(false);
        const uint16_t s1 = 0;
        ASSERT_EQ(0, fe.submit(s1, T_OUT, 0, sizeof(buf), false));
        ASSERT_TRUE(fe.kick_if_needed());
        ASSERT_TRUE(fe.served_within(s1)) << "request 1 was not served at all";
        EXPECT_EQ(0, (int)*(uint8_t*)(fe.mem + fe.status_off(s1)))
            << "served, but with a nonzero virtio-blk status";
        EXPECT_TRUE(fe.callfd_readable(1000))
            << "flags mode with interrupts enabled did not notify";
        (void)fe.callfd_drain();

        // ---- request 2: the driver's suppression bit set, so it MUST NOT ----
        fe.set_avail_no_interrupt(true);
        const uint16_t s2 = 1;
        ASSERT_EQ(0, fe.submit(s2, T_OUT, 0, sizeof(buf), false));
        ASSERT_TRUE(fe.kick_if_needed());
        ASSERT_TRUE(fe.served_within(s2)) << "request 2 was not served at all";
        EXPECT_EQ(0, (int)*(uint8_t*)(fe.mem + fe.status_off(s2)))
            << "served, but with a nonzero virtio-blk status";
        // Suppression is about the notification, not the work, and the two lines
        // above are what keep this one honest: a request nobody served also raises
        // no interrupt, so on its own this would pass against a dead queue. The
        // bound is the 50 ms interrupt_suppressed_by_used_event uses, and here it is
        // load-bearing in a second way: the flag for request 3 must not be cleared
        // while this request's notify decision may still be pending, or a decision
        // that read NO_INTERRUPT would land after the clear and be counted as
        // request 3's. complete_req runs serve_chain's status-byte write and then
        // the used append and should_notify with no yield between them, so a poll
        // that blocked its full 50 ms and saw nothing is proof the decision was
        // taken, and took it with the flag still set.
        EXPECT_FALSE(fe.callfd_readable(50))
            << "interrupted although avail->flags asked it not to (§2.7.7.2)";
        (void)fe.callfd_drain();

        // ---- request 3: the bit cleared again, so it MUST notify again ----
        // The discriminating half; see the block comment for why 1 and 2 cannot be.
        fe.set_avail_no_interrupt(false);
        const uint16_t s3 = 2;
        ASSERT_EQ(0, fe.submit(s3, T_OUT, 0, sizeof(buf), false));
        ASSERT_TRUE(fe.kick_if_needed());
        ASSERT_TRUE(fe.served_within(s3)) << "request 3 was not served at all";
        EXPECT_EQ(0, (int)*(uint8_t*)(fe.mem + fe.status_off(s3)))
            << "served, but with a nonzero virtio-blk status";
        EXPECT_TRUE(fe.callfd_readable(1000))
            << "flags mode stopped consulting avail->flags after the first request";
        EXPECT_EQ(1u, fe.callfd_drain()) << "expected exactly one notification";
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

// The same gate, the other direction: a poisoned side-channel. A flags-mode
// frontend does not publish used_event at all -- that slot is part of the
// EVENT_IDX layout -- and §2.7.7.2's "the device MUST ignore the lower bit of
// flags" is conditional on bit 29 having been negotiated, which is to say the
// flags bit REPLACES the index rather than sitting beside it. This case writes the
// slot anyway, with the value that would suppress under EVENT_IDX, and asserts the
// interrupt still arrives: a device that goes on reading used_event after its peer
// declined bit 29 is a device taking input from a channel that peer never wrote,
// and the value it finds there is whatever the previous session or the memset left.
//
// Request 1 cannot discriminate, for the reason the case above gives: should_notify's
// first-decision rule notifies unconditionally whichever regime the device thinks it
// is in. The assertion with teeth is request 2's, where the mutated device computes
// need_event(100, 2, 1) = (uint16)(2 - 100 - 1) = 65437 < 1, i.e. false, and stays
// silent while the correct one answers from avail->flags and notifies.
TEST_F(VhostUserTest, a_flags_mode_session_ignores_a_planted_used_event) {
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
        fe.decline = F_RING_EVENT_IDX;
        ASSERT_TRUE(fe.negotiate(false));
        ASSERT_FALSE(fe.features & F_RING_EVENT_IDX)
            << "bit 29 survived the decline; this case would exercise EVENT_IDX and pass vacuously";

        // Planted AFTER negotiate(), which memsets the whole guest region, and left
        // alone from here on: nothing in this case calls collect(), so the only
        // writer of the slot is this line. 100 is interrupt_suppressed_by_used_event's
        // value and suppresses for the same arithmetic reason.
        fe.set_used_event(100);
        (void)fe.callfd_drain();   // SET_VRING_CALL's one-shot install signal
        char buf[512] = {};

        for (int i = 0; i < 2; i++) {
            const uint16_t slot = (uint16_t)i;
            ASSERT_EQ(0, fe.submit(slot, T_OUT, 0, sizeof(buf), false));
            ASSERT_TRUE(fe.kick_if_needed());
            ASSERT_TRUE(fe.served_within(slot)) << "request " << i << " was not served at all";
            EXPECT_EQ(0, (int)*(uint8_t*)(fe.mem + fe.status_off(slot)))
                << "request " << i << " served, but with a nonzero virtio-blk status";
            EXPECT_TRUE(fe.callfd_readable(1000))
                << "request " << i << " was not notified: a flags-mode device took its "
                   "decision from the planted used_event instead of from avail->flags";
            EXPECT_EQ(1u, fe.callfd_drain()) << "expected exactly one notification";
        }
        // avail->flags was never touched, so it is still the memset 0 that asks for
        // interrupts. Asserted rather than assumed: the case's whole claim is that
        // this bit and not the index is what the device consulted, and a region
        // whose flags had been set elsewhere would make both notifications
        // unexpected rather than expected.
        EXPECT_EQ(0, (int)((vavail*)(fe.mem + L_AVAIL))->flags);
        EXPECT_EQ(100u, fe.get_used_event())
            << "something else wrote the slot; the case's premise is that this plant is "
               "the only used_event the device can find there";
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

// One queue, so the placement is a single fact: the loop coroutine that serves
// every request must be on a pool vcpu, not on the vcpu that called start(). The
// IO is real -- the frontend writes it and reads it back -- so the recorded set is
// populated by an actual serving coroutine.
TEST_F(VhostUserTest, pool_placement) {
    test::TestPool pool(2);
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.pool = pool;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    auto wbuf = pattern(0x5a);
    std::vector<char> rbuf(IO_LEN);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.write_dev(1 << 20, wbuf.data(), wbuf.size()) != S_OK) return EIO;
        if (fe.read_dev(1 << 20, rbuf.data(), rbuf.size()) != S_OK) return EIO;
        return memcmp(wbuf.data(), rbuf.data(), wbuf.size()) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, rc);
    // through `file`, not `rec`: this read runs on the caller's vcpu, so routing it
    // via the probe would record that vcpu as one of the device's own placements
    EXPECT_EQ(0, verify_backend(1 << 20, wbuf));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_FALSE(rec.ran_on(caller));
}

// n > m. The case above draws the cursor exactly once -- one queue, one vq_start,
// one migrate_to_pool -- so what it observes is a landing and not a wrap. Four
// queues over two vcpus is the wrap: WorkPool answers the out-of-range index
// migrate_to_pool always passes with `vcpu_index++ % size`, so four consecutive
// draws come back as two vcpus twice over. What the count at the end witnesses is
// narrower than that, and has to be stated narrowly: RecordingFile keeps a SET of
// vcpus with no queue attributed to any of them, so "two queues each" is a property
// of the code this case cannot observe, and a cursor that saturated after its first
// cycle would read 2 as well. What the count does exclude is the mutant this case
// exists for -- a migrate_to_pool that passed a constant IN-range index instead of
// the out-of-range one parks all four on that index's vcpu and this reads 1.
//
// Every queue has to carry IO and not merely be enabled, because vcpu_count() counts
// what reached the backend: a queue that was migrated and never driven contributes
// nothing to the set, so a case that drove queue 0 alone would read 1 against a
// cursor working perfectly. Sequential IO is nonetheless a complete observation --
// placement is decided once, at the SET_VRING_ENABLE that starts each queue, and the
// per-request coroutines dispatch_avail creates are created on that queue's own vcpu
// and never migrated, so nothing can move after the four enables. Concurrency would
// add no coverage here, only a race to lose.
TEST_F(VhostUserTest, four_queues_over_a_two_vcpu_pool_use_both_vcpus) {
    constexpr uint32_t QUEUES = 4;
    constexpr size_t LEN = 4096;
    test::TestPool pool(2);
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.queues = QUEUES;
    cfg.pool = pool;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    // A distinct seed and a distinct offset per queue, so a read-back cannot be
    // satisfied by another queue's data and a write served into the wrong range
    // still fails its own comparison. 0xcc under the read buffers for the reason
    // two_queues_complete_independently gives: "the device filled this" and "it
    // already held what we expected" must not be the same observation.
    std::vector<char> w[QUEUES], r[QUEUES];
    for (uint32_t q = 0; q < QUEUES; q++) {
        w[q] = pattern((uint8_t)(0x11 * (q + 1)), LEN);
        r[q].resize(LEN, (char)0xcc);
    }

    // The raw off-vcpu helper rather than run_frontend(), for the reason
    // a_queue_above_zero_serves_io_end_to_end gives: the preconditions below are
    // gtest macros and need a void context, and `fe` has to outlive the block so a
    // fatal ASSERT that returns early still leaves here what it wrote before it did.
    MockFrontend fe;
    test::run_off_vcpu([&] {
        ASSERT_TRUE(fe.connect_to(SOCK_PATH));
        ASSERT_TRUE(fe.negotiate(false));
        // Preconditions, and what stops this case passing vacuously. negotiate() asks
        // GET_QUEUE_NUM only once PROTOCOL_F_MQ is settled and leaves queue_num at 1
        // otherwise, so against a device serving fewer queues these two fire and the
        // setup_queue loop is never reached. Without them the same device would fail
        // below as an error ack on a vring index it does not have, which reads as a
        // data-path failure and is not one.
        ASSERT_NE(0u, fe.proto_features & (1ULL << VHOST_USER_PROTOCOL_F_MQ))
            << "protocol MQ was not settled, so this frontend has one queue";
        ASSERT_EQ(QUEUES, fe.queue_num)
            << "the peer learned a count that cannot address queue " << QUEUES - 1;
        for (uint32_t q = 1; q < QUEUES; q++)
            ASSERT_TRUE(fe.setup_queue(q)) << "queue " << q;

        for (uint32_t q = 0; q < QUEUES; q++) {
            uint64_t off = (uint64_t)(q + 1) << 20;
            ASSERT_EQ(S_OK, fe.write_dev(off, w[q].data(), LEN, nullptr, q))
                << "write on queue " << q;
            ASSERT_EQ(S_OK, fe.read_dev(off, r[q].data(), LEN, q))
                << "read on queue " << q;
        }
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);

    for (uint32_t q = 0; q < QUEUES; q++) {
        EXPECT_EQ(0, memcmp(w[q].data(), r[q].data(), LEN))
            << "queue " << q << " did not read back its own write";
        // through `file`, not `rec`, for the reason pool_placement gives: this read
        // runs on the caller's vcpu, so routing it via the probe would record that
        // vcpu as one of the device's own placements
        EXPECT_EQ(0, verify_backend((uint64_t)(q + 1) << 20, w[q]));
    }

    // Both halves, and neither alone is the claim. The count says the four queues
    // covered two vcpus; the ran_on says neither of the two was this one, which is
    // what turns "two" into "the whole pool" -- a queue whose migration was refused
    // stays here and would make the count 3, and a count of 2 built from this vcpu
    // plus one pool vcpu is a pool half used.
    EXPECT_EQ(2u, rec.vcpu_count());
    EXPECT_FALSE(rec.ran_on(caller));
}

// Two devices on ONE pool, one queue each. The cursor is WorkPool's own
// `vcpu_index`, a member of the pool and shared by every caller, so the second
// device continues where the first left off instead of starting over at vcpus[0] --
// which is the entire reason migrate_to_pool hands thread_migrate an out-of-range
// index rather than 0. Nothing else in this file can see that: every other case here
// binds SOCK_PATH, and bind() hands a path to one device at a time, so none of them
// has two serving -- and one device draws consecutive values whichever way the cursor
// is scoped, so a per-device cursor and a pool-wide one are the same observation until
// a second device draws.
//
// One queue per device is what makes the assertion below exact rather than likely.
// Each queue is drawn for exactly once, at the SET_VRING_ENABLE that starts it: the
// SET_VRING_NUM, _ADDR, _KICK and _CALL handlers a negotiate() sends ahead of the
// enable call vq_start too, but its readiness gate wants `enabled`, and in a session
// that settled bit 30 only the enable sets it; a queue whose loop already exists then
// short-circuits ahead of the create, so the enable's call is the only one that
// reaches migrate_to_pool. That scope is a premise and not a given -- a session that
// DECLINED bit 30 gets every queue enabled by its own SET_FEATURES instead, sends no
// enable at all, and so draws three times per queue, at _ADDR/_KICK/_CALL -- which is
// why both frontends below assert the bit settled before either drives anything.
// start()'s own engine probe draws nothing either: check_pool_engines migrates with an
// IN-range index, which is the branch that skips the cursor. Two devices are therefore
// two CONSECUTIVE draws, and `% 2` maps consecutive draws to different vcpus whatever
// the counter held before them.
//
// Disjointness is the strongest claim that follows from that. It is deliberately not
// the stronger-looking "A on the pool's first vcpu, B on its second": that pins where
// the counter stood when A drew, an absolute this transport has no say in, and a pool
// its caller had already drawn from would land A elsewhere while behaving correctly.
// Consecutiveness is all the premise disjointness needs, and one queue per device is
// what buys it. Nor is the assertion weaker than it looks: a migrate_to_pool that
// passed an in-range index draws vcpus[0] for both devices -- a constant 0 and the
// queue index are the same thing when each device has one queue, and 0 is in range --
// and a cursor scoped to the device instead of the pool restarts at 0 for the second
// one. Either way the EXPECT_NE at the end reads two equal pointers.
//
// One RecordingFile per device so each placement stays attributable. A single probe
// behind both backends would report a union, and a union of two is evidence of a
// split only once each device is known to have contributed exactly one vcpu -- the
// pair of counts below, which holds because a device's per-request coroutines run on
// its loop's vcpu and are never migrated.
TEST_F(VhostUserTest, two_devices_share_one_pool) {
    // A second path INSIDE SOCK_DIR, which is what new_device() requires of a
    // sock_path, and so a second basename as well: do_listen() binds the path and
    // answers EEXIST for one already bound, and the serial a device reports to
    // VIRTIO_BLK_T_GET_ID is that basename, so two sockets in one scope sharing it
    // would tell their guests they are the same device. shutdown() removes the node
    // its own device bound; SetUp's sweep of the directory is the backstop for a case
    // that never got that far.
    const std::string sock_b = std::string(SOCK_DIR) + "/vhu2.sock";
    // The pool first: BlkConfig's lifetime contract wants every device using it shut
    // down before it goes, and declaration order is what buys that here.
    test::TestPool pool(2);
    // Both probes before both devices, and neither owning `file`: each has to outlive
    // the shutdown DEFER that issues its own device's last backend IO.
    test::RecordingFile rec_a(file);
    test::RecordingFile rec_b(file);
    auto* caller = photon::get_vcpu();

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.queues = 1;   // exactly one queue: exactly one draw, which is what the
                      // disjointness below is argued from
    cfg.pool = pool;
    auto dev_a = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev_a);
    DEFER(delete dev_a);
    ASSERT_EQ(0, dev_a->start(&rec_a));
    DEFER(dev_a->shutdown());

    auto cfg_b = cfg;
    cfg_b.sock_path = sock_b;
    auto dev_b = ctl->new_device(cfg_b);
    ASSERT_NE(nullptr, dev_b);
    DEFER(delete dev_b);
    ASSERT_EQ(0, dev_b->start(&rec_b));
    DEFER(dev_b->shutdown());

    // Disjoint ranges of one image. The property is placement, and a second
    // TestImage would add a second file without adding an observation -- while a
    // shared range would make the two read-backs unable to tell the devices apart.
    constexpr uint64_t OFF_A = 8 << 20, OFF_B = 9 << 20;
    constexpr size_t LEN = 4096;
    auto wa = pattern(0x6c, LEN), wb = pattern(0xb1, LEN);
    std::vector<char> ra(LEN, (char)0xcc), rb(LEN, (char)0xcc);

    // Sequential, and it can be: the draws happen at each session's enable, so the
    // second device's landing is already decided by the time the first frontend
    // disconnects. Running the two frontends at once would interleave two draws that
    // are consecutive either way.
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        // The regime the one-draw-per-queue argument is scoped to: with bit 30
        // declined the draws move to _ADDR/_KICK/_CALL and the two sessions stop
        // being two consecutive draws, so the EXPECT_NE below would be arguing
        // from a premise this lambda no longer holds.
        if (!(fe.features & (1ULL << VHOST_USER_F_PROTOCOL_FEATURES)))
            return EPROTONOSUPPORT;
        if (fe.write_dev(OFF_A, wa.data(), LEN) != S_OK) return EIO;
        if (fe.read_dev(OFF_A, ra.data(), LEN) != S_OK) return EIO;
        return memcmp(wa.data(), ra.data(), LEN) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, rc);
    rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(sock_b.c_str())) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!(fe.features & (1ULL << VHOST_USER_F_PROTOCOL_FEATURES)))
            return EPROTONOSUPPORT;   // as above: this session has to draw once too
        if (fe.write_dev(OFF_B, wb.data(), LEN) != S_OK) return EIO;
        if (fe.read_dev(OFF_B, rb.data(), LEN) != S_OK) return EIO;
        return memcmp(wb.data(), rb.data(), LEN) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, rc);
    // through `file`, not either probe: these reads run on the caller's vcpu
    EXPECT_EQ(0, verify_backend(OFF_A, wa));
    EXPECT_EQ(0, verify_backend(OFF_B, wb));

    ASSERT_EQ(1u, rec_a.vcpu_count()) << "one queue serves one device from one vcpu";
    ASSERT_EQ(1u, rec_b.vcpu_count()) << "one queue serves one device from one vcpu";
    auto* vcpu_a = rec_a.vcpus()[0];
    auto* vcpu_b = rec_b.vcpus()[0];
    // Kept apart from the EXPECT_NE below because a migration that failed outright
    // fails that one too, and for a reason which is not the cursor: the two vcpus
    // would be equal because both are this one. These two say the split is a split OF
    // THE POOL, which is what makes the inequality about the cursor.
    EXPECT_FALSE(rec_a.ran_on(caller));
    EXPECT_FALSE(rec_b.ran_on(caller));
    EXPECT_NE(vcpu_a, vcpu_b)
        << "both devices' serving coroutines landed on the same pool vcpu";
}

// No pool at all. Identical IO, and the one loop coroutine must be on this vcpu --
// the placement every other case in this file runs with.
TEST_F(VhostUserTest, pool_null_serves_on_the_caller_vcpu) {
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.pool = nullptr;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    auto wbuf = pattern(0x5a);
    std::vector<char> rbuf(IO_LEN);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.write_dev(1 << 20, wbuf.data(), wbuf.size()) != S_OK) return EIO;
        if (fe.read_dev(1 << 20, rbuf.data(), rbuf.size()) != S_OK) return EIO;
        return memcmp(wbuf.data(), rbuf.data(), wbuf.size()) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(1 << 20, wbuf));
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(caller));
}

// WorkPool's cursor is `vcpu_index++ % size`, so a zero-size pool is a SIGFPE the
// moment anything asks it for a vcpu; migrate_to_pool's short-circuit is the only
// thing in the way. Surviving is half the assertion, and staying on the caller's
// vcpu is the other half.
TEST_F(VhostUserTest, empty_pool_falls_back_to_the_caller_vcpu) {
    photon::WorkPool empty(0);   // no vcpus, so no engines to match
    ASSERT_EQ(0, empty.get_vcpu_num());
    test::RecordingFile rec(file);
    auto* caller = photon::get_vcpu();

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.pool = &empty;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rec));
    DEFER(dev->shutdown());

    auto wbuf = pattern(0x5a);
    std::vector<char> rbuf(IO_LEN);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.write_dev(1 << 20, wbuf.data(), wbuf.size()) != S_OK) return EIO;
        if (fe.read_dev(1 << 20, rbuf.data(), rbuf.size()) != S_OK) return EIO;
        return memcmp(wbuf.data(), rbuf.data(), wbuf.size()) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(1u, rec.vcpu_count());
    EXPECT_TRUE(rec.ran_on(caller));
}

// check_pool_engines' integration half: the helper is unit-tested on its own, this
// proves the transport actually asks. A pool whose vcpus cannot host the serving
// coroutines is a configuration error, so start() refuses it up front -- before
// anything is bound or listened on -- and the failed start's rollback leaves no
// socket behind.
TEST_F(VhostUserTest, pool_without_an_event_engine_is_refused) {
    photon::WorkPool bad(2);      // ev_engine defaults to 0: no engine at all
    test::RecordingFile rec(file);

    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.pool = &bad;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    errno = 0;
    EXPECT_EQ(-1, dev->start(&rec));
    EXPECT_EQ(EINVAL, errno);
    EXPECT_EQ(0u, rec.vcpu_count());
    EXPECT_NE(0, ::access(SOCK_PATH, F_OK));   // start failed: no live socket left behind
}

// The spec makes SET_VRING_ENABLE conditional -- it "should be sent only when
// VHOST_USER_F_PROTOCOL_FEATURES has been negotiated" -- and for that same
// condition says a SET_FEATURES "without VHOST_USER_F_PROTOCOL_FEATURES set,
// back-end must enable all rings immediately". A backend that waits for the enable
// therefore serves nothing at all to a frontend that declined bit 30, and it fails
// silently: every control message it did receive was answered normally, so the only
// symptom is I/O that never completes.
TEST_F(VhostUserTest, a_frontend_that_never_negotiates_protocol_features_still_serves) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x3C, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        fe.no_protocol_features = true;
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        // A precondition on the mock, not a claim about the device: this frontend
        // settled no protocol word, which is what makes the absence of
        // SET_VRING_ENABLE legal rather than an oversight. If the mock ever sends
        // the enable again this case stops testing anything, and this is the line
        // that says so instead of reporting a green that means nothing.
        if (fe.proto_features != 0) {
            LOG_ERROR("the frontend settled a protocol word, so it did send SET_VRING_ENABLE");
            return EPROTO;
        }
        if (fe.write_dev(13 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> rb(w.size());
        if (fe.read_dev(13 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(13 << 20, w));
}

// A frontend that sets a feature the device never offered is claiming behaviour
// that does not exist, and the concrete cost is durability rather than tidiness:
// FLUSH accepted without being offered clears write_through, so writes stop being
// synced on the strength of a flush the backend was never asked to support. The
// refusal has to cost the peer its message and not its connection -- a bad value in
// a well-formed message is a semantic violation, not a framing one -- so the case
// also asserts that the session carries on and that a SUBSET of the offer is still
// accepted, which is what keeps the guard from passing by rejecting everything.
TEST_F(VhostUserTest, set_features_rejects_a_bit_the_device_never_offered) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x5E, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.features == 0) return EPROTO;   // nothing to subtract from

        vhost_user_msg m, r;
        // Bit 63: reserved, and far from anything this device offers, so the
        // rejection can only be about it being unoffered.
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_FEATURES; m.size = 8;
        m.payload.u64 = fe.features | (1ULL << 63);
        if (!fe.transact(&m, &r)) return EPROTO;
        if (r.payload.u64 == 0) {
            LOG_ERROR("SET_FEATURES accepted a bit outside the offer, word=", HEX(m.payload.u64));
            return EINVAL;
        }

        // The refused word must not have been applied: the one settled during
        // negotiate() is still in force, so I/O still works on the same session.
        if (fe.write_dev(14 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> rb(w.size());
        if (fe.read_dev(14 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        if (memcmp(w.data(), rb.data(), w.size())) return EILSEQ;

        // And a strict subset is still accepted, or the guard above proves nothing.
        // F_RING_EVENT_IDX is already a mask, unlike the VHOST_USER_* bit numbers.
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_FEATURES; m.size = 8;
        m.payload.u64 = fe.features & ~F_RING_EVENT_IDX;
        if (!fe.transact(&m, &r)) return EPROTO;
        if (r.payload.u64 != 0) {
            LOG_ERROR("SET_FEATURES refused a subset of the offer, word=", HEX(m.payload.u64));
            return EINVAL;
        }
        // Re-settle the full word: leaving the device on the subset would change
        // what the I/O below means, and the case is about the guard, not the subset.
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_SET_FEATURES; m.size = 8;
        m.payload.u64 = fe.features;
        if (!fe.transact(&m, &r, nullptr, 0, /*check_ack=*/true)) return EPROTO;
        if (fe.write_dev(15 << 20, w.data(), w.size()) != S_OK) return EIO;
        if (fe.read_dev(15 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(14 << 20, w));
    EXPECT_EQ(0, verify_backend(15 << 20, w));
}

// The spec marks RESET_OWNER deprecated and recommends a back-end "either ignore
// this message, or use it to disable all rings", recording that the ambiguity arose
// because some back-ends also discarded connection state. Ignoring it is one of the
// two recommended readings and the only one compatible with a frontend that never
// negotiated bit 30: such a frontend has no way to re-enable a ring, so disabling
// here would strand it permanently. This case pins the choice, so that switching to
// the other reading is a decision somebody makes deliberately and sees fail.
TEST_F(VhostUserTest, reset_owner_leaves_the_device_serving) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto first = pattern(0x71, 4096);
    auto second = pattern(0xB2, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.write_dev(16 << 20, first.data(), first.size()) != S_OK) return EIO;

        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_RESET_OWNER; m.size = 0;
        if (!fe.transact(&m, &r)) return EPROTO;
        if (r.payload.u64 != 0) {
            LOG_ERROR("RESET_OWNER was refused; the spec recommends ignoring it");
            return EINVAL;
        }

        // Still serving, on the same session, and the bytes written before the
        // reset are still there: "ignore" means neither the rings nor the memory
        // table moved.
        if (fe.write_dev(17 << 20, second.data(), second.size()) != S_OK) return EIO;
        std::vector<char> rb(first.size());
        if (fe.read_dev(16 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(first.data(), rb.data(), rb.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(16 << 20, first));
    EXPECT_EQ(0, verify_backend(17 << 20, second));
}

// RESET_DEVICE is "only valid if the VHOST_USER_PROTOCOL_F_RESET_DEVICE protocol
// feature is set by the back-end", and this backend does not set it -- so no
// conformant frontend sends the message. Acking it as a success would tell the peer
// that all rings were disabled and all internal state returned to initial, none of
// which happened, and a frontend that believed it would reinitialize a device that
// is still mid-session. Refusing costs the peer nothing: a bad value in a
// well-formed message is a semantic violation here too, so the session carries on.
TEST_F(VhostUserTest, reset_device_is_refused_when_its_protocol_feature_is_not_advertised) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    auto w = pattern(0x9D, 4096);
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        // The precondition this whole case rests on: the protocol word the device
        // offered does not carry the reset feature. Asserted, not assumed, because
        // the day it does is the day refusing this message becomes wrong.
        if (fe.proto_features & (1ULL << VHOST_USER_PROTOCOL_F_RESET_DEVICE)) {
            LOG_ERROR("the device advertises the reset protocol feature, so refusing RESET_DEVICE is no longer correct");
            return EINVAL;
        }
        if (fe.write_dev(18 << 20, w.data(), w.size()) != S_OK) return EIO;

        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_RESET_DEVICE; m.size = 0;
        if (!fe.transact(&m, &r)) return EPROTO;
        if (r.payload.u64 == 0) {
            LOG_ERROR("RESET_DEVICE was acked as a success without the protocol feature");
            return EINVAL;
        }

        // Nothing was reset: the same session still serves, and what it wrote
        // before the refused message is still on the backend.
        std::vector<char> rb(w.size());
        if (fe.read_dev(18 << 20, rb.data(), rb.size()) != S_OK) return EIO;
        return memcmp(w.data(), rb.data(), w.size()) ? EILSEQ : 0;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(0, verify_backend(18 << 20, w));
}

// The resume cursor on adoption is the one the frontend gave us in SET_VRING_BASE,
// not used->idx. used->idx counts completions and out-of-order completion means it
// is not a contiguous prefix of consumed avail entries: resuming from it both loses
// uncompleted entries below it and re-serves completed ones above it, producing
// duplicate used elements for heads the driver has already reclaimed. BASE is the
// previous backend's own last_avail -- GET_VRING_BASE drains, stops and returns it
// -- so resuming there accepts that dispatched-but-uncompleted entries are genuinely
// unrecoverable without publishing duplicates.
//
// Discriminating by construction: BASE (7) differs from the planted used_idx (5),
// and SEVEN driven requests leave avail entries 5 and 6 holding real heads that
// point at slots 5 and 6 -- the two entries a used_idx resume replays, each with a
// status byte a replay has to write. Two oracles watch them, because neither one
// alone is race-free. serve_chain writes the status byte BEFORE complete_req
// appends the used element, so a bare read right after the status poll below can
// still see the planted 5 in the honoured regime; the used ring is therefore polled
// until it stops moving, bounded, and the settled value is the assertion: 6 honours
// BASE, 8 resumed from used_idx and replayed first. The sentinels re-armed on slots
// 5 and 6 are the order-independent oracle: a replay overwrites them before the
// entry at BASE is even dispatched -- dispatch consumes entries in cursor order on
// one vcpu and a request coroutine does not yield before its status write -- while
// a device that honours BASE never touches them at all. Neither avail->idx nor
// avail_event can answer this question: the first is driver-owned and submit()
// wrote it; the second ends at 8 under both regimes.
TEST_F(VhostUserTest, adopt_resumes_from_the_frontends_base_not_used_idx) {
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

        // Drive SEVEN requests to completion: slots 0..6, so avail entries 5 and 6
        // hold the heads that point at slots 5 and 6.
        char buf[512] = {};
        for (int i = 0; i < 7; i++)
            ASSERT_EQ(0, fe.write_dev((uint64_t)i * 512, buf, sizeof(buf)));

        ASSERT_TRUE(fe.set_vring_enable(false));   // vq_stop() JOINS the loop

        // The fiction: a previous backend consumed all seven entries and completed
        // five before it died, so entries 5 and 6 are dispatched-but-uncompleted --
        // the loss a BASE resume accepts -- and its own last_avail (7) is what the
        // frontend hands back. Roll the ring's used index back to 5 to plant it.
        const uint16_t BASE = 7;
        ((vused*)(fe.mem + L_USED))->idx = 5;
        ((vavail*)(fe.mem + L_AVAIL))->idx = BASE;
        fe.used_idx = 5;
        fe.avail_idx = BASE;
        fe.set_used_event(BASE);
        // Re-arm the sentinels on the two entries a used_idx resume replays:
        // round 1 left both at S_OK, and submit() arms only the slot it builds.
        *(uint8_t*)(fe.mem + fe.status_off(5)) = 0xff;
        *(uint8_t*)(fe.mem + fe.status_off(6)) = 0xff;
        ASSERT_TRUE(fe.restart_with_base(BASE));
        (void)fe.callfd_drain();

        // Submit ONE request at slot BASE. A device that resumed from used_idx (5)
        // serves slots 5 and 6 again before reaching BASE; one that honours BASE
        // serves exactly slot BASE.
        ASSERT_EQ(0, fe.submit(BASE, T_OUT, 0, sizeof(buf), false));
        ASSERT_TRUE(fe.kick());

        bool served = false;
        for (int i = 0; i < 2000 && !served; i++) {
            served = (*(uint8_t*)(fe.mem + fe.status_off(BASE)) != 0xff);
            if (!served) ::usleep(1000);
        }
        ASSERT_TRUE(served) << "the request at BASE was not served";
        EXPECT_EQ(0, (int)*(uint8_t*)(fe.mem + fe.status_off(BASE)))
            << "served, but with a nonzero virtio-blk status";

        // Settle, then read: the status byte precedes the used append and a
        // replay's extra appends land in an order nothing promises from here, so
        // wait for the ring to stop moving -- 50 stable 1 ms polls, 5 s bound --
        // and assert on the settled value.
        uint16_t settled = 0, prev = 0;
        int stable = 0;
        for (int i = 0; i < 5000 && stable < 50; i++) {
            settled = ((vused*)(fe.mem + L_USED))->idx;
            stable = (settled == prev) ? stable + 1 : 0;
            prev = settled;
            ::usleep(1000);
        }
        EXPECT_EQ(6u, (unsigned)settled)
            << "used->idx settled at " << (unsigned)settled << ", not 6: the planted"
               " 5 plus the one request at BASE. 8 is a resume from used_idx that"
               " replayed entries 5 and 6";
        // The order-independent half: both sentinels were re-armed above, and a
        // replay writes S_OK over them before the entry at BASE is dispatched.
        EXPECT_EQ(0xff, (int)*(uint8_t*)(fe.mem + fe.status_off(5)))
            << "entry 5 was re-served after the restart: the device resumed from"
               " used_idx, not from BASE";
        EXPECT_EQ(0xff, (int)*(uint8_t*)(fe.mem + fe.status_off(6)))
            << "entry 6 was re-served after the restart: the device resumed from"
               " used_idx, not from BASE";
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

// The other half of the same resume: what happens when BASE is not a cursor this
// ring could have produced. SET_VRING_BASE lets the frontend put last_avail
// anywhere, and vq_start's only defence is to snap an impossible one back to the
// used_idx it just read from the live ring. For that defence to be sound the bound
// has to be the ring's own width: a split ring of num entries holds at most num
// outstanding, so last_avail - used_idx is a count and cannot exceed num. Half the
// 16-bit counter space reads like the same test and is not -- it agrees with the
// invariant for every num up to 16384 and is wrong at MAX_VRING_NUM, where a
// legitimately saturated 32768-entry ring sits exactly 0x8000 past used_idx and a
// correct cursor would be discarded.
//
// Asserted on avail_event rather than on served I/O because the engine publishes
// avail_event == last_avail as an invariant, so what the driver can read back out
// of the used ring IS the cursor vq_start settled on. A BASE 1000 past used_idx on
// a 256-entry ring is adopted by the half-space bound and snapped by the ring's
// own, and the two readings differ by exactly that.
TEST_F(VhostUserTest, adopt_snaps_a_base_the_ring_cannot_vouch_for) {
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
            << "bit 29 was not negotiated, so avail_event is never published and "
               "this case would read back a stale zero and pass for the wrong reason";

        // Five requests to completion, so used idx == avail idx == 5 and the ring is
        // coherent with nothing outstanding. The cursor under test is then the only
        // incoherent thing in the picture.
        char buf[512] = {};
        for (int i = 0; i < 5; i++)
            ASSERT_EQ(0, fe.write_dev((uint64_t)i * 512, buf, sizeof(buf)));
        ASSERT_EQ(5, fe.avail_idx);
        ASSERT_TRUE(fe.set_vring_enable(false));   // vq_stop() JOINS the loop

        // Far enough past the ring to be unambiguous, well inside half the counter
        // space so the bound under test is the only thing that can catch it.
        const uint16_t BASE = 5 + 1000;
        fe.set_used_event(5);
        ASSERT_TRUE(fe.restart_with_base(BASE));
        (void)fe.callfd_drain();

        EXPECT_EQ(5, fe.get_avail_event())
            << "a BASE 1000 entries past used_idx was adopted on a " << VQ_NUM
            << "-entry ring; that many entries cannot be outstanding at once, so the"
               " cursor describes a state this queue was never in";

        // And the snap is more than a tidier number: resuming at used_idx is what
        // makes the next entry the driver publishes the one that gets served.
        ASSERT_EQ(0, fe.submit(5, T_OUT, 0, sizeof(buf), false));
        ASSERT_TRUE(fe.kick());
        bool served = false;
        for (int i = 0; i < 2000 && !served; i++) {
            served = (*(uint8_t*)(fe.mem + fe.status_off(5)) != 0xff);
            if (!served) ::usleep(1000);
        }
        EXPECT_TRUE(served) << "the request published at the snapped cursor was not served";
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

// The boundary of that bound, from both sides. A ring of num entries can hold
// exactly num outstanding: the driver's free count starts at num and dispatch_avail's
// own cap is `in_flight >= num`, so a predecessor that consumed every entry and
// completed none leaves last_avail precisely num past used_idx. That is a full ring,
// not a stale record, and snapping it loses the whole ring -- the same loss the case
// above exists to prevent, reached by an off-by-one in the other direction. One entry
// past it is impossible and has to snap, or the bound is not the ring's width but
// something looser that only happens to agree with it here.
TEST_F(VhostUserTest, adopt_keeps_a_full_ring_cursor_and_snaps_one_entry_past_it) {
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
            << "bit 29 was not negotiated, so avail_event is never published and "
               "this case would read back a stale zero and pass for the wrong reason";

        char buf[512] = {};
        for (int i = 0; i < 5; i++)
            ASSERT_EQ(0, fe.write_dev((uint64_t)i * 512, buf, sizeof(buf)));
        ASSERT_TRUE(fe.set_vring_enable(false));   // vq_stop() JOINS the loop
        fe.set_used_event(5);

        // used idx 5, avail idx 5 + VQ_NUM: every slot the driver owns is consumed
        // and none of them completed, which is the fullest a ring this wide can be.
        const uint16_t FULL = 5 + VQ_NUM;
        ((vused*)(fe.mem + L_USED))->idx = 5;
        ((vavail*)(fe.mem + L_AVAIL))->idx = FULL;
        fe.used_idx = 5;
        fe.avail_idx = FULL;
        ASSERT_TRUE(fe.restart_with_base(FULL));
        (void)fe.callfd_drain();
        EXPECT_EQ(FULL, fe.get_avail_event())
            << "a cursor exactly " << VQ_NUM << " entries past used_idx -- a full "
            << VQ_NUM << "-entry ring, not an impossible one -- was snapped back";

        // One entry past the width. avail idx goes back to 5 so that the snap this
        // expects leaves last_avail equal to it and nothing is dispatched: the
        // assertion is on the cursor, and a ring full of unconsumed garbage would
        // only add noise to it.
        ASSERT_TRUE(fe.set_vring_enable(false));
        const uint16_t PAST = FULL + 1;
        ((vavail*)(fe.mem + L_AVAIL))->idx = 5;
        fe.avail_idx = 5;
        ASSERT_TRUE(fe.restart_with_base(PAST));
        (void)fe.callfd_drain();
        EXPECT_EQ(5, fe.get_avail_event())
            << "a cursor " << VQ_NUM + 1 << " entries past used_idx was adopted on a "
            << VQ_NUM << "-entry ring; one more outstanding than the ring can hold is"
               " not a state this queue was ever in";
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

// ---------------------------------------------------------------------------
// Indirect descriptors over the wire
//
// The engine half of this is pinned in test-blk-vq.cpp against a fixture whose guest
// memory the case itself owns. What this file adds is the transport: that the bit is
// offered, that the device walks a table only once the NEGOTIATED word carries it,
// that a reconnect which declines the bit stops walking them, and that seg_max is
// published where the wire says it is.
//
// One trap the cases below are written around. Offering bit 28 makes every existing
// case in this file negotiate it, and not one of them sends a table -- they all build
// three-descriptor direct chains. So the 68 that predate this block are the regression
// guard for "offered the bit, driver still sends direct chains", which is a combination
// a real frontend produces, and nothing in them exercises the walk by accident. Every
// case below builds its indirect request explicitly.
// ---------------------------------------------------------------------------

TEST_F(VhostUserTest, indirect_desc_is_offered_and_negotiated) {
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
        // The offer is a PRECONDITION and is read before the negotiated word is
        // asserted on: a bit that was never offered cannot survive negotiation either,
        // and the second assertion below would then pass for a reason nobody intended.
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_FEATURES; m.size = 0;
        ASSERT_TRUE(fe.transact(&m, &r));
        // Copied out before a macro sees it: `r` is a packed struct.
        const uint64_t offer = r.payload.u64;
        ASSERT_TRUE(offer & F_RING_INDIRECT_DESC) << "bit 28 is not in GET_FEATURES";

        ASSERT_TRUE(fe.negotiate(false));
        ASSERT_TRUE(fe.features & F_RING_INDIRECT_DESC)
            << "bit 28 was offered but did not survive negotiation, so every case below"
               " that builds a table is exercising a refusal";
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
}

// The gate itself, and the in-repo discriminator for deriving it from the offer
// instead of from the negotiated word: this mock can DECLINE one bit, so "we offered
// it" and "the peer accepted it" are two different sessions here. The request sent is
// a legal one -- byte for byte the request M3 serves -- so what differs is only the
// feature word.
//
// The write/read pair afterwards is not padding. A refusal that tore the session down
// would satisfy every assertion above it and still be a defect.
TEST_F(VhostUserTest, an_indirect_request_is_refused_when_the_feature_was_declined) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint64_t OFF = 4ull << 20;
    auto good = pattern(0x11, 4096);
    std::vector<char> buf(4096, 0);
    int st = -1;
    uint32_t ulen = 0;
    const char* why = nullptr;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_FEATURES; m.size = 0;
        if (!fe.transact(&m, &r)) return EPROTO;
        const uint64_t offer = r.payload.u64;
        if (!(offer & F_RING_INDIRECT_DESC)) { why = "bit 28 was never offered"; return EPROTO; }
        fe.decline = F_RING_INDIRECT_DESC;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.features & F_RING_INDIRECT_DESC) { why = "bit 28 survived the decline"; return EPROTO; }

        if (fe.write_dev(OFF, good.data(), good.size()) != S_OK) return EIO;
        st = fe.do_request(T_IN, OFF >> 9, buf.data(), buf.size(), true, &ulen, 0,
                           /*indirect=*/true);
        // And the session is still usable, which is the half a bare refusal shape
        // cannot show.
        if (fe.write_dev(OFF, good.data(), good.size()) != S_OK) return EIO;
        std::vector<char> back(good.size(), 0);
        if (fe.read_dev(OFF, back.data(), back.size()) != S_OK) return EIO;
        return memcmp(good.data(), back.data(), good.size()) ? EILSEQ : 0;
    });
    ASSERT_EQ(nullptr, why) << why;
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0xff, st) << "a declined feature still walked the table";
    EXPECT_EQ(0u, ulen);
    EXPECT_EQ(0, verify_backend(OFF, good));
}

// A READ published as a table reports data plus status in the used element's length,
// exactly as the direct form does. The 4097 is written out rather than left to
// do_request's own expectation: it derives the same number from the request type, and
// a case that leaned on that would stop being a check on the wire the moment somebody
// "simplified" the helper.
TEST_F(VhostUserTest, an_indirect_read_reports_data_plus_status) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint64_t OFF = 4ull << 20;
    auto w = pattern(0x33, 4096);
    std::vector<char> buf(4096, 0);
    int st = -1;
    uint32_t ulen = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!(fe.features & F_RING_INDIRECT_DESC)) return EPROTO;
        // Seeded through the direct path, so what the indirect read brings back is
        // compared against bytes that got there a different way.
        if (fe.write_dev(OFF, w.data(), w.size()) != S_OK) return EIO;
        st = fe.do_request(T_IN, OFF >> 9, buf.data(), buf.size(), true, &ulen, 0, true);
        return st < 0 ? EIO : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(S_OK, st);
    EXPECT_EQ(4097u, ulen);
    EXPECT_EQ(0, memcmp(w.data(), buf.data(), w.size()));
}

// The WRITE half. Its evidence that no table byte reached the backend is read from the
// image itself rather than from the used length: verify_backend bypasses the frontend
// entirely, which is the transport-side counterpart of the engine case that asserts
// the table descriptor carries no data.
TEST_F(VhostUserTest, an_indirect_write_reports_only_the_status_byte) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint64_t OFF = 4ull << 20;
    auto w = pattern(0x44, 4096);
    int st = -1;
    uint32_t ulen = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!(fe.features & F_RING_INDIRECT_DESC)) return EPROTO;
        st = fe.do_request(T_OUT, OFF >> 9, w.data(), w.size(), false, &ulen, 0, true);
        return st < 0 ? EIO : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(S_OK, st);
    EXPECT_EQ(1u, ulen);
    EXPECT_EQ(0, verify_backend(OFF, w));
}

// D1 from the transport side, and the same argument as the engine case: an indirect
// request is one head, so it is one in_flight, so a configured queue_depth caps it the
// same way. Every expectation is copied from configured_queue_depth_caps_in_flight and
// only the request's LAYOUT differs -- which is the discriminator, since per-entry
// accounting would hit the cap during the first dispatch and `seen` would not read 2.
TEST_F(VhostUserTest, configured_queue_depth_caps_indirect_requests_the_same_way) {
    constexpr uint64_t DEPTH = 2;
    constexpr int N = 8;   // far under VQ_NUM, so only the depth can hold this at 2
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    cfg.queue_depth = (uint32_t) DEPTH;
    test::RecordingFile rf(file);
    rf.gated = true;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(&rf));
    DEFER(dev->shutdown());
    DEFER(rf.release_gate(1024));

    uint64_t seen = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!(fe.features & F_RING_INDIRECT_DESC)) return EPROTO;
        constexpr uint16_t SLOT = SLOTS - 1;
        // ONE ring descriptor naming a three-entry table, in place of the three ring
        // descriptors the direct-chain case writes. Everything after this is verbatim.
        auto* desc = (vdesc*)(fe.mem + L_DESC);
        auto* t = (vdesc*)(fe.mem + MockFrontend::tbl_off(SLOT));
        t[0] = vdesc{MockFrontend::hdr_off(SLOT), sizeof(blk_outhdr), DESC_F_NEXT, 1};
        t[1] = vdesc{MockFrontend::data_off(SLOT), 512,
                     (uint16_t)(DESC_F_WRITE | DESC_F_NEXT), 2};
        t[2] = vdesc{MockFrontend::status_off(SLOT), 1, DESC_F_WRITE, 0};
        desc[0] = vdesc{MockFrontend::tbl_off(SLOT), 3 * (uint32_t)sizeof(vdesc),
                        DESC_F_INDIRECT, 0};
        *(blk_outhdr*)(fe.mem + MockFrontend::hdr_off(SLOT)) = blk_outhdr{T_IN, 0, 0};
        *(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT)) = 0xff;

        uint16_t u0 = fe.used_idx_now();
        for (int i = 0; i < N; i++)
            fe.publish(0);
        if (!fe.kick()) return EIO;   // ONE kick for all N

        for (int i = 0; i < 2000 && rf.arrivals.load() < DEPTH; i++)
            ::usleep(1000);
        // The gate is shut, so nothing completes and nothing frees a slot: the count
        // can only rise here, and waiting can never turn a device that over-admitted
        // into one that looks like it did not.
        ::usleep(50 * 1000);
        seen = rf.arrivals.load();
        rf.release_gate(1024);
        if (!fe.wait_used_advance(u0, (uint16_t) N, 20000)) return ETIMEDOUT;
        return *(uint8_t*)(fe.mem + MockFrontend::status_off(SLOT)) == S_OK ? 0 : EIO;
    });
    EXPECT_EQ(0, rc);
    EXPECT_EQ(DEPTH, seen);
    EXPECT_EQ((uint64_t) N, rf.arrivals.load());
}

// A session that declines bit 28 does not inherit the previous one's, and the reason is
// SET_FEATURES: it re-derives the flag from the word it carries. A frontend RECONNECT
// never reaches vq_reset(), because msg_loop tears a disconnected session down with
// vq_stop and leaves the per-queue state standing for the next frontend. The reset path
// is what the case below pins: its second session sends no SET_FEATURES at all, so it
// has nothing to re-derive the flag from.
//
// Session 1 has to SUCCEED. A case whose first half also failed would be comparing two
// refusals and would pass with the clear deleted.
TEST_F(VhostUserTest, a_reconnect_that_declines_the_feature_stops_walking_tables) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint64_t OFF = 4ull << 20;
    auto w = pattern(0x55, 4096);
    int st1 = -1, st2 = -1;
    uint32_t ulen2 = 0;

    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!(fe.features & F_RING_INDIRECT_DESC)) return EPROTO;
        st1 = fe.do_request(T_OUT, OFF >> 9, w.data(), w.size(), false, nullptr, 0, true);
        return st1 == S_OK ? 0 : EIO;
    });
    ASSERT_EQ(0, rc) << "session 1 did not serve the table, so session 2 proves nothing";
    EXPECT_EQ(S_OK, st1);

    rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        fe.decline = F_RING_INDIRECT_DESC;
        if (!fe.negotiate(false)) return EPROTO;
        if (fe.features & F_RING_INDIRECT_DESC) return EPROTO;
        st2 = fe.do_request(T_OUT, OFF >> 9, w.data(), w.size(), false, &ulen2, 0, true);
        return 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0xff, st2) << "a session that declined bit 28 inherited the previous one's";
    EXPECT_EQ(0u, ulen2);
}

// The reset path the case above cannot OBSERVE, and the only case here that can tell
// whether vq_reset()'s clear of indirect_desc ran. Plenty of cases EXECUTE that clear --
// every shutdown() reaches it through detach() -- but they all send SET_FEATURES in their
// next session, which overwrites whatever it left behind, so none of them can see it.
// Per-queue state crosses a detach()/start() boundary because the Vq objects do -- built
// in the constructor, freed only in the destructor, and that is deliberate: a device has
// to stay able to start again and those are the slots it starts from. detach() is blk.h's
// stop-serving-and-start-again call and keeps them too; what it does empty, through
// stop_session()'s vq_reset(), is the slots' ring and negotiated state -- num, the three
// ring addresses, both cursors, both eventfds, and every feature-derived flag.
// SET_FEATURES is the only message that puts bit 28 back, so a session that never sends
// one runs on whatever the reset left behind: nothing, in a device that clears, and the
// previous session's true in one that does not -- the offer-instead-of-negotiated
// mistake, arriving through the reset path.
//
// Nothing else on the way to a dispatch asks whether SET_FEATURES arrived: the ring goes
// live on SET_VRING_ENABLE alone, so such a session is served rather than refused, and
// that is what makes an inherited flag reachable at all.
//
// Session 1 has to SUCCEED, for the reason the case above gives, and its proof is read
// out of the image rather than out of the frontend: those bytes got there through the
// TABLE's data descriptor.
TEST_F(VhostUserTest, a_session_that_never_sends_set_features_does_not_inherit_the_last_one_s_indirect_desc) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));

    constexpr uint64_t OFF  = 4ull << 20;
    constexpr uint64_t OFF2 = 5ull << 20;
    auto w = pattern(0x66, 4096);
    auto w2 = pattern(0x77, 4096);
    std::vector<char> buf(4096, 0);
    int st1 = -1, st2 = -1;
    uint32_t ulen2 = 0;

    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!(fe.features & F_RING_INDIRECT_DESC)) return EPROTO;
        // No used_len out-param on this one: do_request already refuses a successful
        // write whose used length is not the single status byte, so rc == 0 IS that
        // check, and an EXPECT_EQ on the value here could only ever read back what the
        // call it follows requires.
        st1 = fe.do_request(T_OUT, OFF >> 9, w.data(), w.size(), false, nullptr, 0,
                            /*indirect=*/true);
        return st1 == S_OK ? 0 : EIO;
    });
    ASSERT_EQ(0, rc) << "session 1 did not walk the table, so session 2 proves nothing";
    EXPECT_EQ(S_OK, st1);
    EXPECT_EQ(0, verify_backend(OFF, w));

    // detach() is what runs stop_session() and so vq_reset(). It also leaves the socket
    // node behind, which shutdown() does not, and bind() refuses whatever already sits at
    // the path -- a dead listener included -- so the node has to go before the next
    // start(). destroy_orphan() removes a dead one and refuses a live one, so its
    // returning 0 is also the witness that the detach really stopped serving.
    ASSERT_EQ(0, dev->detach(true));
    BlkDevInfo orphan;
    orphan.identity = SOCK_PATH;
    ASSERT_EQ(0, ctl->destroy_orphan(orphan));
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        fe.skip_set_features = true;
        if (!fe.negotiate(false)) return EPROTO;
        // A precondition on the mock, in the shape the declined case above uses: this
        // session settled no feature word at all, so nothing below can be read as an
        // agreement to walk a table. The ring is up all the same, so an inherited flag
        // would be reachable rather than merely present.
        if (fe.features != 0) return EPROTO;
        st2 = fe.do_request(T_IN, OFF >> 9, buf.data(), buf.size(), true, &ulen2, 0,
                            /*indirect=*/true);
        // And the session is still usable, which is the half a bare refusal shape cannot
        // show.
        if (fe.write_dev(OFF2, w2.data(), w2.size()) != S_OK) return EIO;
        std::vector<char> back(w2.size(), 0);
        if (fe.read_dev(OFF2, back.data(), back.size()) != S_OK) return EIO;
        return memcmp(w2.data(), back.data(), w2.size()) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0xff, st2) << "a session that never sent SET_FEATURES inherited the last one's bit 28";
    EXPECT_EQ(0u, ulen2);
    // The refusal reaches the data path too, and this reads it out of the buffer rather
    // than out of the status byte: a device that walked the table would have filled this
    // from the image, with the pattern session 1 put there through a table of its own.
    const std::vector<char> zeroes(buf.size(), 0);
    EXPECT_EQ(0, memcmp(zeroes.data(), buf.data(), buf.size()))
        << "the refused read still filled the buffer it was offered";
}

// The table's address goes through the same containment predicate every other buffer
// does, and an address whose length wraps past the top of the space is refused by it.
// No mutant of its own, and that is honest bookkeeping: the predicate is pre-existing
// and already pinned. What this adds is the CALL SITE, and a predicate's coverage is
// counted per call site rather than per line of code -- a table translated through a
// private path would pass every existing case.
TEST_F(VhostUserTest, a_wrapping_table_address_is_refused) {
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

    constexpr uint16_t SLOT = 3;
    int st = -1;
    uint32_t ulen = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!(fe.features & F_RING_INDIRECT_DESC)) return EPROTO;

        uint16_t head = fe.alloc_head_indirect();
        blk_outhdr hdr{T_IN, 0, 0};
        vdesc chain[1] = {{WRAP_ADDR, WRAP_LEN, DESC_F_INDIRECT, 0}};
        st = fe.do_raw(head, SLOT, hdr, chain, 1, &ulen);
        // The session survives its own refusal, which is the half the shape above
        // cannot show on its own.
        auto w = pattern(0x66, 512);
        if (fe.write_dev(1 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> back(w.size(), 0);
        if (fe.read_dev(1 << 20, back.data(), back.size()) != S_OK) return EIO;
        return memcmp(w.data(), back.data(), w.size()) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0xff, st);
    EXPECT_EQ(0u, ulen);
}

// An over-cap table is refused and the session goes on serving. The entries are
// CHAINED, not zeroed: flags == 0 means no NEXT, so a region of zeroes is a one-entry
// table and both a capped and an uncapped engine pay exactly one translate for it.
//
// The kick is unconditional because do_raw's is, and that is the right choice here
// rather than an oversight: this case sends a deliberately malformed table, so the
// backend is busy refusing it, and a conditional kick would make the verdict depend on
// an avail_event published by a device in exactly that window. §2.7.10.2 lets a device
// tolerate a spurious notification, which is what this relies on.
//
// The entry-count half of the DoS argument is NOT observable here and this case does
// not claim it: one translate on this transport is at most a handful of integer
// comparisons against the mapped regions, so even a quarter of a million of them costs
// milliseconds and collect()'s budget never expires. The count is pinned by the engine
// case that asserts zero translates for an over-cap table. What is left here is the
// refusal shape and the session surviving it.
TEST_F(VhostUserTest, an_absurd_table_is_refused_and_the_session_survives) {
    VhostUserController::Config cfg(make_info());
    cfg.sock_path = SOCK_PATH;
    auto dev = ctl->new_device(cfg);
    ASSERT_NE(nullptr, dev);
    DEFER(delete dev);
    ASSERT_EQ(0, dev->start(file));
    DEFER(dev->shutdown());

    constexpr uint16_t SLOT = 4;
    int st = -1;
    uint32_t ulen = 0;
    int rc = run_frontend([&](MockFrontend& fe) -> int {
        if (!fe.connect_to(SOCK_PATH)) return ECONNREFUSED;
        if (!fe.negotiate(false)) return EPROTO;
        if (!(fe.features & F_RING_INDIRECT_DESC)) return EPROTO;

        auto* t = (vdesc*)(fe.mem + L_TBL_BIG);
        for (uint32_t i = 0; i < TBL_BIG_ENTRIES; i++) {
            t[i].addr = 0;
            t[i].len = 0;
            t[i].flags = (i + 1 < TBL_BIG_ENTRIES) ? DESC_F_NEXT : 0;
            t[i].next = (uint16_t)(i + 1);
        }
        uint16_t head = fe.alloc_head_indirect();
        blk_outhdr hdr{T_IN, 0, 0};
        vdesc chain[1] = {{L_TBL_BIG, TBL_BIG_ENTRIES * (uint32_t)sizeof(vdesc),
                           DESC_F_INDIRECT, 0}};
        st = fe.do_raw(head, SLOT, hdr, chain, 1, &ulen);
        auto w = pattern(0x77, 512);
        if (fe.write_dev(1 << 20, w.data(), w.size()) != S_OK) return EIO;
        std::vector<char> back(w.size(), 0);
        if (fe.read_dev(1 << 20, back.data(), back.size()) != S_OK) return EIO;
        return memcmp(w.data(), back.data(), w.size()) ? EILSEQ : 0;
    });
    ASSERT_EQ(0, rc);
    EXPECT_EQ(0xff, st);
    EXPECT_EQ(0u, ulen);
}

// seg_max over the wire, which pins the value AND the offset it sits at: both are read
// out of a GET_CONFIG reply rather than out of this process's idea of the struct, so a
// 62 written one field early reads back as 0 here and as 62 in the size_max assertion
// below, and the pair goes red together.
//
// That matters because the seven static_asserts over the config struct pin capacity,
// blk_size and num_queues by offset and are required to stay exactly as they are, so
// seg_max's offset is otherwise only implied by packed-ness and its neighbours. This is
// the one way to pin it without touching that copy.
//
// What it cannot prove, stated plainly: that 62 is the RIGHT number. This asserts we
// published the cap minus the two framing descriptors. That the driver reads the field
// the same way is argued from the spec and from a kernel source read recorded in the
// design document, not from anything in this repository.
TEST_F(VhostUserTest, seg_max_is_published_as_the_cap_minus_the_two_framing_descriptors) {
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
        vhost_user_msg m, r;
        memset(&m, 0, sizeof(m));
        m.request = VHOST_USER_GET_FEATURES; m.size = 0;
        ASSERT_TRUE(fe.transact(&m, &r));
        const uint64_t offer = r.payload.u64;
        ASSERT_TRUE(offer & F_BLK_SEG_MAX)
            << "bit 2 is not offered, so a seg_max in the config space means nothing";
        ASSERT_TRUE(fe.negotiate(false));
        // 64 entries minus the header and status descriptors a driver frames its data
        // segments with. Spelled out, not read out of the code under test.
        EXPECT_EQ(62u, fe.config_seg_max());
        // VIRTIO_BLK_F_SIZE_MAX (bit 1) is not offered, so its field stays zero -- and
        // a seg_max written one field early would show up here instead of nowhere.
        EXPECT_FALSE(offer & (1ULL << 1)) << "bit 1 is offered but nothing fills size_max";
        EXPECT_EQ(0u, fe.config_size_max());
    });
    if (!fe.err.empty())
        LOG_ERROR("mock frontend: `", fe.err);
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
