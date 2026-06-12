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

#include <atomic>
#include <thread>
#include <vector>

#include <photon/common/alog.h>
#include <photon/common/lockfree_queue.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include "../../../test/gtest.h"

// Regression test for the RingChannel notification refactor.
//
// Failure mode being guarded:
//   When N producers fire a burst, the previous design issued one
//   `queue_sem.signal(1)` per push every time the consumer happened to be in
//   its `idler` window. The semaphore counter accumulated linearly with the
//   burst size, so after the burst the consumer would loop forever between
//   1ms busy-yield and a `wait()` that returned immediately, burning a full
//   core for as long as the accumulated counter took to drain.
//
// Verification strategy (deterministic, no wall-clock assumptions):
//   - Drive a real `RingChannel` directly, with `kProducers` std::thread
//     producers and a single std::thread consumer that owns its own photon
//     vCPU.
//   - Synchronize using a producer-side `received` counter and a sentinel
//     value to terminate the consumer. All "wait" operations are
//     event-driven (`std::thread::join`, atomic counter spin), never
//     wall-clock sleeps.
//   - After the consumer thread has fully joined, all RingChannel state is
//     quiescent and visible to the test thread. Assert the *invariant*
//     `notification_pending() <= idler_peak`. With a single consumer,
//     `idler_peak == 1`, so `pending` must remain ≤1 regardless of burst
//     size. Without the fix, `pending` (== `queue_sem.m_count`) would carry
//     leftover tokens proportional to how often the consumer dipped into
//     the idler window during the burst.
TEST(ring_channel, burst_drain_no_signal_accumulation) {
    using QT = LockfreeMPMCRingQueue<int, 4096>;
    photon::common::RingChannel<QT> ch(1024, 1024);

    constexpr int kProducers = 4;
    constexpr int kPerProducer = 25000;
    constexpr int kBurst = kProducers * kPerProducer;
    constexpr int kSentinel = 0;  // sentinel value to terminate consumer

    std::atomic<int> received{0};

    // Consumer owns its own photon vCPU, so it can be scheduled
    // independently of the producers and of this test thread.
    std::thread consumer([&] {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());
        for (;;) {
            int x = ch.recv();
            if (x == kSentinel) break;
            received.fetch_add(1, std::memory_order_acq_rel);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                // Producers are plain std::threads; ThreadPause yields the
                // OS thread on the (extremely unlikely) full-queue spin.
                ch.template send<ThreadPause>(/*non-zero*/ 1);
            }
        });
    }
    for (auto& t : producers) t.join();

    // Deterministic synchronization point: spin until the consumer has
    // drained exactly the burst. No wall-clock sleeps.
    while (received.load(std::memory_order_acquire) < kBurst) {
        std::this_thread::yield();
    }

    // Terminate consumer cleanly, then join. After join() returns, no other
    // thread can touch `ch`, so we can sample its state safely.
    ch.template send<ThreadPause>(kSentinel);
    consumer.join();

    auto pending = ch.notification_pending();
    LOG_INFO("after burst: received=`, notification_pending=`",
             received.load(), pending);

    // Strong invariant under the fix: in-flight wake-up tokens are capped
    // by the historical peak of `idler`. With a single consumer that peak
    // is 1, so `pending` must remain ≤1 regardless of burst size. Without
    // the fix `pending` would scale with kBurst.
    EXPECT_LE(pending, 1u);
    EXPECT_EQ(kBurst, received.load());
}
