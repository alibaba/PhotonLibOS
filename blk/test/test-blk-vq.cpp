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

// The shared virtio execution engine, tested directly: a VirtQueueServer driving a
// vring that lives in this process's memory, with no transport, no frontend and no
// kernel. Both virtio transports are thin control planes over this engine, so a
// defect in its request parsing or its notification decisions is a defect in both
// of them -- and reaching it through a transport means the transport's own mock
// gets to decide what a legal request looks like. That is how a wrong assumption
// shared by the engine and its mock survives every test: neither side can see it.
// Here the ring is built by hand from the wire layout, so a request shape no
// transport mock produces is still expressible.
//
// The second OS thread is not decoration. Notification loss is a store/load race
// between two parties, and a single-threaded test of should_notify() can only ever
// check the value it reads, never the ordering that decides whether the reader on
// the other side sees it in time.

#include "../utils.h"
#include "../../test/gtest.h"
#include "harness.h"        // only for main()'s consumer-child dispatch

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/thread/thread.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace photon;
using namespace photon::blk;

namespace {

constexpr uint32_t RING_NUM = 8;

// The split-ring layout, in ordinary memory: desc[num], then the avail ring with
// its trailing used_event, then the used ring with its trailing avail_event. The
// two trailing slots are what vring_used_event()/vring_avail_event() read, so they
// have to be part of the allocation even for a test that never negotiates
// EVENT_IDX -- reading them is unconditional in one of should_notify()'s branches.
// Each region starts on an 8-byte boundary; the real layout pads to a page for DMA,
// which matters to a device, not to a struct's alignment. Its fields are the
// uapi's plain integers, not std::atomic, so the accesses to them below go
// through the __atomic builtins -- which take __ATOMIC_*, not std::memory_order:
// that stopped converting to int when C++20 made it a scoped enum.
struct MemVring {
    std::vector<char> mem;
    vring_desc* desc = nullptr;
    vring_avail* avail = nullptr;
    vring_used* used = nullptr;

    explicit MemVring(uint32_t num = RING_NUM) {
        size_t desc_sz = sizeof(vring_desc) * num;
        // flags, idx, ring[num], used_event
        size_t avail_sz = sizeof(uint16_t) * (3 + num);
        // flags, idx, ring[num], avail_event
        size_t used_sz = sizeof(uint16_t) * 3 + sizeof(vring_used_elem) * num;
        size_t total = desc_sz + avail_sz + used_sz + 3 * 8;
        mem.assign(total, 0);
        char* p = mem.data();
        desc = (vring_desc*)p;
        p += desc_sz;
        avail = (vring_avail*)p;
        p += avail_sz;
        p = (char*)(((uintptr_t)p + 7) & ~(uintptr_t)7);
        used = (vring_used*)p;
    }
};

// A two-party barrier. std::thread and atomics rather than pthread_barrier_t,
// which is not available on every platform these suites build on.
struct Spin2 {
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> gen{0};

    void wait() {
        uint32_t g = gen.load(std::memory_order_relaxed);
        if (count.fetch_add(1, std::memory_order_acq_rel) == 1) {
            count.store(0, std::memory_order_relaxed);
            gen.fetch_add(1, std::memory_order_release);
        } else {
            while (gen.load(std::memory_order_acquire) == g)
                ;
        }
    }
};

std::atomic<uint64_t> g_notifications{0};

void notify_thunk(void*) {
    g_notifications.fetch_add(1, std::memory_order_relaxed);
}

bool ready_true(void*) {
    return true;
}

}   // namespace

// A completion published while the driver is in the act of enabling notifications.
//
// The driver's side of this is prescribed: write the flag, barrier, re-read the
// used index, and only then decide to sleep. The device's side is ours, and it has
// to be the mirror image -- publish the used element and its index, barrier, then
// read the driver's flag. Drop the device's barrier and the two stores can pass
// each other: the device reads the flag as still suppressed while the driver reads
// the used index as still unchanged. Neither side notifies, and the completed
// request is not reported until some later completion happens to interrupt.
//
// This is the flags mode of the ring, which is what a driver that did not
// negotiate EVENT_IDX gets, and what should_notify() takes through its early
// return. It is asserted as exactly zero misses because zero is what the ordering
// guarantees -- not as a rate below a threshold, which would pass on a build that
// had lost the fence and simply got lucky.
TEST(VqNotify, flags_mode_does_not_lose_a_completion_to_the_driver_enable_race) {
    MemVring r;
    VirtQueueServer srv;
    srv.desc = r.desc;
    srv.avail = r.avail;
    srv.used = r.used;
    srv.num = RING_NUM;
    srv.event_idx.store(false, std::memory_order_relaxed);
    // Past the first decision on this ring, so should_notify()'s unconditional
    // first-notification rule cannot mask the race by answering true every time.
    srv.notify_valid.store(true, std::memory_order_relaxed);
    srv.hooks.notify.bind(nullptr, &notify_thunk);
    srv.hooks.ready.bind(nullptr, &ready_true);

    constexpr int RACES = 300000;
    Spin2 bar;
    uint64_t misses = 0, device_notified_count = 0, driver_saw_count = 0;

    std::thread device([&] {
        for (int i = 0; i < RACES; i++) {
            bar.wait();
            srv.complete_req(0, 0);      // the real path: append, then decide
            bar.wait();
        }
    });

    for (int i = 0; i < RACES; i++) {
        const uint16_t base = (uint16_t)i;
        // Reset while the device is parked at the barrier, so the two stores that
        // race are only its used-index and this thread's flag.
        __atomic_store_n(&r.avail->flags, (uint16_t)VRING_AVAIL_F_NO_INTERRUPT,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&r.used->idx, base, __ATOMIC_RELAXED);
        srv.used_idx = base;
        uint64_t before = g_notifications.load(std::memory_order_relaxed);

        bar.wait();
        // The driver's prescribed sequence: enable, barrier, re-read.
        __atomic_store_n(&r.avail->flags, (uint16_t)0, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        bool driver_saw_it = __atomic_load_n(&r.used->idx, __ATOMIC_ACQUIRE) != base;
        bar.wait();

        bool device_notified = g_notifications.load(std::memory_order_relaxed) != before;
        if (device_notified)
            device_notified_count++;
        if (driver_saw_it)
            driver_saw_count++;
        if (!device_notified && !driver_saw_it)
            misses++;
    }
    device.join();

    // Both halves of "somebody noticed" have to fail for a miss. Asserted as
    // exactly zero because zero is what the ordering guarantees -- not as a rate
    // below a threshold, which would also pass on a build that had lost the fence
    // and merely got lucky.
    EXPECT_EQ(0u, misses) << "completed requests that nobody reported";
    // The one non-vacuity control that holds on every platform: the loop ran and
    // the notify hook fires.
    EXPECT_GT(device_notified_count, 0u);
    EXPECT_LE(device_notified_count, (uint64_t)RACES);
    // NOT asserted, and the omission is deliberate: how often the driver's re-read
    // observes the completion. On aarch64 it observes it zero times in 300000
    // rounds, because this thread's seq_cst fence costs a dmb while the device
    // side reaches its load through three cross-DSO calls, so the driver
    // systematically arrives after the device has published. Asserting a nonzero
    // count here produced a red run on a correct build.
    //
    // Which means the zero above cannot catch a LOST FENCE on this architecture:
    // the driver always wins, the device always sees the cleared flag, and a build
    // with the fence removed passes too -- measured, not assumed. Against that
    // mutant the case earns its place on x86, where store-buffer drain timing lets
    // both sides miss each other and the same harness counted 449 lost
    // notifications in 300000 races. It is not unfalsifiable here in general:
    // inverting the flags-mode answer below turns it red with 299996 misses out of
    // 300000. Read it as coverage whose reach depends on the runner, never as
    // proof of the fence.
    (void)driver_saw_count;
}

// The half of the notification decision that IS deterministic, and therefore
// testable anywhere: in flags mode the answer is exactly "the driver has not
// suppressed interrupts". The fence above orders this read against the driver's
// write; it does not change what the read decides.
TEST(VqNotify, flags_mode_answers_from_the_driver_suppression_bit) {
    MemVring r;
    VirtQueueServer srv;
    srv.desc = r.desc;
    srv.avail = r.avail;
    srv.used = r.used;
    srv.num = RING_NUM;
    srv.event_idx.store(false, std::memory_order_relaxed);
    srv.notify_valid.store(true, std::memory_order_relaxed);
    srv.hooks.notify.bind(nullptr, &notify_thunk);
    srv.hooks.ready.bind(nullptr, &ready_true);

    __atomic_store_n(&r.avail->flags, (uint16_t)VRING_AVAIL_F_NO_INTERRUPT,
                     __ATOMIC_RELAXED);
    EXPECT_FALSE(srv.should_notify(0));
    __atomic_store_n(&r.avail->flags, (uint16_t)0, __ATOMIC_RELAXED);
    EXPECT_TRUE(srv.should_notify(0));
}

// EVENT_IDX mode: the first decision on a ring notifies whatever the used_event
// arithmetic says, because a resumed ring's used_idx and the used_event the
// previous daemon left behind need not satisfy the equality, and without this the
// first completion after adoption would never be reported. The second decision
// follows the arithmetic.
TEST(VqNotify, the_first_decision_on_a_ring_notifies_and_the_second_follows_the_arithmetic) {
    MemVring r;
    VirtQueueServer srv;
    srv.desc = r.desc;
    srv.avail = r.avail;
    srv.used = r.used;
    srv.num = RING_NUM;
    srv.event_idx.store(true, std::memory_order_relaxed);
    srv.notify_valid.store(false, std::memory_order_relaxed);
    srv.hooks.notify.bind(nullptr, &notify_thunk);
    srv.hooks.ready.bind(nullptr, &ready_true);

    // used_event lives in the slot just past the avail ring. With it at 0 and both
    // the new and the old used index at 5, need_event is (5-0-1) < (5-5), i.e.
    // false -- so a true answer here can only have come from the first-decision
    // rule, not from the arithmetic agreeing by accident.
    __atomic_store_n(&r.avail->ring[RING_NUM], (uint16_t)0, __ATOMIC_RELAXED);
    srv.used_idx = 5;
    EXPECT_TRUE(srv.should_notify(5));
    EXPECT_TRUE(srv.notify_valid.load(std::memory_order_relaxed));
    // Same inputs, rule now spent: the arithmetic decides, and it says no.
    EXPECT_FALSE(srv.should_notify(5));
}

// ---------------------------------------------------------------------------
// Request parsing
//
// A descriptor chain carries two byte streams, not one role per descriptor:
// everything device-readable is the header followed by a WRITE's payload, and
// everything device-writable is a READ's destination followed by the one-byte
// status. virtio 1.2 does not require a driver to start a new descriptor at
// those boundaries, so a parser that assigns roles to whole descriptors drops
// whatever shares one -- and a WRITE whose payload shared the header's
// descriptor was answered VIRTIO_BLK_S_OK having written nothing.
//
// These cases build the chain by hand and serve it against a real backend image,
// so the oracle is the bytes that reached the backend rather than a mock's
// opinion of what a legal request looks like. Reaching the engine through a
// transport means the transport's own frontend mock picks the shapes, and a
// wrong assumption shared by the engine and its mock survives every such test.
// ---------------------------------------------------------------------------

constexpr uint64_t CHAIN_CAPACITY = 1 << 20;
constexpr uint64_t CHAIN_SECTOR = 4;
constexpr uint8_t SENTINEL = 0xcc;
// shorter than VIRTIO_BLK_ID_BYTES on purpose, so a GET_ID has padding to write
constexpr const char* CHAIN_SERIAL = "photon-vq-test";

// A flat "guest" memory plus a descriptor table pointing into it. The translate
// hook below turns a descriptor address into a local pointer, which is what a
// transport's mapping does in production.
struct GuestChain {
    std::vector<char> mem;
    std::vector<vring_desc> desc;
    uint16_t ndesc = 0;
    size_t bump = 0;

    GuestChain() : mem(64 * 1024, 0), desc(RING_NUM) {}

    // reserve `len` bytes of guest memory, optionally filled; returns the
    // address a descriptor would carry
    size_t place(const void* src, size_t len) {
        size_t off = bump;
        bump += (len + 7) & ~(size_t) 7;
        EXPECT_LE(bump, mem.size());
        if (src && len)
            memcpy(mem.data() + off, src, len);
        return off;
    }
    void add(size_t off, uint32_t len, uint16_t flags) {
        EXPECT_LT((size_t) ndesc, desc.size());
        desc[ndesc].addr = off;
        desc[ndesc].len = len;
        desc[ndesc].flags = flags | VRING_DESC_F_NEXT;
        desc[ndesc].next = ndesc + 1;
        ndesc++;
    }
    // add() marks every descriptor as having a successor; the last one does not
    void finish() { desc[ndesc - 1].flags &= ~VRING_DESC_F_NEXT; }
    uint8_t* at(size_t off) { return (uint8_t*) mem.data() + off; }
};

void* chain_translate(void* a, uint64_t addr, size_t len) {
    auto* c = (GuestChain*) a;
    // wrap-safe, the spelling the engine's own bounds checks use: an address
    // near the top of the space must not be admitted by an overflowing sum
    if (addr > c->mem.size() || len > c->mem.size() - addr)
        return nullptr;
    return c->mem.data() + addr;
}

// non-repeating, so a misplaced or partial write shows up in the bytes and not
// only in a count
void fill_pattern(uint8_t* p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t) (seed + i * 7 + (i >> 3));
}

class ChainFixture : public ::testing::Test {
public:
    test::TestImage img;
    std::vector<uint8_t> pattern;

    void SetUp() override {
        ASSERT_EQ(0, img.create("/tmp/photon-blk-vq-chain.img", CHAIN_CAPACITY));
        pattern.resize(512);
        fill_pattern(pattern.data(), pattern.size(), 0x11);
    }

    uint8_t serve(GuestChain& c, bool read_only, uint32_t* written) {
        VirtioBlkTranslate tr;
        tr.bind(&c, &chain_translate);
        return virtio_blk_serve_chain(img.file, read_only, CHAIN_SERIAL, "vq",
                                      c.desc.data(), 0, RING_NUM, CHAIN_CAPACITY,
                                      tr, written);
    }
    ssize_t read_back(void* dst, size_t n) {
        return img.file->pread(dst, n, (off_t) (CHAIN_SECTOR << 9));
    }
    // a header for a request that carries `sector`, placed and described
    size_t put_header(GuestChain& c, uint32_t type) {
        virtio_blk_outhdr hdr{};
        hdr.type = type;
        hdr.sector = CHAIN_SECTOR;
        size_t off = c.place(&hdr, sizeof(hdr));
        c.add(off, sizeof(hdr), 0);
        return off;
    }
    size_t put_status(GuestChain& c) {
        size_t off = c.place(nullptr, 1);
        *c.at(off) = SENTINEL;
        c.add(off, 1, VRING_DESC_F_WRITE);
        return off;
    }
};

// The shape the reviewer measured: header and WRITE payload in ONE
// device-readable descriptor, status in its own. A whole-descriptor parser
// takes the header and discards the 512 bytes behind it, so the payload list is
// empty, the request writes nothing, and it is still answered OK.
TEST_F(ChainFixture, a_write_whose_payload_shares_the_header_descriptor_reaches_the_backend) {
    GuestChain c;
    virtio_blk_outhdr hdr{};
    hdr.type = VIRTIO_BLK_T_OUT;
    hdr.sector = CHAIN_SECTOR;
    size_t off = c.place(nullptr, sizeof(hdr) + pattern.size());
    memcpy(c.at(off), &hdr, sizeof(hdr));
    memcpy(c.at(off) + sizeof(hdr), pattern.data(), pattern.size());
    c.add(off, sizeof(hdr) + pattern.size(), 0);
    size_t st = put_status(c);
    c.finish();

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    EXPECT_EQ(1u, written);      // the guest gets the status byte back, no data

    uint8_t back[512] = {};
    ASSERT_EQ((ssize_t) sizeof(back), read_back(back, sizeof(back)));
    EXPECT_EQ(0, memcmp(pattern.data(), back, sizeof(back)));
}

// The mirror image: a READ whose destination and status share ONE
// device-writable descriptor. The status is the last byte of the writable
// stream, not necessarily a descriptor of its own, so a parser that only
// recognises a 1-byte tail descriptor leaves the byte the driver polls at
// whatever the driver put there.
TEST_F(ChainFixture, a_read_whose_payload_shares_the_status_descriptor_gets_its_status_byte) {
    ASSERT_EQ((ssize_t) pattern.size(),
              img.file->pwrite(pattern.data(), pattern.size(),
                               (off_t) (CHAIN_SECTOR << 9)));
    // The backend byte behind the payload is a third distinct value, because
    // VIRTIO_BLK_S_OK is zero: a parser that reads one byte too far would leave
    // a zero in the guest's status slot, and a zero left by an over-read is
    // indistinguishable from a status that was actually written.
    const uint8_t behind = 0xee;
    ASSERT_EQ(1, img.file->pwrite(&behind, 1,
                                  (off_t) ((CHAIN_SECTOR << 9) + pattern.size())));

    GuestChain c;
    put_header(c, VIRTIO_BLK_T_IN);
    const size_t total = pattern.size() + 1;
    size_t woff = c.place(nullptr, total);
    memset(c.at(woff), SENTINEL, total);
    c.add(woff, total, VRING_DESC_F_WRITE);
    c.finish();

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written));
    EXPECT_EQ((uint32_t) total, written);
    EXPECT_EQ(0, memcmp(pattern.data(), c.at(woff), pattern.size()));
    EXPECT_EQ(VIRTIO_BLK_S_OK, c.at(woff)[pattern.size()]);
}

// Nothing requires the header to sit inside one descriptor either. A parser
// that demands a whole header from the first readable descriptor answers IOERR
// to a request it could have served.
TEST_F(ChainFixture, a_header_split_across_two_readable_descriptors_is_still_served) {
    GuestChain c;
    virtio_blk_outhdr hdr{};
    hdr.type = VIRTIO_BLK_T_OUT;
    hdr.sector = CHAIN_SECTOR;
    const uint8_t* hp = (const uint8_t*) &hdr;
    size_t h1 = c.place(hp, 8);
    c.add(h1, 8, 0);
    size_t h2 = c.place(hp + 8, sizeof(hdr) - 8);
    c.add(h2, sizeof(hdr) - 8, 0);
    size_t doff = c.place(pattern.data(), pattern.size());
    c.add(doff, pattern.size(), 0);
    size_t st = put_status(c);
    c.finish();

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));

    uint8_t back[512] = {};
    ASSERT_EQ((ssize_t) sizeof(back), read_back(back, sizeof(back)));
    EXPECT_EQ(0, memcmp(pattern.data(), back, sizeof(back)));
}

// A WRITE's payload is device-readable. A writable data buffer on a write
// request has no defined meaning, and serving it would pwritev() from memory
// the guest never filled in, so it is refused rather than guessed at.
TEST_F(ChainFixture, a_write_request_carrying_a_writable_data_buffer_is_refused) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_OUT);
    size_t doff = c.place(pattern.data(), pattern.size());
    c.add(doff, pattern.size(), 0);
    size_t bogus = c.place(nullptr, 512);
    c.add(bogus, 512, VRING_DESC_F_WRITE);
    size_t st = put_status(c);
    c.finish();

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &written));
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, *c.at(st));

    // refused means the backend was left alone
    uint8_t back[512] = {};
    ASSERT_EQ((ssize_t) sizeof(back), read_back(back, sizeof(back)));
    EXPECT_NE(0, memcmp(pattern.data(), back, sizeof(back)));
}

// The LBA and the backend offset are both in 512-byte units, so a payload that
// is not a whole number of sectors names a range the engine cannot express.
// Serving it would write past the sector the guest named.
TEST_F(ChainFixture, a_payload_that_is_not_a_whole_number_of_sectors_is_refused) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_OUT);
    size_t doff = c.place(pattern.data(), 100);
    c.add(doff, 100, 0);
    size_t st = put_status(c);
    c.finish();

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &written));
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, *c.at(st));

    // refused means the backend was left alone -- and "left alone" here has to
    // be spelled as all-zero rather than as "differs from the pattern", because
    // the buggy parser writes only the first 100 of the 512 bytes read back and
    // so differs from the pattern too
    uint8_t back[512] = {};
    ASSERT_EQ((ssize_t) sizeof(back), read_back(back, sizeof(back)));
    const uint8_t zeros[512] = {};
    EXPECT_EQ(0, memcmp(zeros, back, sizeof(back)));
}

// The header is gather-copied out of the readable stream, so the bound that
// refuses a stream too short to hold one is a length test on the STREAM, not on
// the first descriptor: half a header is refused even though the descriptor
// that carries it is perfectly valid.
TEST_F(ChainFixture, a_chain_too_short_to_hold_a_header_is_refused) {
    GuestChain c;
    virtio_blk_outhdr hdr{};
    hdr.type = VIRTIO_BLK_T_FLUSH;
    size_t off = c.place(&hdr, sizeof(hdr) / 2);
    c.add(off, sizeof(hdr) / 2, 0);
    size_t st = put_status(c);
    c.finish();

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &written));
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, *c.at(st));
    EXPECT_EQ(1u, written);   // refused, but the guest still gets its status byte
}

// VIRTIO_BLK_T_GET_ID fills a fixed-width field, so the device writes the whole
// buffer the guest offered -- NUL-padded past the end of the serial -- and the
// used length covers it. Writing only strlen(serial) leaves the rest of the
// guest's buffer holding whatever the guest prefilled there, and reports a used
// length that does not reach the end of the field the guest asked about.
TEST_F(ChainFixture, get_id_fills_the_whole_fixed_width_field_with_nul_padding) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_GET_ID);
    size_t ioff = c.place(nullptr, VIRTIO_BLK_ID_BYTES);
    memset(c.at(ioff), SENTINEL, VIRTIO_BLK_ID_BYTES);
    c.add(ioff, VIRTIO_BLK_ID_BYTES, VRING_DESC_F_WRITE);
    size_t st = put_status(c);
    c.finish();

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    // the whole field, plus the status byte the guest also gets back
    EXPECT_EQ((uint32_t) VIRTIO_BLK_ID_BYTES + 1, written);

    const size_t slen = strlen(CHAIN_SERIAL);
    ASSERT_LT(slen, (size_t) VIRTIO_BLK_ID_BYTES);
    EXPECT_EQ(0, memcmp(CHAIN_SERIAL, c.at(ioff), slen));
    for (size_t i = slen; i < (size_t) VIRTIO_BLK_ID_BYTES; i++)
        EXPECT_EQ(0, c.at(ioff)[i]);
}

// A guest may offer less than the field's width, and the padding must stop at
// what it offered. The canary sits in guest memory immediately behind the
// described buffer -- reachable by an over-long scatter, described by nothing.
TEST_F(ChainFixture, get_id_into_a_shorter_buffer_writes_only_what_was_offered) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_GET_ID);
    const size_t offered = 8;
    size_t ioff = c.place(nullptr, offered);
    size_t coff = c.place(nullptr, offered);
    memset(c.at(ioff), SENTINEL, offered);
    memset(c.at(coff), SENTINEL, offered);
    c.add(ioff, offered, VRING_DESC_F_WRITE);
    size_t st = put_status(c);
    c.finish();

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    EXPECT_EQ((uint32_t) offered + 1, written);
    EXPECT_EQ(0, memcmp(CHAIN_SERIAL, c.at(ioff), offered));
    for (size_t i = 0; i < offered; i++)
        EXPECT_EQ(SENTINEL, c.at(coff)[i]);
}

// ---------------------------------------------------------------------------
// Ring generations
//
// `ready` answers "is there a usable ring right now", which is not the question
// a request suspended inside the backend needs answered. The transport clears
// ready when a reset or an unmap retires a ring and sets it true again for the
// replacement, so a request that suspended across that window passes the check
// and appends its completion to a used ring whose negotiation it never belonged
// to. The generation token is what distinguishes "a ring" from "that ring".
// ---------------------------------------------------------------------------

// lay a FLUSH chain into `r`'s descriptor table: a readable header at `hoff`
// and a writable status byte at `soff`, both offsets into whatever guest memory
// the translate hook is bound to. A FLUSH needs no data buffer, so the whole
// request fits in two small ranges.
void put_flush_chain(MemVring& r, size_t hoff, size_t soff) {
    r.desc[0].addr = hoff;
    r.desc[0].len = sizeof(virtio_blk_outhdr);
    r.desc[0].flags = VRING_DESC_F_NEXT;
    r.desc[0].next = 1;
    r.desc[1].addr = soff;
    r.desc[1].len = 1;
    r.desc[1].flags = VRING_DESC_F_WRITE;
    r.desc[1].next = 0;
    r.avail->ring[0] = 0;
    __atomic_store_n(&r.avail->idx, (uint16_t) 1, __ATOMIC_RELEASE);
}

class GenFixture : public ::testing::Test {
public:
    test::TestImage img;
    MemVring old_ring{RING_NUM};
    MemVring new_ring{RING_NUM};
    GuestChain guest;
    VirtQueueServer srv;
    virtio_blk_outhdr hdr{};
    size_t hoff = 0;
    size_t soff = 0;
    int translate_calls = 0;

    void SetUp() override {
        ASSERT_EQ(0, img.create("/tmp/photon-blk-vq-gen.img", CHAIN_CAPACITY));
        srv.backend = img.file;
        srv.capacity.store(CHAIN_CAPACITY, std::memory_order_relaxed);
        srv.serial = CHAIN_SERIAL;
        srv.tag = "vq-gen";
        srv.hooks.ready.bind(nullptr, &ready_true);
        srv.hooks.notify.bind(nullptr, &notify_thunk);
        srv.hooks.translate.bind(&guest, &chain_translate);
        srv.event_idx.store(false, std::memory_order_relaxed);
        srv.notify_valid.store(true, std::memory_order_relaxed);

        hdr.type = VIRTIO_BLK_T_FLUSH;
        hoff = guest.place(&hdr, sizeof(hdr));
        soff = guest.place(nullptr, 1);
        *guest.at(soff) = SENTINEL;
        put_flush_chain(new_ring, hoff, soff);
    }
    // what dispatch_avail does: one increment, then a coroutine holding the
    // generation current at that moment
    uint64_t dispatch_against(MemVring& r) {
        srv.set_ring(r.desc, r.avail, r.used, RING_NUM);
        srv.in_flight++;
        return srv.generation.load(std::memory_order_acquire);
    }
    // the driver reset and renegotiated onto a different ring: new addresses,
    // and the counters restart at zero the way a reset_pending refresh does
    void renegotiate() {
        srv.set_ring(new_ring.desc, new_ring.avail, new_ring.used, RING_NUM);
        srv.used_idx = 0;
        srv.last_avail = 0;
        srv.notify_valid.store(false, std::memory_order_relaxed);
    }
};

// translate, except that the call resolving the status descriptor renegotiates
// the ring first -- by which point serve_chain is already running, which is the
// only place a test can put the change between handle_req's two checks without
// a backend that yields. Note that renegotiate() republishes the SAME three
// pointers here and still retires the request: comparing ring addresses would
// not have caught this, and a driver that renegotiates onto the same buffers is
// not a hypothetical.
void* gen_translate_thunk(void* a, uint64_t addr, size_t len) {
    auto* f = (GenFixture*) a;
    if (++f->translate_calls == 2)
        f->renegotiate();
    return chain_translate(&f->guest, addr, len);
}

TEST_F(GenFixture, a_request_dispatched_before_a_renegotiation_does_not_complete_into_the_new_ring) {
    uint64_t gen = dispatch_against(old_ring);
    renegotiate();
    ASSERT_NE(gen, srv.generation.load(std::memory_order_acquire));

    srv.handle_req(0, gen);

    // the completion belongs to a negotiation that no longer exists, so it
    // appears in neither ring, the guest's status byte is left alone, and the
    // request is accounted for exactly as the uncompleted handover documents
    EXPECT_EQ(0, vring_used_idx(new_ring.used));
    EXPECT_EQ(0, vring_used_idx(old_ring.used));
    EXPECT_EQ(SENTINEL, *guest.at(soff));
    EXPECT_EQ(0u, srv.in_flight.load());
}

// The control the case above needs: without it, a handle_req that declined
// everything would pass too. Same ring, same chain, the generation it was
// actually dispatched at -- this one completes.
TEST_F(GenFixture, a_request_at_the_current_generation_still_completes) {
    uint64_t gen = dispatch_against(new_ring);

    srv.handle_req(0, gen);

    EXPECT_EQ(1, vring_used_idx(new_ring.used));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *guest.at(soff));
    EXPECT_EQ(0u, srv.in_flight.load());
}

// The case above is refused by the check that runs BEFORE serve_chain, so on its
// own it says nothing about the check that runs after -- which is the one that
// matters in production, because that is where a request that suspended inside
// the backend lands. This one moves the ring while the request is between the
// two: the translate hook renegotiates on the call that resolves the status
// descriptor, by which point serve_chain is already running.
TEST_F(GenFixture, a_ring_that_changes_mid_request_does_not_receive_its_completion) {
    uint64_t gen = dispatch_against(new_ring);
    srv.hooks.translate.bind(this, &gen_translate_thunk);

    srv.handle_req(0, gen);

    // serve_chain ran to the end: it resolved both descriptors, flushed the
    // backend and wrote the guest's status byte, all against the ring it was
    // dispatched from. Only the completion is refused.
    EXPECT_EQ(2, translate_calls);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *guest.at(soff));
    EXPECT_EQ(0, vring_used_idx(new_ring.used));
    EXPECT_EQ(0, vring_used_idx(old_ring.used));
    EXPECT_EQ(0u, srv.in_flight.load());
}

// The two cases above hand handle_req a generation they read themselves, so
// neither witnesses the other half of the binding: that dispatch_avail snapshots
// the CURRENT generation when it creates the request. Get that wrong and every
// request a real device dispatches is born stale and silently never completes.
// The transport suites do catch it, but only by failing most of their cases
// after long timeouts; this goes through dispatch_avail so the engine suite
// catches it in milliseconds.
TEST_F(GenFixture, dispatch_binds_the_generation_current_when_it_created_the_request) {
    srv.set_ring(new_ring.desc, new_ring.avail, new_ring.used, RING_NUM);
    srv.last_avail = 0;

    srv.dispatch_avail();
    // bounded: a request that is never completed must fail the assertions below
    // rather than hang the suite
    for (int i = 0; i < 2000 && srv.in_flight.load(); i++)
        photon::thread_usleep(1000);

    EXPECT_EQ(0u, srv.in_flight.load());
    EXPECT_EQ(1, vring_used_idx(new_ring.used));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *guest.at(soff));
}

int main(int argc, char** argv) {
    // A consumer child is this binary re-executed with a sentinel in argv[1]:
    // dispatch it before photon::init() and before gtest sees that argument.
    int cons = photon::blk::test::consumer_child_main(argc, argv);
    if (cons != photon::blk::test::CONS_NOT_A_CHILD)
        return cons;
    ::testing::InitGoogleTest(&argc, argv);
    if (photon::init(test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE))
        return -1;
    DEFER(photon::fini());
    set_log_output_level(1);
    return RUN_ALL_TESTS();
}
