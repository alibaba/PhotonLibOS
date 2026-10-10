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
#include <cstdint>
#include <cstring>
#include <memory>
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
    // A table deeper than the ring, for the cases that walk a chain up to the
    // engine's own depth bound: `ring_num` is the extent the walk checks every
    // guest-written index against, so a chain of N descriptors needs a table of at
    // least N. 64 KiB of guest memory is unchanged and still enough -- the deepest
    // chain built here is 64 * 512 bytes of payload plus a header and a status.
    explicit GuestChain(uint32_t table_size) : mem(64 * 1024, 0), desc(table_size) {}

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

void* chain_translate(void* a, uint64_t addr, size_t len, bool writable) {
    auto* c = (GuestChain*) a;
    // `writable` is not consulted here: this mock's guest memory is one read-write
    // buffer, so it has no per-buffer permission for the direction to disagree with.
    // DirectedGuest below is the one that models a transport whose mappings carry a
    // permission, which is what vduse's are.
    // wrap-safe, the spelling the engine's own bounds checks use: an address
    // near the top of the space must not be admitted by an overflowing sum
    if (addr > c->mem.size() || len > c->mem.size() - addr)
        return nullptr;
    return c->mem.data() + addr;
}

// A translate that models what a transport whose mappings carry a permission has to
// do: refuse an access the mapping forbids, and record every direction it was asked
// for. The recording is the point -- without it a case cannot tell "the engine
// forwarded the descriptor's own flag" from "the engine forwarded a constant", and a
// constant is what the plumbing would degrade to if the flag were dropped anywhere.
struct DirectedGuest {
    GuestChain c;
    uint64_t ro_end = 0;    // [0, ro_end) is mapped read-only
    std::vector<uint64_t> asked_addr;
    std::vector<uint8_t> asked_writable;
};

void* directed_translate(void* a, uint64_t addr, size_t len, bool writable) {
    auto* g = (DirectedGuest*) a;
    g->asked_addr.push_back(addr);
    g->asked_writable.push_back(writable ? 1 : 0);
    if (writable && addr < g->ro_end)
        return nullptr;   // the mapping forbids the access that was asked for
    return chain_translate(&g->c, addr, len, writable);
}

// A translate that models the guest rewriting its own descriptor while the device is
// mapping it. That interleaving needs no cooperation from our scheduler: the ring is
// guest memory, another of the guest's vCPUs can store into it at any instruction of
// ours, and on one transport the window between two reads of one descriptor can be an
// ioctl and an mmap wide.
//
// The rewrite is placed INSIDE the hook rather than raced from a second thread
// because the hook IS the window -- a thread would have to win a few-instruction race
// to land its store there, and a case that only usually fails is a case that usually
// books a kill it did not make. What this expresses is the interleaving without the
// timing: the first read of the descriptor has happened, the second has not.
struct RacingGuest {
    GuestChain c;
    // Which descriptor to rewrite, named by the buffer address its translate arrives
    // with, plus where that descriptor lives -- the ring, or a table inside c.mem.
    // Both halves are needed: the address says WHEN, and the hook has no other way to
    // name the slot.
    uint64_t mutate_addr = UINT64_MAX;
    vring_desc* slot = nullptr;
    uint32_t new_len = 0;
    bool rewrite_flags = false;
    uint16_t new_flags = 0;
    int mutations = 0;               // the non-vacuity control every case asserts on
    std::vector<uint32_t> asked_len;   // the length translate was asked to validate
};

void* racing_translate(void* a, uint64_t addr, size_t len, bool writable) {
    auto* g = (RacingGuest*) a;
    g->asked_len.push_back((uint32_t) len);
    if (addr == g->mutate_addr) {
        g->mutations++;
        g->slot->len = g->new_len;
        if (g->rewrite_flags)
            g->slot->flags = g->new_flags;
    }
    // Containment is answered for the length THIS call was given, which is what a
    // transport does: it validated that length and no other.
    return chain_translate(&g->c, addr, len, writable);
}

// The containment predicate the translate obligation is discharged with, against the
// two rows the review that added it demonstrated: a mapping of [0x2000,0x2fff] was
// being accepted both for a request that ran past its end and for one that started
// below it. Pure integers, so the table can also hold the top-of-space rows that no
// live mapping can be made to produce -- and those are the rows that catch the
// endpoint form, whose overflow SATISFIES the bound it was supposed to fail.
TEST(IovaRangeCovers, containment_is_wrap_free_and_refuses_a_zero_length) {
    struct Row { uint64_t start, last, iova; size_t len; bool want; const char* why; };
    static const Row rows[] = {
        {0x2000, 0x2fff, 0x2000, 0x1000, true,  "exact fit"},
        {0x2000, 0x2fff, 0x2800, 0x0800, true,  "ends exactly at last"},
        {0x2000, 0x2fff, 0x2800, 0x1000, false, "runs past last: the review's first row"},
        {0x2000, 0x2fff, 0x1000, 0x2000, false, "starts below start: the review's second row"},
        {0x2000, 0x2fff, 0x3000, 0x0010, false, "wholly above the range"},
        {0x2000, 0x2fff, 0x1000, 0x0010, false, "wholly below the range"},
        {0x2000, 0x2fff, 0x2000, 0,      false, "zero length is not a vacuous success"},
        {UINT64_MAX, UINT64_MAX, UINT64_MAX, 1, true, "one byte at the top of the space"},
        {0, UINT64_MAX, 0, 1, true,  "the whole space, one byte at its start"},
        {0, UINT64_MAX - 1, UINT64_MAX - 1, 1, true, "one byte at the range's own end"},
        {0x1000, 0x1fff, 0x1fff, 2, false, "one byte past the end of the range"},
        {0, UINT64_MAX, UINT64_MAX, 2, false, "len 2 at UINT64_MAX would wrap the sum"},
        // The one input where the leading `len` test is the ONLY thing refusing: for
        // every narrower range a zero length is caught by the size comparison
        // underflowing to SIZE_MAX, so without this row dropping `len &&` from the
        // predicate changes no answer and the mutation survives.
        {0, UINT64_MAX, 0, 0, false, "zero length against the whole space"},
        {UINT64_MAX - 3, UINT64_MAX, UINT64_MAX - 3, 4, true, "the widest fit at the top"},
        {UINT64_MAX - 3, UINT64_MAX, UINT64_MAX - 3, 5, false, "one byte too wide at the top"},
    };
    // A control on the table itself: a predicate stuck at one value passes every row
    // that wants that value, so a table wanting only one answer proves nothing.
    int want_true = 0, want_false = 0;
    for (const auto& r : rows) {
        r.want ? want_true++ : want_false++;
        EXPECT_EQ(r.want, iova_range_covers(r.start, r.last, r.iova, r.len))
            << r.why << ": [" << r.start << "," << r.last << "] iova " << r.iova
            << " len " << r.len;
    }
    EXPECT_GT(want_true, 0);
    EXPECT_GT(want_false, 0);
    EXPECT_EQ((int) (sizeof(rows) / sizeof(rows[0])), want_true + want_false);
}

// The other half of an iotlb invalidation: which rings a range covers. The ring is
// `a` and the range asked about is `b` -- in the transport, the range of a mapping
// the update took out of its lookup, which is the driver's mapping and not the
// message's own range. The rows that carry the finding are the three shapes such a
// range arrives in -- the whole address space, a subrange that covers one queue's
// vring, and the single byte at IOVA 0, which is a legal update that a test for
// "start == 0 && last == 0" reads as the first of the three. Deciding by
// intersection answers all three, and the empty range {1, 0} is how "this queue has
// no ring published" says it does not intersect anything, including the whole space.
TEST(IovaRangesIntersect, an_invalidated_range_finds_the_rings_it_covers) {
    struct Row { uint64_t a0, a1, b0, b1; bool want; const char* why; };
    static const Row rows[] = {
        {0x2000, 0x2fff, 0, UINT64_MAX, true,  "the kernel's full replacement covers any ring"},
        {0x2000, 0x2fff, 0x2000, 0x2fff, true,  "a subrange that is exactly the ring"},
        {0x2000, 0x2fff, 0x1000, 0x2000, true,  "overlaps only the ring's first byte"},
        {0x2000, 0x2fff, 0x2fff, 0x4000, true,  "overlaps only the ring's last byte"},
        {0x2000, 0x2fff, 0x2800, 0x2900, true,  "wholly inside the ring"},
        {0x2000, 0x2fff, 0x1000, 0x1fff, false, "ends one below the ring"},
        {0x2000, 0x2fff, 0x3000, 0x3fff, false, "starts one above the ring"},
        // The row the old spelling got wrong in the other direction: a one-byte range
        // at IOVA 0 is not a whole-address-space replacement, and a predicate that
        // cannot tell the two apart answers the same way for both.
        {0x2000, 0x2fff, 0, 0, false, "one byte at IOVA 0 misses a ring above it"},
        {0, 0x0fff, 0, 0, true,  "one byte at IOVA 0 does cover a ring starting there"},
        {1, 0, 0, UINT64_MAX, false, "no ring published, so not even the whole space hits"},
        {1, 0, 1, 0, false, "two empty ranges do not intersect each other"},
        {0, 0, 0, 0, true,  "one byte at IOVA 0 against itself"},
        {UINT64_MAX - 3, UINT64_MAX, 0, UINT64_MAX, true, "a ring at the top of the space"},
        {UINT64_MAX, UINT64_MAX - 1, 0, UINT64_MAX, false, "a reversed ring range is empty"},
        {0x2000, 0x2fff, 0x2fff, 0x2000, false, "the update's own range reversed is empty"},
    };
    int want_true = 0, want_false = 0;
    for (const auto& r : rows) {
        r.want ? want_true++ : want_false++;
        EXPECT_EQ(r.want, iova_ranges_intersect(r.a0, r.a1, r.b0, r.b1))
            << r.why << ": ring [" << r.a0 << "," << r.a1 << "] update ["
            << r.b0 << "," << r.b1 << "]";
        // Symmetry is part of the claim: "the update covers the ring" and "the ring
        // lies in the update" are one question, and an asymmetric answer would mean
        // the predicate is really testing containment of one side.
        EXPECT_EQ(r.want, iova_ranges_intersect(r.b0, r.b1, r.a0, r.a1))
            << "not symmetric: " << r.why;
    }
    EXPECT_GT(want_true, 0);
    EXPECT_GT(want_false, 0);
    EXPECT_EQ((int) (sizeof(rows) / sizeof(rows[0])), want_true + want_false);
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

    // `backend` defaults to the image itself; the caching cases pass a
    // test::BackendProbe so the sync becomes observable. `write_through` defaults
    // to false because every case that predates the caching policy asserts on the
    // bytes and none on persistence -- the ones that do pass it by name.
    // `ring_num` defaults to RING_NUM, which is what GuestChain's default constructor
    // sizes its table to, so every existing case passes the same value it always did;
    // the deep-chain cases pass their own table's extent, because that is the bound
    // the walk checks each guest-written descriptor index against.
    // `allow_indirect` defaults to true, which means every pre-existing case now runs
    // in a world where tables are permitted -- and none of their chains carries
    // VRING_DESC_F_INDIRECT, so nothing about them changes. Pinning the negotiation
    // gate (B4) therefore needs an EXPLICIT false; the default cannot do it.
    uint8_t serve(GuestChain& c, bool read_only, uint32_t* written,
                  bool write_through = false, fs::IFile* backend = nullptr,
                  uint32_t ring_num = RING_NUM, bool allow_indirect = true) {
        VirtioBlkTranslate tr;
        tr.bind(&c, &chain_translate);
        return virtio_blk_serve_chain(backend ? backend : img.file, read_only, write_through,
                                      allow_indirect, CHAIN_SERIAL, "vq", c.desc.data(), 0,
                                      ring_num, CHAIN_CAPACITY, tr, written);
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
    // the WRITE payload as its own device-readable descriptor, following a header
    // placed by put_header(). The cases above deliberately share one descriptor
    // between header and payload; this is the ordinary split a driver emits.
    void put_write_payload(GuestChain& c) {
        size_t off = c.place(pattern.data(), pattern.size());
        c.add(off, (uint32_t) pattern.size(), 0);
    }
    // The same walk through a translate that models a permission-carrying mapping:
    // it refuses a write into [0, ro_end) and records the direction of every call.
    uint8_t serve_directed(DirectedGuest& g, uint32_t* written, bool allow_indirect = true) {
        VirtioBlkTranslate tr;
        tr.bind(&g, &directed_translate);
        return virtio_blk_serve_chain(img.file, false, false, allow_indirect, CHAIN_SERIAL, "vq",
                                      g.c.desc.data(), 0, RING_NUM, CHAIN_CAPACITY,
                                      tr, written);
    }
    // The same walk through a translate that rewrites one descriptor on the way past,
    // which is the window between the engine's two reads of it. `backend` defaults to
    // the image; the cases that read the scatter list pass a ScatterProbe.
    uint8_t serve_racing(RacingGuest& g, uint32_t* written, fs::IFile* backend = nullptr) {
        VirtioBlkTranslate tr;
        tr.bind(&g, &racing_translate);
        return virtio_blk_serve_chain(backend ? backend : img.file, false, false, true,
                                      CHAIN_SERIAL, "vq", g.c.desc.data(), 0, RING_NUM,
                                      CHAIN_CAPACITY, tr, written);
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
// Compute the same FNV-1a hash that virtio_blk_serve_chain uses for GET_ID,
// formatted as a 16-char hex string into a VIRTIO_BLK_ID_BYTES-wide buffer.
static void fnv1a_serial(const char* input, char out[20]) {
    uint64_t h = 14695981039346656037ULL;
    for (const char* p = input; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    memset(out, 0, 20);
    snprintf(out, 20, "%016llx", (unsigned long long)h);
}

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

    char want[20];
    fnv1a_serial(CHAIN_SERIAL, want);
    EXPECT_EQ(0, memcmp(want, c.at(ioff), strlen(want)));
    for (size_t i = strlen(want); i < (size_t) VIRTIO_BLK_ID_BYTES; i++)
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
    char want[20];
    fnv1a_serial(CHAIN_SERIAL, want);
    EXPECT_EQ(0, memcmp(want, c.at(ioff), offered));
    for (size_t i = 0; i < offered; i++)
        EXPECT_EQ(SENTINEL, c.at(coff)[i]);
}

// ---------------------------------------------------------------------------
// The request path's working storage
//
// What the engine claims about serving a request is that it allocates nothing for
// it: the chain is walked into a DescStream that lives in serve_chain's own frame,
// whose iovec array is a fixed C array one element deep per descriptor the walk
// will take, and the scatter list the backend is handed is that array pointing at
// the guest's own buffers. Two consequences of that are observable from here and a
// third is not; the two cases below pin the first two, and (3) says what the third
// would need:
//
//   1. the depth is a COMPILE-TIME bound and the engine refuses past it rather than
//      growing: a chain of exactly DEEPEST_CHAIN is served and one descriptor deeper
//      is refused. That bound IS the array's depth -- DescStream's iov[] carries one
//      element per descriptor the walk will take -- so it is also what sets
//      serve_chain's stack footprint, and raising it to serve deeper chains is the
//      change that would first need the storage moved off the stack.
//   2. the payload reaches the backend as ONE gather over the guest's buffers. A
//      per-request bounce buffer, the one-line way to break the claim, is invisible
//      in the bytes that land and in every count test::BackendProbe already keeps;
//      the iovec bases and lengths are what betray it, so this records them.
//   3. that the array is an array rather than something with a heap behind it is NOT
//      observable from here: a std::vector<iovec> of the same depth produces the same
//      bases, the same lengths and the same single call, so both cases below stay
//      green over it. Witnessing that directly needs an allocator instrument, and one
//      perturbs the path it is measuring -- which is why (1) is pinned instead: it is
//      the part of the claim a reason to heap-allocate would have to break first.
// ---------------------------------------------------------------------------

// blk/utils.cpp's MAX_DESC_CHAIN, spelled out here on purpose. It is file-local to
// the implementation and this suite family's standing rule is that it must not read
// the values it asserts against out of the code under test (see PEER_MAX_QUEUES in
// test-vduse.cpp): the constant is what bounds both the walk's step count and the
// iovec array's depth, so if it ever moves these two cases go red, which is the
// point.
constexpr int DEEPEST_CHAIN = 64;
constexpr int SCATTER_MAX_IOV = DEEPEST_CHAIN;

// The scatter list exactly as the backend received it. Derived from BackendProbe in
// this file rather than added to harness.h: that header is compiled into five
// binaries and its classes are the ones whose layout the project has already
// measured and pinned, so an observation only this suite needs stays here.
class ScatterProbe : public test::BackendProbe {
public:
    // does NOT own f, exactly as BackendProbe does not
    explicit ScatterProbe(fs::IFile* f) : test::BackendProbe(f) {}

    struct Call {
        int iovcnt = 0;
        void* base[SCATTER_MAX_IOV] = {};
        size_t len[SCATTER_MAX_IOV] = {};
    };
    // One entry per call, in order. A vector because the number of calls is the
    // thing being asserted -- a fixed array would have to guess it.
    std::vector<Call> calls;

    // preadv only, because both cases below build READ chains: that is the arm whose
    // destination has to be the guest's own memory for the bytes to land anywhere the
    // guest can see. pwritev is left to BackendProbe, whose short-count knob these
    // cases do not use.
    ssize_t preadv(const struct iovec* iov, int iovcnt, off_t offset) override {
        Call c;
        c.iovcnt = iovcnt;
        for (int i = 0; i < iovcnt && i < SCATTER_MAX_IOV; i++) {
            c.base[i] = iov[i].iov_base;
            c.len[i] = iov[i].iov_len;
        }
        calls.push_back(c);
        return test::BackendProbe::preadv(iov, iovcnt, offset);
    }
};

// Lay out a READ chain `ndesc` descriptors deep: a readable header, ndesc - 2
// writable payload descriptors of one sector each, and the writable status byte.
// Returns the payload offsets in guest memory, in chain order -- which is what the
// scatter list is compared against.
static std::vector<size_t> put_read_chain(GuestChain& c, int ndesc) {
    virtio_blk_outhdr hdr{};
    hdr.type = VIRTIO_BLK_T_IN;
    hdr.sector = CHAIN_SECTOR;
    c.add(c.place(&hdr, sizeof(hdr)), sizeof(hdr), 0);
    std::vector<size_t> offs;
    for (int i = 0; i < ndesc - 2; i++) {
        size_t o = c.place(nullptr, 512);
        memset(c.at(o), SENTINEL, 512);
        c.add(o, 512, VRING_DESC_F_WRITE);
        offs.push_back(o);
    }
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;
    c.add(st, 1, VRING_DESC_F_WRITE);
    c.finish();
    return offs;
}

// The deepest chain the walk will take, served. Which assertion carries what:
//   serve() == S_OK, written == payload + 1   a chain this deep is servable at all,
//                                             so the counts below are not the
//                                             silence of a refusal
//   calls.size() == 1                         gathered, not looped over per element
//   iovcnt == PAYLOAD_DESCS, and every base   THE discriminator for the no-copy
//   equal to the guest buffer it describes    claim: a per-request bounce buffer
//                                             answers the first two and puts heap
//                                             addresses here instead
//   the payload memcmp                        the control on the bases: it says the
//                                             addresses recorded really were where
//                                             the bytes went, which a bounce buffer
//                                             also satisfies -- so it corroborates
//                                             the probe and discriminates nothing
//                                             on its own
TEST_F(ChainFixture, the_deepest_chain_reaches_the_backend_as_one_gather_over_the_guest_buffers) {
    constexpr int PAYLOAD_DESCS = DEEPEST_CHAIN - 2;   // a header and a status take the other two
    constexpr size_t PAYLOAD_BYTES = (size_t) PAYLOAD_DESCS * 512;
    GuestChain c(DEEPEST_CHAIN);
    ScatterProbe backend(img.file);

    // A pattern where the read will fetch from, so "the scatter list was the
    // destination" is witnessed by bytes and not only by addresses.
    std::vector<uint8_t> src(PAYLOAD_BYTES);
    fill_pattern(src.data(), src.size(), 0x33);
    ASSERT_EQ((ssize_t) PAYLOAD_BYTES,
              img.file->pwrite(src.data(), src.size(), (off_t) (CHAIN_SECTOR << 9)));

    std::vector<size_t> offs = put_read_chain(c, DEEPEST_CHAIN);
    ASSERT_EQ((uint16_t) DEEPEST_CHAIN, c.ndesc);
    size_t st = (size_t) c.desc[DEEPEST_CHAIN - 1].addr;   // the status descriptor's guest offset
    ASSERT_EQ(SENTINEL, *c.at(st));

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK,
              serve(c, false, &written, false, &backend, (uint32_t) c.desc.size()));
    EXPECT_EQ((uint32_t) PAYLOAD_BYTES + 1, written);

    // ONE call, not one per descriptor: the chain was gathered, not looped over.
    ASSERT_EQ(1u, backend.calls.size());
    // ... and gathered over the GUEST's buffers, in chain order, each element the
    // length its descriptor carried. This is the assertion the no-copy half of the
    // budget rests on: a bounce buffer satisfies every count above and puts heap
    // addresses here instead.
    ASSERT_EQ(PAYLOAD_DESCS, backend.calls[0].iovcnt);
    for (int i = 0; i < PAYLOAD_DESCS; i++) {
        EXPECT_EQ((void*) (c.mem.data() + offs[i]), backend.calls[0].base[i]) << "iovec " << i;
        EXPECT_EQ(512u, backend.calls[0].len[i]) << "iovec " << i;
        EXPECT_EQ(0, memcmp(src.data() + (size_t) i * 512, c.at(offs[i]), 512)) << "payload " << i;
    }
    // The status is the writable stream's last byte, which at this depth is the
    // chain's own last descriptor. Written, so the walk reached the end of a
    // 64-descriptor chain and located it there rather than refusing.
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
}

// One descriptor past the deepest, refused. This is the other side of the depth
// bound: the constant cannot be raised to serve a deeper chain without reddening
// it, and raising it is the change that would have to move the working storage off
// the stack, because DescStream's array is one element deep per descriptor the walk
// takes.
//
// The refusal is detected MID-WALK -- the loop ran out of steps before it reached a
// descriptor without VRING_DESC_F_NEXT -- so the writable stream's last byte is
// somewhere in the middle of the chain and no status is written at all. That is the
// documented behaviour of a mid-walk refusal, and
// a_buffer_whose_mapping_forbids_the_access_is_refused_as_ioerr below pins the same
// half from the other direction; what is added here is that the backend is left
// untouched, i.e. nothing was gathered into a partial request either.
TEST_F(ChainFixture, a_chain_one_past_the_deepest_is_refused_without_touching_the_backend) {
    constexpr int TOO_DEEP = DEEPEST_CHAIN + 1;
    GuestChain c(TOO_DEEP);
    ScatterProbe backend(img.file);
    std::vector<size_t> offs = put_read_chain(c, TOO_DEEP);
    ASSERT_EQ((uint16_t) TOO_DEEP, c.ndesc);
    size_t st = (size_t) c.desc[TOO_DEEP - 1].addr;   // the status descriptor's guest offset
    ASSERT_EQ(SENTINEL, *c.at(st));

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR,
              serve(c, false, &written, false, &backend, (uint32_t) c.desc.size()));
    // nothing reached the backend, and no status was written over a data byte
    EXPECT_EQ(0u, backend.calls.size());
    EXPECT_EQ(0u, written);
    EXPECT_EQ(SENTINEL, *c.at(st));
    // nor into the guest's payload: every buffer still holds the prefill
    for (size_t o : offs)
        EXPECT_EQ(SENTINEL, *c.at(o));
}

// ---------------------------------------------------------------------------
// Caching policy
//
// A driver that did not negotiate VIRTIO_BLK_F_FLUSH has no command with which to
// ask for its writes to reach stable storage, so the device has to make them
// durable before it completes them. The engine learns that as one flag the
// transport derives from the NEGOTIATED feature word; what is tested here is what
// the engine does with it. The oracle is the sync count, because the bytes are the
// same either way.
// ---------------------------------------------------------------------------

TEST_F(ChainFixture, a_write_in_write_through_mode_is_persisted_before_it_completes) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_OUT);
    put_write_payload(c);
    size_t st = put_status(c);
    c.finish();

    test::BackendProbe backend(img.file);
    const bool write_through = true;
    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written, write_through, &backend));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    std::vector<uint8_t> back(pattern.size());
    EXPECT_EQ((ssize_t) pattern.size(), read_back(back.data(), back.size()));
    EXPECT_EQ(0, memcmp(back.data(), pattern.data(), pattern.size()));
    // the whole point: the data landing is not what distinguishes the two modes
    EXPECT_EQ(1, backend.datasyncs.load());
    EXPECT_EQ(0, backend.syncs.load());
}

TEST_F(ChainFixture, a_write_in_write_back_mode_leaves_persistence_to_an_explicit_flush) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_OUT);
    put_write_payload(c);
    size_t st = put_status(c);
    c.finish();

    test::BackendProbe backend(img.file);
    const bool write_through = false;
    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written, write_through, &backend));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    std::vector<uint8_t> back(pattern.size());
    EXPECT_EQ((ssize_t) pattern.size(), read_back(back.data(), back.size()));
    EXPECT_EQ(0, memcmp(back.data(), pattern.data(), pattern.size()));
    // write-back is not "do not write", it is "do not promise stable storage yet".
    // This zero is the other half of the oracle: a counter that could only read 1
    // would prove nothing in the case above.
    EXPECT_EQ(0, backend.datasyncs.load());
    EXPECT_EQ(0, backend.syncs.load());
}

TEST_F(ChainFixture, an_explicit_flush_does_not_become_two_syncs_in_write_through_mode) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_FLUSH);
    size_t st = put_status(c);
    c.finish();

    test::BackendProbe backend(img.file);
    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written, true, &backend));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    // FLUSH carries no data phase, so the write-through path has nothing of its own
    // to persist and must not add a second sync to the one FLUSH is
    EXPECT_EQ(1, backend.datasyncs.load());
    EXPECT_EQ(0, backend.syncs.load());
}

TEST_F(ChainFixture, a_write_refused_by_read_only_does_not_persist) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_OUT);
    put_write_payload(c);
    size_t st = put_status(c);
    c.finish();

    test::BackendProbe backend(img.file);
    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, true, &written, true, &backend));
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, *c.at(st));
    // nothing reached the backend, so there is nothing to make durable
    EXPECT_EQ(0, backend.datasyncs.load());
}

TEST_F(ChainFixture, a_write_refused_as_out_of_bounds_does_not_persist) {
    GuestChain c;
    virtio_blk_outhdr hdr{};
    hdr.type = VIRTIO_BLK_T_OUT;
    hdr.sector = CHAIN_CAPACITY >> 9;   // the first sector past the end
    size_t hoff = c.place(&hdr, sizeof(hdr));
    c.add(hoff, sizeof(hdr), 0);
    put_write_payload(c);
    size_t st = put_status(c);
    c.finish();

    test::BackendProbe backend(img.file);
    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &written, true, &backend));
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, *c.at(st));
    // a refused write must not be persisted, and must not grow the image either:
    // the sync is what would have made a partial or extending write durable
    EXPECT_EQ(0, backend.datasyncs.load());
}

TEST_F(ChainFixture, a_write_that_landed_short_does_not_persist) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_OUT);
    put_write_payload(c);
    size_t st = put_status(c);
    c.finish();

    test::BackendProbe backend(img.file);
    backend.short_write_by = 1;   // 511 of the 512 bytes asked for
    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &written, true, &backend));
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, *c.at(st));
    // The count is positive, so this is the one rejected write that reaches the
    // sync still holding bytes it wrote: without the exit after the short count,
    // write-through would persist a request it is about to report as failed.
    EXPECT_EQ(0, backend.datasyncs.load());
    EXPECT_EQ(0, backend.syncs.load());
}

TEST_F(ChainFixture, a_write_whose_persist_fails_is_reported_as_failed) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_OUT);
    put_write_payload(c);
    size_t st = put_status(c);
    c.finish();

    test::BackendProbe backend(img.file);
    backend.fail_syncs = true;
    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &written, true, &backend));
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, *c.at(st));
    // The bytes DID reach the backend, which is what makes reporting success
    // actively wrong here rather than merely optimistic: the completion is the
    // device's word that the data is where it promised, and in write-through mode
    // that promise is stable storage. One count, so this says "asked and failed"
    // and not "never asked".
    EXPECT_EQ(1, backend.datasyncs.load());
}

TEST_F(ChainFixture, a_read_in_write_through_mode_does_not_persist) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_IN);
    size_t p = c.place(nullptr, pattern.size());
    c.add(p, (uint32_t) pattern.size(), VRING_DESC_F_WRITE);
    size_t st = put_status(c);
    c.finish();

    test::BackendProbe backend(img.file);
    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written, true, &backend));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    // write-through is a promise about writes; a read has nothing to persist and
    // syncing for one would cost a whole-file flush per request
    EXPECT_EQ(0, backend.datasyncs.load());
    EXPECT_EQ(0, backend.syncs.load());
}

TEST_F(ChainFixture, a_write_carrying_no_payload_does_not_persist) {
    GuestChain c;
    put_header(c, VIRTIO_BLK_T_OUT);
    size_t st = put_status(c);
    c.finish();

    test::BackendProbe backend(img.file);
    uint32_t written = 0;
    // zero bytes is a whole number of sectors and in bounds, so the request is
    // served; it just wrote nothing, and there is nothing to make durable
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &written, true, &backend));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    EXPECT_EQ(0, backend.datasyncs.load());
    EXPECT_EQ(0, backend.syncs.load());
}

// ---------------------------------------------------------------------------
// Ring generations
//
// What a request suspended inside the backend needs to know is whether the ring it
// was dispatched against is still the published one, and `ready` cannot answer
// that: it says whether a ring is usable NOW, and a transport that retires one sets
// it true again for the replacement, so a request suspended across that window
// would append its completion to a used ring whose negotiation it never belonged
// to. The generation token is what distinguishes "a ring" from "that ring", and it
// is the whole of that identity check -- `ready` is not consulted on the completion
// path at all, because it is the DISPATCH gate, and a frontend can pause a queue
// whose ring is perfectly good. That is not a reason to throw away a request already
// taken. The cases immediately below pin the first half of that distinction --
// a retired ring is declined -- and DepthFixture's two pause cases pin the second.
// ---------------------------------------------------------------------------

// The direction the engine will make of a buffer travels with the translate call,
// and these two cases are what keeps it travelling. Without them a translate handed a
// constant would pass every other case in this suite: the mock's guest memory is
// read-write, so nothing before these could tell the difference -- and that
// difference is the whole of a permission-carrying transport's protection against
// writing through a mapping the driver made read-only.
TEST_F(ChainFixture, translate_is_told_the_access_the_engine_will_make_of_each_buffer) {
    DirectedGuest g;
    put_header(g.c, VIRTIO_BLK_T_OUT);      // readable
    put_write_payload(g.c);                 // readable: a WRITE's payload
    size_t st = put_status(g.c);            // writable
    g.c.finish();

    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve_directed(g, &written));
    // one call per descriptor, in chain order, each carrying its own direction: a
    // constant in either direction fails this, and so does dropping the flag
    ASSERT_EQ(3u, g.asked_writable.size());
    EXPECT_EQ(0, g.asked_writable[0]);
    EXPECT_EQ(0, g.asked_writable[1]);
    EXPECT_EQ(1, g.asked_writable[2]);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *g.c.at(st));
}

TEST_F(ChainFixture, a_buffer_whose_mapping_forbids_the_access_is_refused_as_ioerr) {
    // A READ's destination is device-writable. Placing it inside the window this
    // translate reports as read-only is the disagreement that, left unrefused, becomes
    // a write through a mapping that forbids it -- a fault in our own process rather
    // than a status the guest can be told about.
    DirectedGuest g;
    put_header(g.c, VIRTIO_BLK_T_IN);       // readable
    size_t doff = g.c.place(nullptr, 512);  // the destination
    g.c.add(doff, 512, VRING_DESC_F_WRITE);
    size_t st = put_status(g.c);
    g.c.finish();

    // The control first, on this same chain: with nothing marked read-only it is
    // served, so the refusal below is the window's doing and not the chain's.
    uint32_t written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve_directed(g, &written));
    g.asked_addr.clear();
    g.asked_writable.clear();

    g.ro_end = doff + 512;
    *g.c.at(st) = SENTINEL;
    written = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve_directed(g, &written));
    // The guest's own status byte is left alone, and that is the documented half of a
    // refusal detected MID-WALK rather than after it: the status is the writable
    // stream's last byte, and a walk that broke at the second of three descriptors
    // never reached the one holding it, so the only "last writable byte" it saw was
    // somewhere in the middle of the chain. Writing IOERR there would corrupt a data
    // byte and leave the real status saying whatever the guest left in it. The other
    // refusal cases in this fixture assert the opposite -- IOERR written -- because
    // they are refused after the walk finished and the status is located.
    EXPECT_EQ(SENTINEL, *g.c.at(st));
    // And the refusal is the direction check firing rather than a bounds failure: the
    // address refused is the very one that was served a moment earlier, and it was
    // refused on a call that asked to write.
    ASSERT_FALSE(g.asked_addr.empty());
    EXPECT_EQ((uint64_t) doff, g.asked_addr.back());
    EXPECT_EQ(1, g.asked_writable.back());
}

// ---------------------------------------------------------------------------
// One read of a guest-owned field
//
// Range-checking a peer-supplied integer is only half the obligation: the value
// checked and the value used have to be the same value, and the ring stays writable
// while its request is served, so a field read twice can answer twice. The window
// between the two reads of one descriptor is that descriptor's translate -- which can
// be an ioctl and an mmap on one transport, and is a few integer comparisons on the
// other -- and the field whose second read costs something is the length: translate
// validated one range and the iovec carries another.
//
// Three cases, one per site where a re-read changes an answer: the length of a ring
// descriptor, the length of a table entry (further down, with the indirect cases,
// because that is a second walk with its own copy), and the NEXT flag, whose second
// read has a different consequence -- a chain that ends early is treated as complete,
// so the writable stream's last byte is a DATA byte and the request is served one byte
// short of what the driver named.
//
// What these cases pin is the invariant, not a timing: they cannot witness how WIDE
// the production window is, and on the transport where it is a few integer comparisons
// wide a hostile guest still gets to retry it in a loop.
// ---------------------------------------------------------------------------

// The reviewer's probe in the engine's own terms: validated_mapping=512,
// backend_read_length=1024, result=OK. Which assertion carries what:
//   mutations == 1        non-vacuity. Without it a rewrite that never landed would
//                         leave a normally served chain passing every check below, and
//                         the case would book a kill it did not make.
//   asked_len == iov_len  THE invariant, and the one a second read of the length
//                         breaks: the left side stays 512 either way, the right side
//                         is what push() was handed.
//   the victim memcmp     the consequence rather than the bookkeeping -- preadv filled
//                         512 bytes of a guest range no descriptor named. In a
//                         transport that is past the end of an mmap.
//   w, the status byte    the request completed, so the three above are not the
//   and the payload       silence of a refusal
TEST_F(ChainFixture, a_descriptor_lengthened_while_it_is_translated_does_not_lengthen_the_request) {
    RacingGuest g;
    put_header(g.c, VIRTIO_BLK_T_IN);        // readable, descriptor 0
    size_t doff = g.c.place(nullptr, 512);   // the READ's destination, descriptor 1
    memset(g.c.at(doff), SENTINEL, 512);
    g.c.add(doff, 512, VRING_DESC_F_WRITE);
    size_t voff = g.c.place(nullptr, 512);   // guest memory just past that buffer
    memset(g.c.at(voff), SENTINEL, 512);
    size_t st = put_status(g.c);             // writable, descriptor 2
    g.c.finish();

    // Two sectors of source, so a READ that overruns its buffer has backend bytes to
    // put in the victim, and so the request stays inside capacity either way -- the
    // length is what differs here, not the bound on it.
    std::vector<uint8_t> src(1024);
    fill_pattern(src.data(), src.size(), 0x55);
    ASSERT_EQ((ssize_t) 1024,
              img.file->pwrite(src.data(), src.size(), (off_t) (CHAIN_SECTOR << 9)));
    ScatterProbe backend(img.file);

    g.mutate_addr = doff;
    g.slot = &g.c.desc[1];
    g.new_len = 1024;   // twice what translate is about to be asked to validate

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve_racing(g, &w, &backend));
    EXPECT_EQ(1, g.mutations) << "the rewrite never landed, so this case proved nothing";

    ASSERT_EQ(3u, g.asked_len.size());   // header, destination, status
    EXPECT_EQ(512u, g.asked_len[1]) << "translate was not asked about the buffer it validated";
    ASSERT_EQ(1u, backend.calls.size());
    ASSERT_EQ(1, backend.calls[0].iovcnt);
    EXPECT_EQ(g.asked_len[1], backend.calls[0].len[0])
        << "the scatter list carries a length translate never validated";

    std::vector<uint8_t> victim(512, SENTINEL);
    EXPECT_EQ(0, memcmp(victim.data(), g.c.at(voff), victim.size()))
        << "the backend wrote past the buffer its descriptor named";
    EXPECT_EQ(0, memcmp(src.data(), g.c.at(doff), 512))
        << "the buffer that WAS named did not get the bytes";
    EXPECT_EQ(513u, w);   // 512 of data plus the status byte
    EXPECT_EQ(VIRTIO_BLK_S_OK, *g.c.at(st));
}

// The flag whose second read changes an answer, and a different consequence: a NEXT
// that disappears mid-walk ends the chain at the descriptor before the status, the
// engine takes that for a complete chain, and wr.tail(1) then hands out the payload's
// last byte as the status. That is precisely the shape the indirect branch refuses at
// its own INDIRECT-with-NEXT check, arriving instead through the back door of a
// re-read. The backend call count is the assertion that cannot be satisfied by a
// refusal: a chain that ended early issues no IO at all.
TEST_F(ChainFixture, a_next_flag_cleared_while_the_buffer_is_translated_does_not_end_the_chain) {
    RacingGuest g;
    put_header(g.c, VIRTIO_BLK_T_IN);
    size_t doff = g.c.place(nullptr, 512);
    memset(g.c.at(doff), SENTINEL, 512);
    g.c.add(doff, 512, VRING_DESC_F_WRITE);
    size_t st = put_status(g.c);
    g.c.finish();

    std::vector<uint8_t> src(512);
    fill_pattern(src.data(), src.size(), 0x66);
    ASSERT_EQ((ssize_t) 512,
              img.file->pwrite(src.data(), src.size(), (off_t) (CHAIN_SECTOR << 9)));
    ScatterProbe backend(img.file);

    g.mutate_addr = doff;
    g.slot = &g.c.desc[1];
    g.new_len = 512;   // the length is not this case's subject
    g.rewrite_flags = true;
    g.new_flags = VRING_DESC_F_WRITE;   // what it held, minus NEXT

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve_racing(g, &w, &backend));
    EXPECT_EQ(1, g.mutations) << "the rewrite never landed, so this case proved nothing";

    ASSERT_EQ(1u, backend.calls.size()) << "the walk ended before the status descriptor";
    ASSERT_EQ(1, backend.calls[0].iovcnt);
    EXPECT_EQ(512u, backend.calls[0].len[0]);
    EXPECT_EQ(513u, w);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *g.c.at(st));
    // The payload kept all 512 of its bytes: the truncated chain writes the status
    // over the last of them and leaves the guest's own status byte untouched.
    EXPECT_EQ(0, memcmp(src.data(), g.c.at(doff), src.size()))
        << "the payload's last byte was handed out as the status";
}

// ---------------------------------------------------------------------------
// indirect tables
//
// GuestChain::mem IS the guest memory, so a table is just another placed range:
// place() it, point one ring descriptor at it with VRING_DESC_F_INDIRECT, and the
// entries inside it name the buffers. Two traps, each of which produces a case that
// measures something other than what its name says:
//
//   add() cannot build the ring descriptor. It sets VRING_DESC_F_NEXT
//   unconditionally and sets next = ndesc + 1, so a table descriptor built with it
//   also carries NEXT -- the shape the engine refuses. Write desc[...] directly, the
//   way DepthFixture::SetUp does.
//
//   A table of ZEROED entries is not a long table. flags == 0 means no NEXT, so the
//   walk stops after one entry and the entry-count bound never fires. Exercising that
//   bound needs entries that are explicitly chained: len 0, VRING_DESC_F_NEXT set,
//   next = i + 1, and the last one clear.
//
// place() only EXPECT_LEs its bound, so a table that does not fit is REPORTED and then
// built anyway -- which turns a case about the entry count into a case about translate
// failing. begin() below asserts the fit.
// ---------------------------------------------------------------------------

// blk/utils.h's MAX_INDIRECT_ENTRIES, spelled out here for the reason DEEPEST_CHAIN
// above is: this suite must not read the value it asserts against out of the code
// under test, or a cap that moved would move the expectation with it and nothing
// would go red.
constexpr uint32_t TABLE_ENTRIES_MAX = 64;

// One indirect table inside a GuestChain, plus the ring descriptor that names it.
struct IndirectTable {
    GuestChain* c = nullptr;
    size_t off = 0;          // the table's guest address
    uint32_t n = 0;          // entries the table DECLARES, i.e. de->len / 16
    uint16_t ring_idx = 0;   // the ring descriptor holding the INDIRECT flag

    // Lay `count` zeroed entries and return a pointer to them; the caller fills them.
    // `declared` is what the ring descriptor's len will say, which need not equal
    // `count` -- that disagreement is exactly what E1 and E3 test.
    vring_desc* begin(uint32_t count, uint32_t declared) {
        off = c->place(nullptr, (size_t) count * sizeof(vring_desc));
        n = declared;
        auto* t = (vring_desc*) c->at(off);
        memset(t, 0, (size_t) count * sizeof(vring_desc));
        EXPECT_LE((size_t) count * sizeof(vring_desc), c->mem.size() - off);
        return t;
    }
    // Chain `count` entries into a zero-length list: every one asks for one byte of a
    // mapping it never uses, which is what makes an over-cap table cost a translate per
    // entry instead of terminating at the first. See the second trap above.
    static void chain_zero_length(vring_desc* t, uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
            t[i].addr = 0;
            t[i].len = 0;
            t[i].flags = (i + 1 < count) ? VRING_DESC_F_NEXT : 0;
            t[i].next = (uint16_t) (i + 1);
        }
    }
    // The ring descriptor. Written directly, NOT through add(): see the traps above.
    // `extra_flags` is how a case adds VRING_DESC_F_WRITE (E6) or VRING_DESC_F_NEXT
    // (E7) to the INDIRECT flag; `declared_len` overrides n * 16 for E3 / E3b / E4.
    void publish(uint16_t idx, uint16_t extra_flags = 0, uint32_t declared_len = 0) {
        ring_idx = idx;
        c->desc[idx].addr = off;
        c->desc[idx].len = declared_len ? declared_len : n * (uint32_t) sizeof(vring_desc);
        c->desc[idx].flags = (uint16_t) (VRING_DESC_F_INDIRECT | extra_flags);
        c->desc[idx].next = 0;
    }
};

// A virtio_blk_outhdr placed in guest memory, returned as its address. The fixture's
// put_header() also adds a RING descriptor, which is the one thing an indirect case
// must not do: the header belongs in the table.
size_t put_table_header(GuestChain& c, uint32_t type, uint64_t sector = CHAIN_SECTOR) {
    virtio_blk_outhdr hdr{};
    hdr.type = type;
    hdr.sector = sector;
    return c.place(&hdr, sizeof(hdr));
}

// The four things a served request leaves behind, so that E8 can compare a direct
// chain and an indirect table for one request as a single value. Comparing them one
// at a time would let each be satisfied by a different bug: `written` alone hides a
// wrong used length behind a right byte count, and the image alone hides a status
// that was never written.
struct ChainOutcome {
    uint8_t st = 0;
    uint32_t w = 0;
    uint8_t status_byte = 0;
    std::vector<uint8_t> image;
};

// The negotiation gate's SHAPE: a table offered to an engine told not to walk one is
// refused, and refused the way every other mid-walk refusal in this file is -- no
// status written, no bytes reported. What this case cannot see is whether a transport
// derives allow_indirect from the NEGOTIATED features or from its own offer; that is
// M2, which has a frontend able to decline one bit.
TEST_F(ChainFixture, an_indirect_descriptor_is_refused_when_the_feature_was_not_negotiated) {
    GuestChain c;
    IndirectTable t;
    t.c = &c;
    size_t hoff = put_table_header(c, VIRTIO_BLK_T_IN);
    size_t doff = c.place(nullptr, 512);
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;
    vring_desc* tbl = t.begin(3, 3);
    tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
    tbl[1] = vring_desc{doff, 512, (uint16_t) (VRING_DESC_F_WRITE | VRING_DESC_F_NEXT), 2};
    tbl[2] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
    t.publish(0);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR,
              serve(c, false, &w, false, nullptr, RING_NUM, /*allow_indirect=*/false));
    EXPECT_EQ(0u, w);
    EXPECT_EQ(SENTINEL, *c.at(st));
}

// A table entry's `next` indexes the TABLE, and the bound on it is the table's own
// entry count. Taking the bound from the ring instead passes the ring's index check
// for any value below ring_num -- the two counts are unrelated -- and then reads a
// descriptor past the end of the table. What sits there in this fixture is a planted
// descriptor naming a 513-byte buffer, so the wrong bound turns into a preadv into
// memory the driver never named for this request.
//
// 513 and not 512: at 512 the writable stream's last byte is taken as the status,
// leaving want = 511, and the whole-sector check refuses that before any byte is
// written -- the decoy would stay untouched and the assertion would prove nothing.
TEST_F(ChainFixture, an_out_of_range_table_next_is_refused_instead_of_read_past_the_table) {
    GuestChain c;
    // Seed the image where the read would come from, so "the decoy was read into" is
    // distinguishable in bytes from "the decoy still holds its prefill".
    std::vector<uint8_t> seed(512);
    fill_pattern(seed.data(), seed.size(), 0x33);
    ASSERT_EQ((ssize_t) 512, img.file->pwrite(seed.data(), seed.size(), (off_t) (CHAIN_SECTOR << 9)));

    std::vector<uint8_t> decoy(513, SENTINEL);
    size_t decoy_off = c.place(decoy.data(), decoy.size());
    size_t hoff = put_table_header(c, VIRTIO_BLK_T_IN);
    size_t doff = c.place(nullptr, 512);
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;

    IndirectTable t;
    t.c = &c;
    // Six entries laid out, THREE declared. Index 5 is outside the table the engine
    // was told about and inside the range it would read if it bounded `next` by the
    // ring (RING_NUM is 8), which is the whole of the discrimination.
    vring_desc* tbl = t.begin(6, 3);
    tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 5};
    tbl[1] = vring_desc{doff, 512, (uint16_t) (VRING_DESC_F_WRITE | VRING_DESC_F_NEXT), 2};
    tbl[2] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
    tbl[5] = vring_desc{decoy_off, 513, VRING_DESC_F_WRITE, 0};
    t.publish(0);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &w));
    EXPECT_EQ(0u, w);
    EXPECT_EQ(SENTINEL, *c.at(st));
    EXPECT_EQ(SENTINEL, *c.at(decoy_off))
        << "a descriptor past the end of the table was read and served from";
}

// §2.7.5.3.2: "The device MUST handle the case of zero or more normal chained
// descriptors followed by a single descriptor with flags&VIRTQ_DESC_F_INDIRECT." The
// table is not the only thing in the chain, and the descriptors before it keep their
// bytes in the same two streams its entries go into -- here the header is split with
// its first half in the ring and its second half in the table.
//
// This is the case that catches an inner walk indexing desc[] instead of the
// translated table: at t == 0 it re-reads the ring's own header half and at t == 1 it
// reaches the ring descriptor carrying INDIRECT, which the nesting rule then refuses.
// It fails on that collision, not on the header's contents being wrong.
TEST_F(ChainFixture, normal_descriptors_before_a_trailing_indirect_table_are_all_served) {
    GuestChain c;
    // A FLUSH: it needs no data phase, so the whole request is the split header and
    // the status, and every byte of the header has to come from somewhere.
    virtio_blk_outhdr hdr{};
    hdr.type = VIRTIO_BLK_T_FLUSH;
    size_t hoff = c.place(&hdr, sizeof(hdr));
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;

    IndirectTable t;
    t.c = &c;
    vring_desc* tbl = t.begin(2, 2);
    tbl[0] = vring_desc{hoff + 8, sizeof(virtio_blk_outhdr) - 8, VRING_DESC_F_NEXT, 1};
    tbl[1] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
    t.publish(1);
    // desc[0] carries the header's first half and chains to the table descriptor.
    c.desc[0] = vring_desc{hoff, 8, VRING_DESC_F_NEXT, 1};

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &w));
    EXPECT_EQ(1u, w);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
}

// §2.7.5.3.1: "The driver MUST NOT set the VIRTQ_DESC_F_INDIRECT flag within an
// indirect descriptor (ie. only one table per descriptor)." That is a DRIVER
// requirement -- §2.7.5.3.2 gives the device no matching MUST -- so refusing is our
// own strictness, and the reason is the bound: nesting turns one step budget into a
// product of budgets, with the depth the peer's.
//
// All three assertions are needed. The status code alone would also pass an
// implementation that walked one level of nesting and then failed for an unrelated
// reason; `w == 0` and the untouched sentinel are what place the refusal DURING the
// walk rather than after it.
TEST_F(ChainFixture, a_nested_indirect_table_is_refused) {
    GuestChain c;
    size_t hoff = put_table_header(c, VIRTIO_BLK_T_IN);
    size_t doff = c.place(nullptr, 512);
    size_t inner = c.place(nullptr, 512);
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;

    // The nested table, built as a real one so that an implementation which allowed
    // the nesting would have something to walk and would go on to serve.
    IndirectTable inner_t;
    inner_t.c = &c;
    vring_desc* it = inner_t.begin(2, 2);
    it[0] = vring_desc{inner, 512, (uint16_t) (VRING_DESC_F_WRITE | VRING_DESC_F_NEXT), 1};
    it[1] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};

    IndirectTable t;
    t.c = &c;
    vring_desc* tbl = t.begin(3, 3);
    tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
    tbl[1] = vring_desc{inner_t.off, 2 * (uint32_t) sizeof(vring_desc),
                        (uint16_t) (VRING_DESC_F_INDIRECT | VRING_DESC_F_NEXT), 2};
    tbl[2] = vring_desc{doff, 512, VRING_DESC_F_WRITE, 0};
    t.publish(0);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &w));
    EXPECT_EQ(0u, w);
    EXPECT_EQ(SENTINEL, *c.at(st));
}

// The table's length is a second peer-supplied integer and the only one that names a
// whole array rather than one buffer, so it has to divide evenly. Flooring the
// division and ignoring the tail would SERVE this request, and the tail it ignored is
// not ours: on a transport whose regions are the whole of guest memory those eight
// bytes belong to somebody else's request, and the containment predicate a translate
// runs has nothing to say about a length we rounded down ourselves.
TEST_F(ChainFixture, a_table_length_that_is_not_a_whole_number_of_entries_is_refused) {
    GuestChain c;
    size_t hoff = put_table_header(c, VIRTIO_BLK_T_FLUSH);
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;

    IndirectTable t;
    t.c = &c;
    vring_desc* tbl = t.begin(3, 2);
    tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
    tbl[1] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
    // Two entries and eight bytes: a floor would read it as two and serve.
    t.publish(0, 0, 2 * (uint32_t) sizeof(vring_desc) + 8);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &w));
    EXPECT_EQ(0u, w);
    EXPECT_EQ(SENTINEL, *c.at(st));
}

// A zero-length table is refused BEFORE the table is translated. The status code is
// not the discriminator here -- with no length check the entry count is 0, the inner
// walk takes no step, the table never reports an end, and the circular-table refusal
// answers IOERR too. What differs is the translate count: an implementation that
// substituted one byte for a zero length the way it does for an ordinary buffer would
// pay a mapping for a table it was about to refuse, and on a transport whose translate
// is an ioctl that is a syscall for nothing.
TEST_F(ChainFixture, a_zero_length_table_is_refused_before_any_entry_is_translated) {
    DirectedGuest g;
    IndirectTable t;
    t.c = &g.c;
    // begin(1, 0) declares zero entries, and publish() with no declared_len uses
    // n * 16, which is the zero length under test.
    t.begin(1, 0);
    t.publish(0);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve_directed(g, &w));
    EXPECT_EQ(0u, w);
    ASSERT_EQ(0u, g.asked_addr.size()) << "a table refused on its length was mapped first";
}

// The entry-count bound, and the case the plan is most likely to get wrong if it is
// built the obvious way. Two things have to hold for it to mean anything:
//
//   The entries must be CHAINED. A region of zeroes is not a long table -- flags == 0
//   means no NEXT, the walk ends after one entry, and both a capped and an uncapped
//   engine pay exactly one translate. chain_zero_length gives every entry a NEXT and
//   the last one none, so an uncapped walk pays one translate per entry.
//
//   The status code is not the discriminator. Uncapped, 65 zero-length entries leave
//   the readable stream empty, the header gather-copy fails, and the answer is IOERR
//   anyway. What differs is the translate count: 0 against 66.
TEST_F(ChainFixture, an_absurd_table_is_refused_before_it_is_walked) {
    DirectedGuest g;
    IndirectTable t;
    t.c = &g.c;
    const uint32_t ENTRIES = TABLE_ENTRIES_MAX + 1;
    vring_desc* tbl = t.begin(ENTRIES, ENTRIES);
    IndirectTable::chain_zero_length(tbl, ENTRIES);
    t.publish(0);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve_directed(g, &w));
    EXPECT_EQ(0u, w);
    ASSERT_EQ(0u, g.asked_addr.size())
        << "an over-cap table was mapped and walked instead of refused on its count";
}

// The other end of the same bound, and the one that keeps the case above honest. A
// table of exactly the cap is a legal request and must be served: with only the
// refusal case, tightening the bound to 63 stays green while every maximum-size
// request a real guest builds -- which is exactly 64 descriptors, its two framing
// ones included -- is refused.
TEST_F(ChainFixture, a_table_of_exactly_the_entry_cap_is_served) {
    constexpr uint32_t DATA_DESCS = TABLE_ENTRIES_MAX - 2;   // a header and a status
    constexpr size_t DATA_BYTES = (size_t) DATA_DESCS * 512;
    GuestChain c;
    size_t hoff = put_table_header(c, VIRTIO_BLK_T_IN);
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;

    // Seed the image so "the scatter list was the destination" is witnessed in bytes.
    std::vector<uint8_t> src(DATA_BYTES);
    fill_pattern(src.data(), src.size(), 0x5a);
    ASSERT_EQ((ssize_t) DATA_BYTES,
              img.file->pwrite(src.data(), src.size(), (off_t) (CHAIN_SECTOR << 9)));

    IndirectTable t;
    t.c = &c;
    vring_desc* tbl = t.begin(TABLE_ENTRIES_MAX, TABLE_ENTRIES_MAX);
    tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
    for (uint32_t i = 0; i < DATA_DESCS; i++) {
        size_t off = c.place(nullptr, 512);
        tbl[1 + i] = vring_desc{off, 512,
                                (uint16_t) (VRING_DESC_F_WRITE | VRING_DESC_F_NEXT),
                                (uint16_t) (i + 2)};
    }
    tbl[TABLE_ENTRIES_MAX - 1] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
    t.publish(0);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &w));
    EXPECT_EQ((uint32_t) DATA_BYTES + 1, w);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    // One gather over the guest's own buffers, in table order.
    size_t got = 0;
    for (uint32_t i = 0; i < DATA_DESCS; i++) {
        size_t off = (size_t) tbl[1 + i].addr;
        EXPECT_EQ(0, memcmp(src.data() + got, c.at(off), 512)) << "payload " << i;
        got += 512;
    }
}

// The table is translated read-only, and first. Read-only because the walk is about
// to READ it, and §2.7.5.3.2 requires the device to ignore the write-only flag in the
// descriptor that names a table; first because every entry's address comes out of it,
// so nothing else can be translated before it. Both halves are invisible without a
// translate that records what it was asked for -- the same reason the existing
// directed cases exist, and the same failure they were added for: a translate handed a
// constant passes every other case in this suite.
//
// The request has to be PURELY indirect. In a mixed chain the first translate is for
// the ring's own first descriptor and says nothing about the table.
TEST_F(ChainFixture, the_table_is_translated_read_only_and_first) {
    DirectedGuest g;
    size_t hoff = put_table_header(g.c, VIRTIO_BLK_T_IN);
    size_t doff = g.c.place(nullptr, 512);
    size_t st = g.c.place(nullptr, 1);
    *g.c.at(st) = SENTINEL;

    IndirectTable t;
    t.c = &g.c;
    vring_desc* tbl = t.begin(3, 3);
    tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
    tbl[1] = vring_desc{doff, 512, (uint16_t) (VRING_DESC_F_WRITE | VRING_DESC_F_NEXT), 2};
    tbl[2] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
    // Deliberately also WRITE, which §2.7.5.3.2 tells the device to ignore.
    t.publish(0, VRING_DESC_F_WRITE);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve_directed(g, &w));
    ASSERT_EQ(4u, g.asked_addr.size());
    EXPECT_EQ((uint64_t) t.off, g.asked_addr[0]);
    EXPECT_EQ(0, g.asked_writable[0]) << "the table was mapped writable";
    // ... and then each entry, in table order, with the direction its own flag gave.
    EXPECT_EQ((uint64_t) hoff, g.asked_addr[1]);
    EXPECT_EQ(0, g.asked_writable[1]);
    EXPECT_EQ((uint64_t) doff, g.asked_addr[2]);
    EXPECT_EQ(1, g.asked_writable[2]);
    EXPECT_EQ((uint64_t) st, g.asked_addr[3]);
    EXPECT_EQ(1, g.asked_writable[3]);
}

// The length re-read inside a TABLE walk, which is a second walk with its own copy of
// each entry and its own translate per entry. Every expectation is the direct chain's,
// with the asked-length index shifted by the table's own translate -- which comes
// first, as the case above pins.
TEST_F(ChainFixture, a_table_entry_lengthened_while_it_is_translated_does_not_lengthen_the_request) {
    RacingGuest g;
    size_t hoff = put_table_header(g.c, VIRTIO_BLK_T_IN);
    size_t doff = g.c.place(nullptr, 512);   // the READ's destination
    memset(g.c.at(doff), SENTINEL, 512);
    size_t voff = g.c.place(nullptr, 512);   // guest memory just past that buffer
    memset(g.c.at(voff), SENTINEL, 512);
    size_t st = g.c.place(nullptr, 1);
    *g.c.at(st) = SENTINEL;

    IndirectTable t;
    t.c = &g.c;
    vring_desc* tbl = t.begin(3, 3);
    tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
    tbl[1] = vring_desc{doff, 512, (uint16_t) (VRING_DESC_F_WRITE | VRING_DESC_F_NEXT), 2};
    tbl[2] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
    t.publish(0);

    std::vector<uint8_t> src(1024);
    fill_pattern(src.data(), src.size(), 0x77);
    ASSERT_EQ((ssize_t) 1024,
              img.file->pwrite(src.data(), src.size(), (off_t) (CHAIN_SECTOR << 9)));
    ScatterProbe backend(img.file);

    g.mutate_addr = doff;
    g.slot = &tbl[1];   // the table is guest memory too, and this is where it lives
    g.new_len = 1024;

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve_racing(g, &w, &backend));
    EXPECT_EQ(1, g.mutations) << "the rewrite never landed, so this case proved nothing";

    ASSERT_EQ(4u, g.asked_len.size());   // the table itself, then its three entries
    EXPECT_EQ(512u, g.asked_len[2]);
    ASSERT_EQ(1u, backend.calls.size());
    ASSERT_EQ(1, backend.calls[0].iovcnt);
    EXPECT_EQ(g.asked_len[2], backend.calls[0].len[0])
        << "the scatter list carries a length translate never validated";

    std::vector<uint8_t> victim(512, SENTINEL);
    EXPECT_EQ(0, memcmp(victim.data(), g.c.at(voff), victim.size()))
        << "the backend wrote past the buffer its table entry named";
    EXPECT_EQ(513u, w);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *g.c.at(st));
}

// The table descriptor carries NO data. Its len bytes ARE the table, so pushing them
// into a stream would hand the descriptor array to pwritev as a WRITE's payload --
// the bytes the guest read back would be our own view of its request. §2.7.5.3.2's
// third MUST, that the device ignore the write-only flag in the descriptor naming a
// table, is pinned by the same case: the flag is set here on purpose.
//
// The image is read back rather than inferred from the status, because the failure
// being guarded against is bytes landing in the backend.
TEST_F(ChainFixture, the_table_descriptor_itself_carries_no_data) {
    GuestChain c;
    size_t hoff = put_table_header(c, VIRTIO_BLK_T_OUT);
    std::vector<uint8_t> data(512);
    fill_pattern(data.data(), data.size(), 0x77);
    size_t doff = c.place(data.data(), data.size());
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;

    IndirectTable t;
    t.c = &c;
    vring_desc* tbl = t.begin(3, 3);
    tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
    tbl[1] = vring_desc{doff, 512, VRING_DESC_F_NEXT, 2};
    tbl[2] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
    t.publish(0, VRING_DESC_F_WRITE);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &w));
    EXPECT_EQ(1u, w);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    std::vector<uint8_t> back(512);
    ASSERT_EQ((ssize_t) 512, img.file->pread(back.data(), back.size(), (off_t) (CHAIN_SECTOR << 9)));
    EXPECT_EQ(0, memcmp(data.data(), back.data(), data.size()))
        << "the backend holds something other than the payload the table named";
}

// §2.7.5.3.1: "A driver MUST NOT set both VIRTQ_DESC_F_INDIRECT and VIRTQ_DESC_F_NEXT
// in flags." Refused rather than served with the NEXT ignored, and the reason is not
// tidiness: ignoring it silently drops whatever the driver chained behind the table,
// and a chain whose tail is missing is SERVED as a shorter request. If the dropped
// tail was the status, the writable stream's last byte is a DATA byte and the request
// completes one byte short of what was asked for.
//
// Honest note on the third assertion: the sentinel survives under both the correct
// code (refused during the walk, so nothing is written) and the ignoring one (no
// status descriptor is ever reached), so it does not discriminate here. The status
// code does.
TEST_F(ChainFixture, an_indirect_descriptor_chained_with_next_is_refused) {
    GuestChain c;
    size_t hoff = put_table_header(c, VIRTIO_BLK_T_OUT);
    std::vector<uint8_t> data(512, 0);
    size_t doff = c.place(data.data(), data.size());
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;

    IndirectTable t;
    t.c = &c;
    vring_desc* tbl = t.begin(2, 2);
    tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
    tbl[1] = vring_desc{doff, 512, 0, 0};   // no status in the table
    t.publish(0, VRING_DESC_F_NEXT);
    c.desc[0].next = 1;
    c.desc[1] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR, serve(c, false, &w));
    EXPECT_EQ(0u, w);
    EXPECT_EQ(SENTINEL, *c.at(st));
}

// Indirect is a LAYOUT, not a semantics: the same request built both ways has to
// leave the same four things behind. Compared as one value, because each of the four
// on its own can be satisfied by a different bug -- `written` alone hides a wrong
// used length behind a right byte count, and the image alone hides a status that was
// never written.
//
// The two forms land at two different image offsets and place their own buffers, so
// nothing is reused: a shared buffer would turn "the image matches" into an assertion
// about the sharing.
TEST_F(ChainFixture, an_indirect_read_and_write_report_the_same_result_as_the_direct_form) {
    constexpr uint64_t SEC_DIRECT = CHAIN_SECTOR;
    constexpr uint64_t SEC_TABLE = CHAIN_SECTOR + 8;
    std::vector<uint8_t> data(512);
    fill_pattern(data.data(), data.size(), 0x29);

    // Seed both offsets so a READ has something to fetch and a WRITE has something to
    // be distinguishable from.
    for (uint64_t s : {SEC_DIRECT, SEC_TABLE})
        ASSERT_EQ((ssize_t) 512, img.file->pwrite(data.data(), data.size(), (off_t) (s << 9)));

    auto run = [&](uint32_t type, bool indirect, uint64_t sector) {
        GuestChain c;
        size_t hoff, doff, st;
        // A READ's data buffer is device-writable, a WRITE's is not; both chain on to
        // the status. Computed once because the braced init below would narrow it.
        const uint16_t dflags = (uint16_t) ((type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0) |
                                            VRING_DESC_F_NEXT);
        if (indirect) {
            IndirectTable t;
            t.c = &c;
            hoff = put_table_header(c, type, sector);
            doff = c.place(nullptr, 512);
            if (type == VIRTIO_BLK_T_OUT)
                memcpy(c.at(doff), data.data(), data.size());
            st = c.place(nullptr, 1);
            *c.at(st) = SENTINEL;
            vring_desc* tbl = t.begin(3, 3);
            tbl[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
            tbl[1] = vring_desc{doff, 512, dflags, 2};
            tbl[2] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
            t.publish(0);
        } else {
            hoff = put_table_header(c, type, sector);
            doff = c.place(nullptr, 512);
            if (type == VIRTIO_BLK_T_OUT)
                memcpy(c.at(doff), data.data(), data.size());
            st = c.place(nullptr, 1);
            *c.at(st) = SENTINEL;
            c.desc[0] = vring_desc{hoff, sizeof(virtio_blk_outhdr), VRING_DESC_F_NEXT, 1};
            c.desc[1] = vring_desc{doff, 512, dflags, 2};
            c.desc[2] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
        }
        ChainOutcome o;
        o.st = serve(c, false, &o.w);
        o.status_byte = *c.at(st);
        o.image.resize(512);
        EXPECT_EQ((ssize_t) 512,
                  img.file->pread(o.image.data(), o.image.size(), (off_t) (sector << 9)));
        return o;
    };

    for (uint32_t type : {VIRTIO_BLK_T_IN, VIRTIO_BLK_T_OUT}) {
        ChainOutcome direct = run(type, false, SEC_DIRECT);
        ChainOutcome table = run(type, true, SEC_TABLE);
        const char* which = (type == VIRTIO_BLK_T_IN) ? "READ" : "WRITE";
        ASSERT_EQ(VIRTIO_BLK_S_OK, direct.st) << "the direct form broke, so this compares nothing";
        EXPECT_EQ(direct.st, table.st) << which;
        EXPECT_EQ(direct.w, table.w) << which;
        EXPECT_EQ(direct.status_byte, table.status_byte) << which;
        EXPECT_EQ(0, memcmp(direct.image.data(), table.image.data(), direct.image.size())) << which;
        if (type == VIRTIO_BLK_T_OUT) {
            EXPECT_EQ(0, memcmp(data.data(), table.image.data(), data.size())) << which;
        }
    }
}

// A header split across two TABLE entries. The direct-chain version of this shape is
// already pinned, and the reason it matters is the same: a whole-descriptor parser
// drops the second half and still answers OK. Split ring requires no read/write
// ordering inside a table either, so nothing may assume one entry per role or that the
// readable ones come first.
TEST_F(ChainFixture, a_header_split_across_two_table_entries_is_still_served) {
    GuestChain c;
    size_t hoff = put_table_header(c, VIRTIO_BLK_T_OUT);
    std::vector<uint8_t> data(512);
    fill_pattern(data.data(), data.size(), 0x44);
    size_t doff = c.place(data.data(), data.size());
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;

    IndirectTable t;
    t.c = &c;
    vring_desc* tbl = t.begin(4, 4);
    tbl[0] = vring_desc{hoff, 8, VRING_DESC_F_NEXT, 1};
    tbl[1] = vring_desc{hoff + 8, sizeof(virtio_blk_outhdr) - 8, VRING_DESC_F_NEXT, 2};
    tbl[2] = vring_desc{doff, 512, VRING_DESC_F_NEXT, 3};
    tbl[3] = vring_desc{st, 1, VRING_DESC_F_WRITE, 0};
    t.publish(0);

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_OK, serve(c, false, &w));
    EXPECT_EQ(1u, w);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *c.at(st));
    std::vector<uint8_t> back(512);
    ASSERT_EQ((ssize_t) 512, img.file->pread(back.data(), back.size(), (off_t) (CHAIN_SECTOR << 9)));
    EXPECT_EQ(0, memcmp(data.data(), back.data(), data.size()));
}

// A mixed chain spends ONE scatter-list budget, not two. The ring descriptors and the
// table's entries push into the same two streams, so 32 of the first and 40 of the
// second overflow a 64-deep array even though neither count is over it alone. That
// this shape exists at all is a consequence of the layout §2.7.5.3.2 requires the
// device to handle; under an exclusive reading of it there would be nothing to test.
//
// No mutant of its own: the direct-chain version of the same refusal is already
// pinned one descriptor past the deepest, and what this adds is that the budget is
// shared, which no single-line change can undo.
TEST_F(ChainFixture, a_mixed_chain_that_overflows_the_shared_scatter_list_is_refused) {
    constexpr uint32_t RING_DESCS = 32;
    constexpr uint32_t TBL_DESCS = 40;
    GuestChain c(RING_DESCS + 1);
    // One buffer reused by every descriptor: this case is about the count, and a
    // request that overflows is refused before any byte is gathered.
    size_t shared = c.place(nullptr, 512);
    size_t st = c.place(nullptr, 1);
    *c.at(st) = SENTINEL;
    for (uint32_t i = 0; i < RING_DESCS; i++)
        c.desc[i] = vring_desc{shared, 512, (uint16_t) (VRING_DESC_F_WRITE | VRING_DESC_F_NEXT),
                               (uint16_t) (i + 1)};

    IndirectTable t;
    t.c = &c;
    vring_desc* tbl = t.begin(TBL_DESCS, TBL_DESCS);
    for (uint32_t i = 0; i < TBL_DESCS; i++)
        tbl[i] = vring_desc{shared, 512,
                            (uint16_t) (VRING_DESC_F_WRITE |
                                        (i + 1 < TBL_DESCS ? VRING_DESC_F_NEXT : 0)),
                            (uint16_t) (i + 1)};
    c.desc[RING_DESCS] = vring_desc{t.off, TBL_DESCS * (uint32_t) sizeof(vring_desc),
                                    VRING_DESC_F_INDIRECT, 0};

    uint32_t w = 0;
    EXPECT_EQ(VIRTIO_BLK_S_IOERR,
              serve(c, false, &w, false, nullptr, RING_DESCS + 1));
    EXPECT_EQ(0u, w);
    EXPECT_EQ(SENTINEL, *c.at(st));
}

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

    // Wait out any request coroutine this case left running, while the memory it
    // holds is still alive. Every case here drives handle_req() directly and so
    // creates none of its own -- but a completion can admit a successor
    // (redispatch_backlog), and `run` being false in this fixture is the ONLY thing
    // that stops it. A case that returned with a coroutine still in flight would
    // free three std::vectors and a VirtQueueServer out from under it, and the
    // damage lands in whichever case runs NEXT rather than in the one that caused
    // it. That is not hypothetical: dropping `run` from the redispatch guard reddens
    // exactly the three cases below that leave a backlog behind, but only when each
    // runs in its own process -- in one process the first of them corrupts the rest
    // and the suite reports a different set, including a case that detects nothing.
    void TearDown() override {
        for (int i = 0; i < 3000 && srv.in_flight.load(); i++)
            photon::thread_usleep(1000);
        EXPECT_EQ(0u, srv.in_flight.load());
    }
};

// translate, except that the call resolving the status descriptor renegotiates
// the ring first -- by which point serve_chain is already running, which is the
// only place a test can put the change between handle_req's two checks without
// a backend that yields. Note that renegotiate() republishes the SAME three
// pointers here and still retires the request: comparing ring addresses would
// not have caught this, and a driver that renegotiates onto the same buffers is
// not a hypothetical.
void* gen_translate_thunk(void* a, uint64_t addr, size_t len, bool writable) {
    auto* f = (GenFixture*) a;
    if (++f->translate_calls == 2)
        f->renegotiate();
    return chain_translate(&f->guest, addr, len, writable);
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

// ---------------------------------------------------------------------------
// the in-flight cap
//
// BlkConfig::queue_depth is documented as a per-queue in-flight limit. Both
// virtio transports hand it to the engine, and dispatch_avail's cap is the lesser
// of it and the ring size. Driven here rather than through a transport because the
// cap is the engine's: a transport can only get the plumbing wrong, and the shape
// of the defect that matters -- a device admitting more requests than its caller
// allowed -- is a property of this loop.
//
// There is no loop() coroutine and no kickfd in this fixture. That is deliberate
// and it is what makes the second case a count rather than a timing guess: with no
// external event source, anything that reaches the backend after the first batch
// got there because a COMPLETION put it there.
// ---------------------------------------------------------------------------
class DepthFixture : public ::testing::Test {
public:
    test::TestImage img;
    MemVring ring{RING_NUM};
    GuestChain guest;
    VirtQueueServer srv;
    // Created in SetUp rather than declared with an initializer: RecordingFile
    // takes the backend handle in its constructor and TestImage only has one once
    // create() has run. Destroyed in TearDown, before `img` goes.
    std::unique_ptr<test::RecordingFile> rf;
    size_t soff = 0;
    // `ready` as a state this fixture owns instead of the always-true thunk the
    // other fixtures bind: one case below has to clear it while requests are parked
    // inside the backend, which is what a transport does when its frontend pauses a
    // queue, and the engine's answer to that is what the case measures.
    bool ready_state = true;
    static bool ready_thunk(void* a) { return ((DepthFixture*)a)->ready_state; }
    // Off by default, so the seven cases that share this SetUp are untouched. E10
    // turns it on through IndirectDepthFixture below, which builds the SAME request
    // with its buffers named by a table instead of by three ring descriptors.
    bool indirect = false;

    // Under the ring size, so no avail entry is overwritten by the publish loop,
    // and far enough under it that a cap reading `num` instead of `queue_depth`
    // cannot be mistaken for one reading the depth.
    static constexpr uint32_t DEPTH = 2;
    static constexpr int PUBLISHED = 6;

    void SetUp() override {
        ASSERT_EQ(0, img.create("/tmp/photon-blk-vq-depth.img", CHAIN_CAPACITY));
        rf.reset(new test::RecordingFile(img.file));
        rf->gated = true;
        srv.backend = rf.get();
        srv.capacity.store(CHAIN_CAPACITY, std::memory_order_relaxed);
        srv.serial = CHAIN_SERIAL;
        srv.tag = "vq-depth";
        srv.hooks.ready.bind(this, &ready_thunk);
        srv.hooks.notify.bind(nullptr, &notify_thunk);
        srv.hooks.translate.bind(&guest, &chain_translate);
        srv.event_idx.store(false, std::memory_order_relaxed);
        srv.notify_valid.store(true, std::memory_order_relaxed);
        // No loop coroutine runs here, but this is still what a serving queue has,
        // and the completion-side redispatch reads it.
        srv.run.store(true, std::memory_order_relaxed);
        srv.queue_depth = DEPTH;

        // One READ chain at head 0, and every avail entry names it -- the same
        // device the vhost-user suite's cap cases use: the entries are
        // indistinguishable, so sharing one chain costs nothing and keeps the
        // descriptor table at three. A read is also exactly one recorded IO, which
        // is what lets `arrivals` be read as a request count; a write in
        // write-through mode would record twice, once for the write and once for
        // the sync that persists it.
        virtio_blk_outhdr hdr{};
        hdr.type = VIRTIO_BLK_T_IN;
        hdr.sector = CHAIN_SECTOR;
        size_t hoff = guest.place(&hdr, sizeof(hdr));
        size_t doff = guest.place(nullptr, 512);
        size_t toff = guest.place(nullptr, 3 * sizeof(vring_desc));
        soff = guest.place(nullptr, 1);
        *guest.at(soff) = SENTINEL;
        if (!indirect) {
            ring.desc[0].addr = hoff;
            ring.desc[0].len = sizeof(hdr);
            ring.desc[0].flags = VRING_DESC_F_NEXT;
            ring.desc[0].next = 1;
            ring.desc[1].addr = doff;
            ring.desc[1].len = 512;
            ring.desc[1].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
            ring.desc[1].next = 2;
            ring.desc[2].addr = soff;
            ring.desc[2].len = 1;
            ring.desc[2].flags = VRING_DESC_F_WRITE;
            ring.desc[2].next = 0;
        } else {
            // The table lives in the same guest memory, so the translate hook is
            // unchanged; what changes is that ONE ring descriptor names the three
            // buffers. The feature has to be marked negotiated as well -- a chain
            // carrying a table is a chain whose driver offered bit 28, and an engine
            // that walked one without that would be the defect M2 exists to catch.
            auto* tbl = (vring_desc*) guest.at(toff);
            tbl[0] = vring_desc{hoff, sizeof(hdr), VRING_DESC_F_NEXT, 1};
            tbl[1] = vring_desc{doff, 512,
                                (uint16_t) (VRING_DESC_F_WRITE | VRING_DESC_F_NEXT), 2};
            tbl[2] = vring_desc{soff, 1, VRING_DESC_F_WRITE, 0};
            ring.desc[0].addr = toff;
            ring.desc[0].len = 3 * (uint32_t) sizeof(vring_desc);
            ring.desc[0].flags = VRING_DESC_F_INDIRECT;
            ring.desc[0].next = 0;
            srv.indirect_desc.store(true, std::memory_order_relaxed);
        }
        for (int i = 0; i < PUBLISHED; i++)
            ring.avail->ring[i] = 0;
        __atomic_store_n(&ring.avail->idx, (uint16_t) PUBLISHED, __ATOMIC_RELEASE);
        srv.set_ring(ring.desc, ring.avail, ring.used, RING_NUM);
    }

    void TearDown() override {
        // Nothing may stay parked in the gate: a coroutine left there holds
        // references to rf and to img, and both go away with this fixture.
        rf->release_gate(1024);
        settle_until([&] { return srv.in_flight.load() == 0; });
        srv.run.store(false, std::memory_order_relaxed);
        rf.reset();
    }

    // Yield until `cond` holds, or the budget runs out and the caller's assertion
    // reports the failure. Bounded everywhere: a request that never arrives has to
    // fail a case, not hang the suite.
    template <typename F>
    bool settle_until(F cond, int tries = 3000) {
        for (int i = 0; i < tries && !cond(); i++)
            photon::thread_usleep(1000);
        return cond();
    }

    // Offer `n` chains and let the engine take what its cap allows. SetUp already
    // laid every avail entry out as head 0, so all this moves is the index -- with a
    // release store, because dispatch_avail reads it through vring_avail_idx's acquire
    // load. Writing a SMALLER index than SetUp's is legal here and only here: the
    // cases that use this have dispatched nothing yet, so last_avail is still 0 and
    // the engine sees the index only ever move forward.
    void publish(uint32_t n) {
        __atomic_store_n(&ring.avail->idx, (uint16_t) n, __ATOMIC_RELEASE);
        srv.dispatch_avail();
    }
};

// DepthFixture with the request published as one indirect table instead of three ring
// descriptors. Everything the derived case asserts is unchanged, which is the point:
// the cap counts heads, and a table is still one head.
class IndirectDepthFixture : public DepthFixture {
public:
    void SetUp() override {
        indirect = true;
        DepthFixture::SetUp();
    }
};

// The shape measured in review: a device configured for a depth of 1 admitted four
// blocked requests, all four into the backend at once, because the only bound the
// engine had was `num` -- which for vhost-user is the frontend's SET_VRING_NUM and
// has nothing to do with what the caller asked for.
//
// The gate makes this a state rather than a race: every admitted request parks
// inside the backend, and nothing completes while the gate is shut, so the count
// cannot be read too early. Only too late, which the budget covers.
TEST_F(DepthFixture, dispatch_stops_at_the_configured_depth_not_at_the_ring_size) {
    srv.dispatch_avail();

    ASSERT_TRUE(settle_until([&] { return rf->arrivals.load() >= DEPTH; }));
    EXPECT_EQ((uint64_t) DEPTH, rf->arrivals.load());
    EXPECT_EQ((uint64_t) DEPTH, srv.in_flight.load());
    // Held back means exactly that: last_avail did not advance over them, so the
    // four the cap refused are still the peer's to keep, and nothing completed.
    EXPECT_EQ((uint16_t) DEPTH, srv.last_avail);
    EXPECT_EQ(0, vring_used_idx(ring.used));
    EXPECT_EQ(SENTINEL, *guest.at(soff));

    // And they are still served. A cap that stranded the overflow would be a worse
    // defect than no cap at all -- which is what dispatch_cap_recovery asserts from
    // the transport side at the ring-size cap.
    rf->release_gate(1024);
    ASSERT_TRUE(settle_until([&] { return srv.in_flight.load() == 0; }));
    EXPECT_EQ((uint64_t) PUBLISHED, rf->arrivals.load());
    EXPECT_EQ((uint16_t) PUBLISHED, vring_used_idx(ring.used));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *guest.at(soff));
}

// D1, and the reason it costs no code: an indirect request is ONE head, so it is one
// in_flight, and every number the case above reads is the number this one reads too.
// The expectations are copied from it -- DEPTH, PUBLISHED and all four counts -- and
// their being UNCHANGED is the discriminator. An engine that accounted per table entry
// would reach the cap during the first dispatch, and last_avail would not read back as
// DEPTH.
//
// No mutant, and that is honest bookkeeping rather than an omission: per-entry
// accounting cannot be a one-line change, because the entry count is not known until
// the request coroutine translates the table, and dispatch_avail is yield-free by
// contract. Moving the translate into dispatch to learn the count would break the
// single re-check loop() relies on.
TEST_F(IndirectDepthFixture, an_indirect_request_counts_as_one_in_flight) {
    srv.dispatch_avail();

    ASSERT_TRUE(settle_until([&] { return rf->arrivals.load() >= DEPTH; }));
    EXPECT_EQ((uint64_t) DEPTH, rf->arrivals.load());
    EXPECT_EQ((uint64_t) DEPTH, srv.in_flight.load());
    EXPECT_EQ((uint16_t) DEPTH, srv.last_avail);
    EXPECT_EQ(0, vring_used_idx(ring.used));
    EXPECT_EQ(SENTINEL, *guest.at(soff));

    rf->release_gate(1024);
    ASSERT_TRUE(settle_until([&] { return srv.in_flight.load() == 0; }));
    EXPECT_EQ((uint64_t) PUBLISHED, rf->arrivals.load());
    EXPECT_EQ((uint16_t) PUBLISHED, vring_used_idx(ring.used));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *guest.at(soff));
}

// The cap under a load that MOVES. The case above samples in_flight once, at the
// cap, and one number cannot tell three devices apart: one that admits exactly the
// configured depth, one that admitted more earlier and has since drained, and one
// whose count never returns to zero because a slot leaked. Following the offered
// load up, holding it there, then taking it away separates all three -- the count
// has to TRACK what is offered while the gate holds every request, and return to its
// baseline once the gate lets them finish.
//
// The two halves discriminate different mutations, and neither covers the other:
//   the "past the cap" block   an engine that ADMITS too much. `>` in place of
//                              dispatch_avail's `>=`, or a cap reading `num` instead
//                              of the lesser of `num` and `queue_depth`, both put
//                              in_flight and last_avail above DEPTH here.
//   the final block            an engine that PROGRESSES too little. Deleting
//                              handle_req's DEFER(in_flight--) leaves the count at
//                              the cap forever, so the redispatch a completion
//                              triggers keeps finding the cap binding and the four
//                              held-back chains are never admitted; deleting
//                              DEFER(redispatch_backlog()) drains to zero instead
//                              and still leaves them unserved. Both fail the return
//                              to baseline, the first by never reaching it.
// What does NOT discriminate, and why it is here anyway: `arrivals` inside the "past
// the cap" block. dispatch_avail only queues a coroutine -- it never runs one, since
// neither it nor publish() yields -- so an over-admitting cap has not reached the
// backend yet at the point that count is read, and it reads DEPTH either way. The
// two counts beside it are read at the same instant and do move. arrivals is kept
// because it is the same fact one step later, and a case that asserted the internal
// counters only would not say anything about IO.
TEST_F(DepthFixture, in_flight_follows_the_offered_load_and_returns_to_its_baseline) {
    EXPECT_EQ(0u, srv.in_flight.load());

    // One chain offered, one admitted. Below the cap, so the cap is not what holds
    // this one back and the count has to move with the load rather than sit at it.
    publish(1);
    ASSERT_TRUE(settle_until([&] { return rf->arrivals.load() >= 1; }));
    EXPECT_EQ(1u, srv.in_flight.load());
    EXPECT_EQ((uint16_t) 1, srv.last_avail);

    // Up to the cap. Both requests are parked in the gate, so this is a state and
    // not a race: nothing can free a slot until the gate opens below.
    publish(DEPTH);
    ASSERT_TRUE(settle_until([&] { return rf->arrivals.load() >= DEPTH; }));
    EXPECT_EQ((uint64_t) DEPTH, rf->arrivals.load());
    EXPECT_EQ((uint64_t) DEPTH, srv.in_flight.load());

    // Past it. Everything here is synchronous: publish() and the extra dispatch both
    // run to completion without yielding (thread_create only queues), so the counts
    // are read at a point no coroutine can have moved them from.
    publish(PUBLISHED);
    EXPECT_EQ((uint64_t) DEPTH, rf->arrivals.load());
    EXPECT_EQ((uint64_t) DEPTH, srv.in_flight.load());
    EXPECT_EQ((uint16_t) DEPTH, srv.last_avail);
    // A second kick against a cap that is still binding is refused the same way,
    // which is what makes the cap a bound rather than a one-pass decision.
    srv.dispatch_avail();
    EXPECT_EQ((uint64_t) DEPTH, rf->arrivals.load());
    EXPECT_EQ((uint64_t) DEPTH, srv.in_flight.load());

    // The load goes away and the gate opens: every offered chain is served and the
    // count returns to where it started. in_flight can only reach 0 once the backlog
    // is exhausted -- handle_req's decrement and the redispatch that fills the freed
    // slot are in one DEFER chain with no yield between them -- so the counts below
    // are read at a settled point.
    rf->release_gate(1024);
    ASSERT_TRUE(settle_until([&] { return srv.in_flight.load() == 0; }));
    EXPECT_EQ(0u, srv.in_flight.load());
    EXPECT_EQ((uint64_t) PUBLISHED, rf->arrivals.load());
    EXPECT_EQ((uint16_t) PUBLISHED, vring_used_idx(ring.used));
    EXPECT_EQ((uint16_t) PUBLISHED, srv.last_avail);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *guest.at(soff));
}

// The cap's own recovery, and the reason honouring queue_depth is not a throughput
// trap. Chains the cap held back are not the peer's to re-announce: the driver
// already put them in the ring and already kicked, and with EVENT_IDX it will not
// kick again for a buffer whose index sits behind the avail_event we published.
// loop() would find them on its next KICK_FALLBACK_US re-read -- 5 ms later, every
// time, so a depth of 1 would hold the device to roughly 200 requests a second no
// matter how fast the backend is.
//
// So a completion frees its slot AND takes the next chain. poke_gate rather than
// release_gate is what keeps that observable: exactly one parked IO resumes, so
// exactly one slot frees, and the next arrival still parks. With the gate open the
// cap stops binding and every count below becomes a race.
TEST_F(DepthFixture, a_completion_admits_the_next_request_without_another_kick) {
    srv.dispatch_avail();
    ASSERT_TRUE(settle_until([&] { return rf->arrivals.load() >= DEPTH; }));
    ASSERT_EQ((uint64_t) DEPTH, rf->arrivals.load());

    rf->poke_gate(1);
    ASSERT_TRUE(settle_until([&] { return rf->arrivals.load() > DEPTH; }));
    EXPECT_EQ((uint64_t) DEPTH + 1, rf->arrivals.load());
    // Back AT the cap rather than below it: the slot that freed was filled, and the
    // request still parked from the first batch is the other one.
    EXPECT_EQ((uint64_t) DEPTH, srv.in_flight.load());
    EXPECT_EQ(1, vring_used_idx(ring.used));

    rf->release_gate(1024);
    ASSERT_TRUE(settle_until([&] { return srv.in_flight.load() == 0; }));
    EXPECT_EQ((uint64_t) PUBLISHED, rf->arrivals.load());
    EXPECT_EQ((uint16_t) PUBLISHED, vring_used_idx(ring.used));
}

// The other direction of the same guard. A teardown that did NOT ask to drain must
// leave the backlog stranded -- that is the documented handover, and the next
// daemon resumes from used->idx. So once `stopping` is set, a completion may free
// its slot but must not fill it again.
//
// Waiting on in_flight rather than sleeping makes this deterministic: the
// decrement and the redispatch are two guards in one DEFER chain with no yield
// between them, so a test coroutine that sees the count drop has already missed
// the redispatch that would have followed it.
TEST_F(DepthFixture, a_teardown_that_declined_to_drain_leaves_the_backlog_stranded) {
    srv.dispatch_avail();
    ASSERT_TRUE(settle_until([&] { return rf->arrivals.load() >= DEPTH; }));
    ASSERT_EQ((uint64_t) DEPTH, rf->arrivals.load());

    srv.stopping.store(true, std::memory_order_relaxed);
    rf->poke_gate(1);
    ASSERT_TRUE(settle_until([&] { return srv.in_flight.load() == DEPTH - 1; }));

    // The resumed request neither completed nor was replaced. It got as far as
    // handle_req's SECOND check, the one that runs after the backend IO returns,
    // and that is what makes this case differ from the stale-generation ones above:
    // those are declined by the first check, before serve_chain is ever entered, so
    // the guest's buffer is untouched. This one is declined with the status byte
    // ALREADY WRITTEN. Invisible to a conforming driver and stays so -- a driver may
    // only read a buffer once it sees that descriptor's head in the used ring, no
    // used element was published, so the descriptor is still ours and whoever serves
    // next writes the byte again.
    EXPECT_EQ((uint64_t) DEPTH, rf->arrivals.load());
    EXPECT_EQ(0, vring_used_idx(ring.used));
    EXPECT_EQ(VIRTIO_BLK_S_OK, *guest.at(soff));
    EXPECT_EQ((uint16_t) DEPTH, srv.last_avail);
}

// The redispatch reads the avail ring, so it needs the same proof the loop has that
// there is one to read. A transport clears the ring when a reset or an unmap retires
// it -- vhost-user on a memory-table change, vduse on a device reset -- and a
// request that was parked in the backend across that window completes into a queue
// whose ring is gone. `ready` goes false alongside it in both transports today, so
// the null test is what keeps the engine safe on its own terms rather than only as
// safe as the two ready hooks that happen to exist.
TEST_F(DepthFixture, a_ring_cleared_under_a_parked_request_is_not_dispatched_into) {
    srv.dispatch_avail();
    ASSERT_TRUE(settle_until([&] { return rf->arrivals.load() >= DEPTH; }));
    ASSERT_EQ((uint64_t) DEPTH, rf->arrivals.load());

    srv.clear_ring();
    rf->poke_gate(1);
    ASSERT_TRUE(settle_until([&] { return srv.in_flight.load() == DEPTH - 1; }));

    // Nothing admitted and nothing published. Reading the avail index of a cleared
    // ring is the part that would crash rather than merely fail, so this case's
    // first assertion is that it got here at all.
    EXPECT_EQ((uint64_t) DEPTH, rf->arrivals.load());
    EXPECT_EQ(0, vring_used_idx(ring.used));
    EXPECT_EQ((uint16_t) DEPTH, srv.last_avail);
}

// The complement of the case above, and the difference between the two is the whole
// of a pause. `ready` answers "may the loop take more work from this ring", which is
// what a transport clears when its frontend pauses a queue; the ring itself is still
// published, still mapped and still at the same generation. A request already
// dispatched holds a chain that was CONSUMED from the avail ring -- last_avail moved
// past it at dispatch, and no driver re-publishes a buffer it is still waiting on --
// so refusing that request's completion loses it outright: the backend IO lands and
// the guest is never told. That is why handle_req gates on `stopping` and the
// generation, the two facts that say the ring this request belongs to is GONE, and
// never on `ready`.
//
// Clearing the ring (above) and pausing the queue (here) must therefore stay
// distinguishable, and the counts below are what keep them so: a cleared ring
// publishes nothing, a paused one publishes everything it already took.
TEST_F(DepthFixture, a_paused_ring_still_receives_the_completions_it_owes) {
    srv.dispatch_avail();
    ASSERT_TRUE(settle_until([&] { return rf->arrivals.load() >= DEPTH; }));
    ASSERT_EQ((uint64_t) DEPTH, rf->arrivals.load());

    ready_state = false;      // the pause, as the engine sees it
    rf->release_gate(1024);
    ASSERT_TRUE(settle_until([&] { return srv.in_flight.load() == 0; }));

    // Both owed completions landed, and NEITHER admitted a successor: dispatching
    // more work is exactly what a pause does forbid, and redispatch_backlog is the
    // one completion-side path that consults `ready`. So the four chains still in
    // the avail ring stay there, which is what makes arrivals an equality here
    // rather than a lower bound.
    EXPECT_EQ((uint64_t) DEPTH, rf->arrivals.load());
    EXPECT_EQ((uint16_t) DEPTH, vring_used_idx(ring.used));
    EXPECT_EQ((uint16_t) DEPTH, srv.last_avail);
    EXPECT_EQ(VIRTIO_BLK_S_OK, *guest.at(soff));
}

// The same pause landing EARLIER: after dispatch_avail queued the coroutines but
// before either of them ran. handle_req gates twice -- once before it serves and
// once before it publishes -- and a pause can fall inside either window. The case
// above can only reach the second, because its requests are already parked inside
// the backend by the time `ready` is cleared; this one reaches the first, and both
// have to answer the same way or a pause would drop a request on nothing more than
// how far the scheduler had happened to get.
TEST_F(DepthFixture, a_pause_before_a_queued_request_runs_still_lets_it_complete) {
    srv.dispatch_avail();     // thread_create only queues: nothing has run yet
    ready_state = false;      // the pause, before either coroutine is scheduled
    rf->release_gate(1024);   // open, so they run straight through instead of parking
    ASSERT_TRUE(settle_until([&] { return srv.in_flight.load() == 0; }));

    EXPECT_EQ((uint64_t) DEPTH, rf->arrivals.load());
    EXPECT_EQ((uint16_t) DEPTH, vring_used_idx(ring.used));
    EXPECT_EQ((uint16_t) DEPTH, srv.last_avail);
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
