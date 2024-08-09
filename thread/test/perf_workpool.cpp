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
#include <vector>
#include <chrono>
#include <gflags/gflags.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>
#include <photon/thread/stack-allocator.h>
#include <photon/common/alog.h>

DEFINE_uint64(vcpu_num, 4, "vCPU num");
DEFINE_uint64(concurrency, 64, "Fire tasks into work-pool in parallel. Only available in async mode.");
DEFINE_uint64(fires, 50'000, "How many tasks to fire in each concurrency");

static photon::WorkPool* pool;
static std::atomic<uint64_t> sum_time;

void* task_async(void*) {
    photon::semaphore sem(0);
    for (uint64_t i = 0; i < FLAGS_fires; ++i) {
        auto start = std::chrono::steady_clock::now();
        pool->async_call(new auto([&, start] {
            auto end = std::chrono::steady_clock::now();
            sum_time.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(),
                               std::memory_order_relaxed);
            sem.signal(1);
        }));
        photon::thread_yield();
    }
    sem.wait(FLAGS_fires);
    return nullptr;
}

void* task_sync(void*) {
    auto start = std::chrono::steady_clock::now();
    pool->call([&, start] {
        auto end = std::chrono::steady_clock::now();
        sum_time.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(),
                           std::memory_order_relaxed);
    });
    return nullptr;
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    set_log_output_level(ALOG_INFO);

    photon::use_pooled_stack_allocator();

    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());

    pool = new photon::WorkPool(FLAGS_vcpu_num, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, -1);
    DEFER(delete pool);

    std::vector<photon::join_handle*> jhs;
    auto start = photon::now;
    photon::threads_create_join(FLAGS_concurrency, task_async, nullptr);
    auto end = photon::now;

    LOG_INFO("Fire ` async works and solved by ` vCPU, QPS is `, and average task deliver latency is ` ns",
             FLAGS_fires * FLAGS_concurrency, FLAGS_vcpu_num,
             FLAGS_concurrency * FLAGS_fires * 1000 * 1000 / (end - start),
             sum_time.load() / FLAGS_fires / FLAGS_concurrency);

    sum_time = 0;
    jhs.clear();

    start = photon::now;
    photon::threads_create_join(FLAGS_fires, task_sync, nullptr);
    end = photon::now;
    LOG_INFO("Fire ` sync works and solved by ` vCPU, QPS is `, and average task deliver latency is ` ns",
             FLAGS_fires, FLAGS_vcpu_num, FLAGS_fires * 1000 * 1000 / (end - start),
             sum_time.load() / FLAGS_fires);
}