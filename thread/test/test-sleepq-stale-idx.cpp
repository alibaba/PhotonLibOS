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
#include <chrono>

#include "../../test/gtest.h"
#include <photon/common/utility.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>

TEST(SleepQueue, pop_front_single_sleeper_clears_idx_before_migrate) {
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE));
    DEFER(photon::fini());

    photon::WorkPool pool(1, photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE, 0);
    std::atomic<bool> done{false};

    photon::thread_create11([&] {
        // Keep this thread as the only sleeper on the source vCPU, so timeout
        // wakeup must go through SleepQueue::pop_front()'s single-node path.
        photon::thread_usleep(1000);

        // Re-enter SleepQueue::pop() from another vCPU's standbyq drain. A stale
        // idx left by pop_front() used to make this pop dereference/corrupt the
        // target vCPU's sleepq.
        pool.thread_migrate(photon::CURRENT, 0);
        done.store(true, std::memory_order_release);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load(std::memory_order_acquire)) {
        photon::thread_yield();
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
