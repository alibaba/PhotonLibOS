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
#include <gtest/gtest.h>
#include <thread>
#include <photon/common/metric-meter/metrics.h>
#include <photon/common/alog.h>

Metric::AverageLatencyCounter ldefer, lsem;

void* task_defer(void* arg) {
    SCOPE_LATENCY(*(Metric::AverageLatencyCounter*)(arg));
    auto th = photon::CURRENT;
    photon::thread_usleep_defer(-1, [&]{
        std::thread([&]{
            photon::thread_interrupt(th);
        }).detach();
    });
    return 0;
}

void* task_semaphore(void* arg) {
    SCOPE_LATENCY(*(Metric::AverageLatencyCounter*)(arg));
    std::shared_ptr<photon::semaphore> sem=std::make_shared<photon::semaphore>(0);
    std::thread([&, sem]{
        sem->signal(1);
    }).detach();
    sem->wait(1);
    return 0;
}

void* ph_task_defer(void* arg) {
    SCOPE_LATENCY(*(Metric::AverageLatencyCounter*)(arg));
    auto th = photon::CURRENT;
    auto task = [&]{
            photon::thread_interrupt(th);
        };
    photon::thread_usleep_defer(-1, [&]{
        photon::thread_create11(&decltype(task)::operator(), &task);
    });
    return 0;
}

void* ph_task_semaphore(void* arg) {
    SCOPE_LATENCY(*(Metric::AverageLatencyCounter*)(arg));
    std::shared_ptr<photon::semaphore> sem=std::make_shared<photon::semaphore>(0);
    auto task = [&, sem]{
            sem->signal(1);
        };
    photon::thread_create11(&decltype(task)::operator(), &task);
    sem->wait(1);
    return 0;
}

using TestTask = void*(*)(void*);
template<TestTask task, Metric::AverageLatencyCounter* arg, int ROUND=10000>
void do_test() {
    std::vector<photon::join_handle*> jh;
    for (auto i=0;i<ROUND;i++) {
        jh.emplace_back(
            photon::thread_enable_join(
                photon::thread_create(task, arg)
            )
        );
    }
    for (auto &j : jh) {
        photon::thread_join(j);
    }
}

TEST(perf, task_in_thread) {
    do_test<task_defer, &ldefer>();
    LOG_INFO(VALUE(ldefer.val()));
    do_test<task_semaphore, &lsem>();
    LOG_INFO(VALUE(lsem.val()));
    photon::thread_usleep(1);
}

TEST(perf, task_in_photon) {
    do_test<ph_task_defer, &ldefer>();
    LOG_INFO(VALUE(ldefer.val()));
    do_test<ph_task_semaphore, &lsem>();
    LOG_INFO(VALUE(lsem.val()));
    photon::thread_usleep(1);
}

int main(int argc, char** arg) {
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    photon::fd_events_init();
    DEFER(photon::fd_events_fini());
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
