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

#include <atomic>
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
// which matters to a device, not to a struct's alignment.
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
                         std::memory_order_relaxed);
        __atomic_store_n(&r.used->idx, base, std::memory_order_relaxed);
        srv.used_idx = base;
        uint64_t before = g_notifications.load(std::memory_order_relaxed);

        bar.wait();
        // The driver's prescribed sequence: enable, barrier, re-read.
        __atomic_store_n(&r.avail->flags, (uint16_t)0, std::memory_order_release);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        bool driver_saw_it = __atomic_load_n(&r.used->idx, std::memory_order_acquire) != base;
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
                     std::memory_order_relaxed);
    EXPECT_FALSE(srv.should_notify(0));
    __atomic_store_n(&r.avail->flags, (uint16_t)0, std::memory_order_relaxed);
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
    __atomic_store_n(&r.avail->ring[RING_NUM], (uint16_t)0, std::memory_order_relaxed);
    srv.used_idx = 5;
    EXPECT_TRUE(srv.should_notify(5));
    EXPECT_TRUE(srv.notify_valid.load(std::memory_order_relaxed));
    // Same inputs, rule now spent: the arithmetic decides, and it says no.
    EXPECT_FALSE(srv.should_notify(5));
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
