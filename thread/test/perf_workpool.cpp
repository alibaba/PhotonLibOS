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

#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/photon.h>
#include <photon/thread/workerpool.h>
#include <photon/thread/thread11.h>
#include <sys/fcntl.h>

#include <thread>
#include <vector>
#include <chrono>


static constexpr size_t pool_size = 4;
static constexpr size_t concurrent = 64;
static constexpr size_t fires = 10UL*1000;

photon::WorkPool *pool;
std::atomic<uint64_t> sum;

void* task(void* arg) {
    photon::semaphore sem(0);
    for (auto i = 0; i < fires; i++) {
        auto start = std::chrono::steady_clock::now();
        pool->async_call(new auto ([&, start]{
            auto end = std::chrono::steady_clock::now();
            sum.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(), std::memory_order_relaxed);
            sem.signal(1);
        }));
        photon::thread_yield();
    }
    sem.wait(fires);
    return nullptr;
}

void* task_sync(void* arg) {
    for (auto i = 0; i < fires; i++) {
        auto start = std::chrono::steady_clock::now();
        pool->call([&, start]{
            auto end = std::chrono::steady_clock::now();
            sum.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(), std::memory_order_relaxed);
        });
    }
    return nullptr;
}

int main() {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_LIBAIO);
    DEFER(photon::fini());
    pool = new photon::WorkPool(pool_size, photon::INIT_EVENT_EPOLL, 64);
    DEFER(delete pool);
    std::vector<photon::join_handle*> jhs;
    auto start = photon::now;
    for (uint64_t i = 0; i < concurrent; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create(task, (void*)i)));
    }
    for (auto& x : jhs) {
        photon::thread_join(x);
    }
    auto end = photon::now;
    jhs.clear();
    LOG_INFO("` works in ` usec, QPS=`", concurrent * fires, pool_size,
             concurrent * fires * 1UL * 1000 * 1000 / (end - start));
    LOG_INFO("Fire ` async works and solved by ` vcpu, caused ` ns, average ` ns",
        fires * concurrent, pool_size, sum.load(), sum.load() / fires / concurrent);
    sum = 0;
    start = photon::now;
    for (uint64_t i = 0; i < concurrent; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create(task_sync, (void*)i)));
    }
    for (auto& x : jhs) {
        photon::thread_join(x);
    }
    end = photon::now;
    jhs.clear();
    LOG_INFO("` works in ` usec, QPS=`", concurrent * fires, pool_size,
             concurrent * fires * 1UL * 1000 * 1000 / (end - start));
    LOG_INFO("Fire ` sync works and solved by ` vcpu, caused ` ns, average ` ns",
        fires * concurrent, pool_size, sum.load(), sum.load() / fires / concurrent);
    return 0;
}