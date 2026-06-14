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

#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/workerpool.h>

#include "../../test/gtest.h"

// Regression test for the RingChannel notification refactor's multi-consumer
// behavior: when N idle workers share a RingChannel, a burst of N tasks must
// fan out to all of them concurrently. A binary (single-token) notification
// scheme would serialize the burst onto a single worker.
//
// Verification strategy (deterministic, no wall-clock assumptions):
//   - Each task signals an `arrived` photon::semaphore on entry and then
//     blocks on `release` until the harness allows it to finish.
//   - The harness calls `arrived.wait(N, deadline_us)`. If fan-out works,
//     all N tasks reach the wait point; if the notification path serializes,
//     only one worker can run at a time (the others are stuck holding their
//     vCPU on `release.wait`), so `arrived` will never reach N and the wait
//     will return non-zero on timeout. The deadline is generous (5s) to
//     tolerate CI noise without weakening the assertion.
TEST(workpool, fanout_wakeup) {
    // WorkPool::async_call uses PhotonPause and signals a photon::semaphore;
    // the caller side must already be a photon vcpu.
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());

    constexpr int N = 4;
    photon::WorkPool pool(N, photon::INIT_EVENT_DEFAULT,
                          photon::INIT_IO_NONE, -1);

    photon::semaphore arrived(0);
    photon::semaphore release(0);
    std::atomic<int> done{0};

    for (int i = 0; i < N; ++i) {
        pool.async_call(new auto([&] {
            // Mark this worker as woken-and-running. Hold the vCPU until the
            // harness lets every other worker also reach this point.
            arrived.signal(1);
            release.wait(1);
            done.fetch_add(1, std::memory_order_release);
        }));
    }

    // Deterministic fan-out check. With the fix, all N tasks must reach
    // `arrived.signal(1)` concurrently. Without it, only one worker would
    // ever be released by the channel, so `arrived` saturates at 1 and the
    // wait times out.
    int r = arrived.wait(N, /*timeout_us=*/5ULL * 1000 * 1000);
    EXPECT_EQ(0, r);
    LOG_INFO("fan-out check: arrived.wait(`) returned ` (vcpu_num=`)",
             N, r, N);

    // Release everyone so the WorkPool can be cleanly destroyed.
    release.signal(N);
    while (done.load(std::memory_order_acquire) < N) {
        photon::thread_yield();
    }
}
