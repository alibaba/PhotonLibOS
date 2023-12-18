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

// This is an example of synchronization primitives, also comparing the performance of
// std::condition_variable and photon::semaphore. Note that std::binary_semaphore was not added
// until C++20. Before that, cv is the only alternative.
//
// Test results:
// std:     QPS 1231796, latency 8303 ns
// Photon:  QPS 1102088, latency 10051 ns

#include <mutex>
#include <condition_variable>
#include <chrono>

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/lockfree_queue.h>
#include <photon/thread/workerpool.h>
#include <photon/thread/thread11.h>

// Comment/uncomment this macro to trigger std or photon
#define PHOTON

struct Message {
    photon::semaphore sem;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::chrono::time_point<std::chrono::steady_clock> start;
};

static const int num_producers = 8, num_consumers = 8;
static LockfreeMPMCRingQueue<Message*, 1024 * 1024> ring;
static std::atomic<uint64_t> qps{0}, latency{0};

__attribute__ ((noinline))
static void do_fill(char* buf, size_t size) {
    memset(buf, 0, size);
}

// A function that costs approximately 1 us
static void func_1us() {
    for (int i = 0; i < 5; ++i) {
        constexpr size_t size = 32 * 1024;
        char buf[size];
        do_fill(buf, size);
    }
}

int main() {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    photon::WorkPool wp(num_producers, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 128 * 1024);

    // Setup some producer vCPUs/threads, continuously adding messages into the lock-free ring queue.
    // Use semaphore.wait() for cv.wait() to wait for the messages been processed by the consumers.
#ifdef PHOTON
    for (int i = 0; i < num_producers; ++i) {
        wp.async_call(new auto([] {
            while (true) {
                func_1us();
                Message message;
                ring.push(&message);
                message.sem.wait(1);
                auto end = std::chrono::steady_clock::now();
                auto duration_us = std::chrono::duration_cast<std::chrono::nanoseconds>(end - message.start).count();
                latency.fetch_add(duration_us, std::memory_order::memory_order_relaxed);
                qps.fetch_add(1, std::memory_order::memory_order_relaxed);
            }
        }));
    }
#else
    for (int i = 0; i < num_producers; ++i) {
        new std::thread([] {
            while (true) {
                func_1us();
                Message message;
                ring.push(&message);
                {
                    std::unique_lock<std::mutex> l(message.mu);
                    message.cv.wait(l, [&] { return message.done; });
                }
                auto end = std::chrono::steady_clock::now();
                auto duration_us = std::chrono::duration_cast<std::chrono::nanoseconds>(end - message.start).count();
                latency.fetch_add(duration_us, std::memory_order::memory_order_relaxed);
                qps.fetch_add(1, std::memory_order::memory_order_relaxed);
            }
        });
    }
#endif

    // Setup some consumer std::threads, busy polling.
    // Continuously process messages from the ring queue, and then notify the consumer.
    for (int i = 0; i < num_consumers; ++i) {
        new std::thread([&] {
            while (true) {
                func_1us();
                Message* m;
                bool ok = ring.pop_weak(m);
                if (!ok)
                    continue;
                m->start = std::chrono::steady_clock::now();
#ifdef PHOTON
                m->sem.signal(1);
#else
                {
                    std::unique_lock<std::mutex> l(m->mu);
                    m->done = true;
                    m->cv.notify_one();
                }
#endif
            }
        });
    }

    // Show QPS and latency
    photon::thread_create11([&] {
        while (true) {
            photon::thread_sleep(1);
            auto prev_qps = qps.exchange(0, std::memory_order_seq_cst);
            auto prev_lat = latency.exchange(0, std::memory_order_seq_cst);
            if (prev_qps != 0)
                prev_lat = prev_lat / prev_qps;
            LOG_INFO("QPS `, latency ` ns", prev_qps, prev_lat);
        }
    });

    photon::thread_sleep(-1);
}

