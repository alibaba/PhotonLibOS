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
DEFINE_uint64(fires, 80000, "How many tasks to fire");
DEFINE_uint64(workload_time_us, 0, "The workload time cost before each delivery");

static photon::WorkPool* pool;
static std::atomic<uint64_t> sum_time;

static void workload(uint64_t time_us) {
    auto start = std::chrono::steady_clock::now();
    long diff_ns;
    do {
        auto diff = std::chrono::steady_clock::now() - start;
        diff_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(diff).count();
    } while (diff_ns < (long) time_us * 1000);
}

static void task_async() {
    photon::semaphore sem(0);
    for (uint64_t i = 0; i < FLAGS_fires; ++i) {
        workload(FLAGS_workload_time_us);
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
}

static void task_sync() {
    for (uint64_t i = 0; i < FLAGS_fires; ++i) {
        workload(FLAGS_workload_time_us);
        auto start = std::chrono::steady_clock::now();
        pool->call<photon::PhotonContext>([&, start] {
            auto end = std::chrono::steady_clock::now();
            sum_time.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(),
                               std::memory_order_relaxed);
        });
    }
}

static void task_sync_in_std_context() {
    for (uint64_t i = 0; i < FLAGS_fires; ++i) {
        workload(FLAGS_workload_time_us);
        auto start = std::chrono::steady_clock::now();
        pool->call<photon::StdContext>([&, start] {
            auto end = std::chrono::steady_clock::now();
            sum_time.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(),
                               std::memory_order_relaxed);

        });
    }
}

static inline size_t get_qps(std::chrono::time_point<std::chrono::steady_clock> start,
                             std::chrono::time_point<std::chrono::steady_clock> end) {
    return FLAGS_fires * 1000 * 1000 / std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    set_log_output_level(ALOG_INFO);

    photon::use_pooled_stack_allocator();
    photon::pooled_stack_trim_threshold(-1UL);

    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());

    // 1. thread mode WorkPool, will create thread for every task
    pool = new photon::WorkPool(FLAGS_vcpu_num, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 0);
    DEFER(delete pool);
    auto start = std::chrono::steady_clock::now();
    task_async();
    auto end = std::chrono::steady_clock::now();
    LOG_INFO("Fire ` async works and solved by ` vCPU, QPS is `, and average task deliver latency is ` ns",
             FLAGS_fires, FLAGS_vcpu_num,
             get_qps(start, end),
             sum_time.load() / FLAGS_fires);

    // 2. Same as 1, but use sync API
    sum_time = 0;
    delete pool;
    pool = new photon::WorkPool(FLAGS_vcpu_num, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 0);
    start = std::chrono::steady_clock::now();
    task_sync();
    end = std::chrono::steady_clock::now();
    LOG_INFO("Fire ` sync works and solved by ` vCPU, QPS is `, and average task deliver latency is ` ns",
             FLAGS_fires, FLAGS_vcpu_num,
             get_qps(start, end),
             sum_time.load() / FLAGS_fires);

    // 2. Same as 1, but will create thread from a thread-pool
    sum_time = 0;
    delete pool;
    pool = new photon::WorkPool(FLAGS_vcpu_num, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 65536);
    start = std::chrono::steady_clock::now();
    task_async();
    end = std::chrono::steady_clock::now();
    LOG_INFO("Fire ` async works (to thread-pool) and solved by ` vCPU, QPS is `, and average task deliver latency is ` ns",
             FLAGS_fires, FLAGS_vcpu_num,
             get_qps(start, end),
             sum_time.load() / FLAGS_fires);

    // 4. non-thread mode WorkPool. Tasks are from std context
    delete pool;
    pool = new photon::WorkPool(FLAGS_vcpu_num, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, -1);
    sum_time = 0;
    start = std::chrono::steady_clock::now();
    std::thread th(task_sync_in_std_context);
    th.join();
    end = std::chrono::steady_clock::now();
    LOG_INFO("Fire ` sync works in std context and solved by ` vCPU, QPS is `, and average task deliver latency is ` ns",
             FLAGS_fires, FLAGS_vcpu_num,
             get_qps(start, end),
             sum_time.load() / FLAGS_fires);
}
