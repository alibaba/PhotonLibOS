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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>

#include <photon/common/lockfree_queue.h>
#include <photon/thread/thread.h>
#include <pthread.h>

#include <array>
#ifdef TESTING_ENABLE_BOOST
#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#endif
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <photon/common/alog.h>
#include "../../test/ci-tools.h"

static constexpr size_t sender_num = 4;
static constexpr size_t receiver_num = 4;
static constexpr size_t items_num = 3000000;
static_assert(items_num % sender_num == 0,
              "item_num should able to divided by sender_num");
static_assert(items_num % receiver_num == 0,
              "item_num should able to divided by receiver_num");
static constexpr size_t capacity = 256;

std::array<int, receiver_num> rcnt;
std::array<int, sender_num> scnt;

std::array<std::atomic<int>, items_num / sender_num> sc, rc;

LockfreeMPMCRingQueue<int, capacity> lqueue;
LockfreeBatchMPMCRingQueue<int, capacity> lbqueue;
LockfreeSPSCRingQueue<int, capacity> cqueue;
std::mutex rlock, wlock;

#ifdef TESTING_ENABLE_BOOST
boost::lockfree::queue<int, boost::lockfree::capacity<capacity>> bqueue;
boost::lockfree::spsc_queue<int, boost::lockfree::capacity<capacity>> squeue;
#endif

struct WithLock {
    template <typename T>
    static void lock(T &x) {
        x.lock();
    }
    template <typename T>
    static void unlock(T &x) {
        x.unlock();
    }
};

struct NoLock {
    template <typename T>
    static void lock(T &x) {}
    template <typename T>
    static void unlock(T &x) {}
};

template <typename LSType, typename LRType, typename QType>
int test_queue(const char *name, QType &queue) {
    std::vector<std::thread> senders, receivers;
    scnt.fill(0);
    rcnt.fill(0);
    auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < receiver_num; i++) {
        receivers.emplace_back([i, &queue] {
            photon::set_cpu_affinity(i);
            std::chrono::nanoseconds rspent(std::chrono::nanoseconds(0));
            for (size_t x = 0; x < items_num / receiver_num; x++) {
                int t;
                auto tm = std::chrono::high_resolution_clock::now();
                LRType::lock(rlock);
                while (!queue.pop(t)) {
                    LRType::unlock(rlock);
                    CPUPause::pause();
                    LRType::lock(rlock);
                }
                (void)t;
                LRType::unlock(rlock);
                if (x) rspent += std::chrono::high_resolution_clock::now() - tm;
                rc[t]++;
                rcnt[i]++;
            }
            LOG_DEBUG("` receiver done, ` ns per action", i,
                   rspent.count() / (items_num / receiver_num - 1));
        });
    }
    for (size_t i = 0; i < sender_num; i++) {
        senders.emplace_back([i, &queue] {
            photon::set_cpu_affinity(i);
            std::chrono::nanoseconds wspent{std::chrono::nanoseconds(0)};
            for (size_t x = 0; x < items_num / sender_num; x++) {
                auto tm = std::chrono::high_resolution_clock::now();
                LSType::lock(wlock);
                while (!queue.push(x)) {
                    LSType::unlock(wlock);
                    CPUPause::pause();
                    LSType::lock(wlock);
                }
                LSType::unlock(wlock);
                wspent += std::chrono::high_resolution_clock::now() - tm;
                sc[x]++;
                scnt[i]++;
                // ThreadPause::pause();
            }
            LOG_DEBUG("` sender done, ` ns per action", i,
                   wspent.count() / (items_num / sender_num));
        });
    }
    for (auto &x : senders) x.join();
    for (auto &x : receivers) x.join();
    auto end = std::chrono::steady_clock::now();
    LOG_DEBUG("` ` p ` c, ` items, Spent ` us", name, sender_num, receiver_num, items_num,
           std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
    for (size_t i = 0; i < items_num / sender_num; i++) {
        if (sc[i] != rc[i] || sc[i] != sender_num) {
            LOG_DEBUG("MISMATCH ` ` `", i, sc[i].load(), rc[i].load());
        }
        sc[i] = 0;
        rc[i] = 0;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
}

template <typename LSType, typename LRType, typename QType>
int test_queue_batch(const char *name, QType &queue) {
    std::vector<std::thread> senders, receivers;
    scnt.fill(0);
    rcnt.fill(0);
    auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < receiver_num; i++) {
        receivers.emplace_back([i, &queue] {
            photon::set_cpu_affinity(i);
            int buffer[32];
            size_t size;
            int amount = items_num / receiver_num;
            std::chrono::nanoseconds rspent(std::chrono::nanoseconds(0));
            for (size_t x = 0; x < items_num / receiver_num;) {
                auto tm = std::chrono::high_resolution_clock::now();
                LRType::lock(rlock);
                while (!(size = queue.pop_batch(buffer, std::min(32, amount)))) {
                    LRType::unlock(rlock);
                    CPUPause::pause();
                    LRType::lock(rlock);
                }
                LRType::unlock(rlock);
                if (x) rspent += (std::chrono::high_resolution_clock::now() - tm) / size;
                for (auto y: xrange(size)) {
                    rc[buffer[y]]++;
                    rcnt[i]++;
                }
                x += size;
                amount -= size;
            }
            LOG_DEBUG("` receiver done, ` ns per action", i,
                   rspent.count() / (items_num / receiver_num - 1));
        });
    }
    for (size_t i = 0; i < sender_num; i++) {
        senders.emplace_back([i, &queue] {
            photon::set_cpu_affinity(i);
            std::vector<int> vec;
            vec.resize(items_num / sender_num);
            for (size_t x = 0; x < items_num / sender_num; x++) {
                vec[x] = x;
            }
            size_t size;
            std::chrono::nanoseconds wspent{std::chrono::nanoseconds(0)};
            for (size_t x = 0; x < items_num / sender_num;) {
                auto tm = std::chrono::high_resolution_clock::now();
                LSType::lock(wlock);
                while (!(size = queue.push_batch(&vec[x], std::min(32UL, vec.size() - x)))) {
                    LSType::unlock(wlock);
                    CPUPause::pause();
                    LSType::lock(wlock);
                }
                LSType::unlock(wlock);
                wspent += (std::chrono::high_resolution_clock::now() - tm) / size;
                for (auto y = x; y < x + size; y++) {
                    sc[y] ++;
                    scnt[i] ++;
                }
                x += size;
            }
            LOG_DEBUG("` sender done, ` ns per action", i,
                   wspent.count() / (items_num / sender_num));
        });
    }
    for (auto &x : senders) x.join();
    for (auto &x : receivers) x.join();
    auto end = std::chrono::steady_clock::now();
    LOG_DEBUG("` ` p ` c, ` items, Spent ` us", name, sender_num, receiver_num, items_num,
           std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
    for (size_t i = 0; i < items_num / sender_num; i++) {
        if (sc[i] != rc[i] || sc[i] != sender_num) {
            LOG_DEBUG("MISMATCH ` ` `", i, sc[i].load(), rc[i].load());
        }
        sc[i] = 0;
        rc[i] = 0;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
}

int main() {
#ifdef TESTING_ENABLE_BOOST
    test_queue<NoLock, NoLock>("BoostQueue", bqueue);
#endif
    test_queue<NoLock, NoLock>("PhotonLockfreeMPMCQueue", lqueue);
    test_queue<NoLock, NoLock>("PhotonLockfreeBatchMPMCQueue", lbqueue);
    test_queue_batch<NoLock, NoLock>("PhotonLockfreeBatchMPMCQueue+Batch", lbqueue);
#ifdef TESTING_ENABLE_BOOST
    test_queue<WithLock, WithLock>("BoostSPSCQueue", squeue);
#endif
    test_queue<WithLock, WithLock>("PhotonSPSCQueue", cqueue);
    test_queue_batch<WithLock, WithLock>("PhotonSPSCQueue+Batch", cqueue);
}
