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
#include <vector>
#include <gtest/gtest.h>
#include <photon/photon.h>
#include <photon/thread/std-compat.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include "../../test/ci-tools.h"

static constexpr int nMutexes = 4;
static constexpr int nWorkers = 4;
static constexpr int nThreads = 32;    // Increase if necessary
static constexpr int testTimeSeconds = 60;

photon_std::mutex mutexes[nMutexes];
std::atomic<int64_t> acquisitionCounters[nMutexes];
std::atomic<int64_t> acquisitionDurations[nMutexes];
bool running = true;

static void timedLock(photon_std::mutex& mutex, int index) {
    long long durationMicros;
    auto start = std::chrono::steady_clock::now();
    {
        photon_std::lock_guard<photon_std::mutex> lock(mutex);
        acquisitionCounters[index].fetch_add(1);
        auto end = std::chrono::steady_clock::now();
        durationMicros = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        acquisitionDurations[index].fetch_add(durationMicros);
    }
    if (durationMicros > 1'000'000) {
        LOG_ERROR("long acquisition. mutex `, duration: `ms", index, durationMicros / 1000);
        std::abort();
    }
}

static void myThread(int tid) {
    LOG_INFO("thread ` starting", tid);
    while (running) {
        for (int i = 0; i < nMutexes; i++) {
            timedLock(mutexes[i], i);
        }
    }
}

TEST(multi_vcpu_locking, long_time_acquisition_should_abort) {
    int ret = photon::init(ci_ev_engine, photon::INIT_IO_NONE);
    GTEST_ASSERT_EQ(0, ret);
    DEFER(photon::fini());

    ret = photon_std::work_pool_init(nWorkers, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    GTEST_ASSERT_EQ(0, ret);
    DEFER(photon_std::work_pool_fini());

    std::vector<photon_std::thread> threads;
    for (int i = 0; i < nThreads; i++) {
        threads.emplace_back(myThread, i);
    }

    for (int i = 0; i < testTimeSeconds; ++i) {
        for (int j = 0; j < nMutexes; j++) {
            auto count = acquisitionCounters[j].load();
            auto durationMs = acquisitionDurations[j].load() / 1000;
            LOG_INFO("mutex `: total acquisitions: `, total wait time: `ms", j, count, durationMs);
        }
        photon_std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    running = false;
    for (auto& th: threads) {
        th.join();
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ci_parse_env();
    return RUN_ALL_TESTS();
}