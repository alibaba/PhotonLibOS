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

#include <fcntl.h>
#include <gtest/gtest.h>
#include <photon/common/alog.h>
#include <photon/common/executor/executor.h>
#include <photon/common/executor/stdlock.h>
#include <photon/common/utility.h>
#include <photon/fs/exportfs.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread.h>
#include <sched.h>
#include <immintrin.h>

#include <chrono>
#include <thread>

using namespace photon;

std::atomic<int> count(0), start(0);

static constexpr int th_num = 1;
static constexpr int app_num = 10000;

int ftask(photon::Executor *eth, int i) {
    LOG_TEMP("Add one");
    eth->async_perform(new auto ([i] {
        // sleep for 3 secs
        LOG_INFO("Async work ` start", i);
        start++;
        photon::thread_sleep(3);
        LOG_INFO("Async work ` done", i);
        count++;
    }));
    LOG_TEMP("Add one done");
    return 0;
}

TEST(std_executor, test) {
    photon::Executor eth;

    for (int i = 0; i < 10; i++) {
        ftask(&eth, i);
    }
    EXPECT_LT(count.load(), 10);
    printf("Task applied, wait for loop\n");
    while (count.load() != 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(10, start.load());
    EXPECT_EQ(start.load(), count.load());
}

TEST(std_executor, perf) {
    int cnt = 0;
    DEFER(EXPECT_EQ(th_num * app_num, cnt));
    photon::Executor eth;
    auto dura = std::chrono::nanoseconds(0);
    std::vector<std::thread> ths;
    ths.reserve(th_num);
    for (int i = 0; i < th_num; i++) {
        ths.emplace_back([&] {
            for (int j = 0; j < app_num; j++) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        pthread_setaffinity_np(pthread_self(),
                                        sizeof(cpu_set_t), &cpuset);
                auto start = std::chrono::high_resolution_clock::now();
                eth.async_perform(new auto ([&] { cnt++; if (cnt % 10000 == 0) printf("%d\n", cnt);}));
                auto end = std::chrono::high_resolution_clock::now();
                dura = dura + (end - start);
                // for (auto x = 0; x < 32; x++)
                //     _mm_pause();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    for (auto &x : ths) {
        x.join();
    }

    LOG_INFO(
        "Spent ` us for ` task async apply, average ` ns per task",
        std::chrono::duration_cast<std::chrono::microseconds>(dura).count(),
        DEC(th_num * app_num).comma(true),
        std::chrono::duration_cast<std::chrono::nanoseconds>(dura).count() /
            th_num / app_num);
}
