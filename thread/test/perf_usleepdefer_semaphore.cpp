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

#include "../thread.h"
#include "../thread11.h"
#include <photon/io/fd-events.h>
#include "../../test/gtest.h"
#include <thread>
#include <photon/common/metric-meter/metrics.h>
#include <photon/common/alog.h>

void* task_defer(void* arg) {
    auto th = photon::CURRENT;
    photon::thread_usleep_defer(-1, [&]{
        std::thread([&]{
            photon::thread_interrupt(th);
        }).detach();
    });
    return 0;
}

void* task_semaphore(void* arg) {
    photon::semaphore sem(0);
    std::thread([&]{
        sem.signal(1);
    }).detach();
    sem.wait(1);
    return 0;
}

void* ph_task_defer(void* arg) {
    auto th = photon::CURRENT;
    auto task = [&]{
        photon::thread_interrupt(th);
    };
    photon::thread_usleep_defer(-1, [&]{
        photon::thread_create11(task);
    });
    return 0;
}

void* ph_task_semaphore(void* arg) {
    photon::semaphore sem(0);
    auto task = [&]{
        sem.signal(1);
    };
    photon::thread_create11(task);
    sem.wait(1);
    return 0;
}

const int ROUND = 10000;
void do_test(ALogStringL name, photon::thread_entry task) {
    Metric::AverageLatencyCounter lat; {
        SCOPE_LATENCY(lat);
        photon::threads_create_join(ROUND, task, nullptr);
    }
    LOG_INFO("average latency = ` (`)", lat.val(), name);
}

#define DO_TEST(task) do_test(#task, & task);

TEST(perf, task_in_thread) {
    LOG_INFO(ROUND, " rounds");
    DO_TEST(task_defer);
    DO_TEST(task_semaphore);
    photon::thread_usleep(1);
}

TEST(perf, task_in_photon) {
    LOG_INFO(ROUND, " rounds");
    DO_TEST(ph_task_defer);
    DO_TEST(ph_task_semaphore);
    photon::thread_usleep(1);
}

int main(int argc, char** arg) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
