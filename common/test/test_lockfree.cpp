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
#include <photon/thread/thread11.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
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
#include <gtest/gtest.h>
#include <gflags/gflags.h>
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
FlexLockfreeMPMCRingQueue<int> &luqueue = [] {
    static auto q = FlexLockfreeMPMCRingQueue<int>::create(capacity);
    return std::ref(*q);
}();
LockfreeBatchMPMCRingQueue<int, capacity> lbqueue;
FlexLockfreeBatchMPMCRingQueue<int> &lubqueue = [] {
    static auto q = FlexLockfreeBatchMPMCRingQueue<int>::create(capacity);
    return std::ref(*q);
}();
LockfreeSPSCRingQueue<int, capacity> cqueue;
FlexLockfreeSPSCRingQueue<int> &cuqueue = [] {
    static auto q = FlexLockfreeSPSCRingQueue<int>::create(capacity);
    return std::ref(*q);
}();
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
            photon::set_cpu_affinity(i * 2);
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
            photon::set_cpu_affinity(i * 2 + 1);
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
        EXPECT_EQ(sc[i], rc[i]);
        EXPECT_EQ(sc[i].load(), (int)sender_num);
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
            photon::set_cpu_affinity(i * 2);
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
            photon::set_cpu_affinity(i * 2 + 1);
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
                while (!(size = queue.push_batch(&vec[x], std::min(size_t(32ULL), vec.size() - x)))) {
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
        EXPECT_EQ(sc[i], rc[i]);
        EXPECT_EQ(sc[i].load(), (int)sender_num);
        sc[i] = 0;
        rc[i] = 0;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
}

TEST(lockfree_queue, test_queue) {
#ifdef TESTING_ENABLE_BOOST
    test_queue<NoLock, NoLock>("BoostQueue", bqueue);
#endif
    test_queue<NoLock, NoLock>("PhotonLockfreeMPMCQueue", lqueue);
    test_queue<NoLock, NoLock>("FlexPhotonLockfreeMPMCQueue", luqueue);
    test_queue<NoLock, NoLock>("PhotonLockfreeBatchMPMCQueue", lbqueue);
    test_queue_batch<NoLock, NoLock>("PhotonLockfreeBatchMPMCQueue+Batch",
                                     lbqueue);
    test_queue<NoLock, NoLock>("FlexPhotonLockfreeBatchMPMCQueue", lubqueue);
    test_queue_batch<NoLock, NoLock>("FlexPhotonLockfreeBatchMPMCQueue+Batch",
                                     lubqueue);
#ifdef TESTING_ENABLE_BOOST
    test_queue<WithLock, WithLock>("BoostSPSCQueue", squeue);
#endif
    test_queue<WithLock, WithLock>("PhotonSPSCQueue", cqueue);
    test_queue_batch<WithLock, WithLock>("PhotonSPSCQueue+Batch", cqueue);
    test_queue<WithLock, WithLock>("FlexPhotonSPSCQueue", cuqueue);
    test_queue_batch<WithLock, WithLock>("FlexPhotonSPSCQueue+Batch", cuqueue);
}

TEST(FlexRingChannel, basic) {
    using FlexRing = FlexLockfreeMPMCRingQueue<int>;
    using Channel = photon::common::FlexRingChannel<FlexRing>;

    auto* ch = Channel::create(16);
    ASSERT_NE(ch, nullptr);
    DEFER(Channel::destroy(ch));

    EXPECT_TRUE(ch->empty());
    EXPECT_FALSE(ch->full());
    EXPECT_EQ(ch->read_available(), 0u);

    for (int i = 1; i <= 10; i++) {
        ch->send(i);
    }
    EXPECT_FALSE(ch->empty());
    EXPECT_EQ(ch->read_available(), 10u);

    for (int i = 1; i <= 10; i++) {
        int val = ch->recv();
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(ch->empty());
    EXPECT_EQ(ch->read_available(), 0u);
}

TEST(FlexRingChannel, mpmc) {
    using FlexRing = FlexLockfreeMPMCRingQueue<int>;
    using Channel = photon::common::FlexRingChannel<FlexRing>;

    auto* ch = Channel::create(2048);
    ASSERT_NE(ch, nullptr);
    DEFER(Channel::destroy(ch));

    constexpr int num_items = 1000;
    std::atomic<int> sum{0};
    std::atomic<bool> producer_done{false};

    // Producer thread (std::thread)
    std::thread producer([&] {
        for (int i = 1; i <= num_items; i++) {
            ch->send<ThreadPause>(i);
        }
        producer_done.store(true);
    });

    // Consumer thread (photon thread)
    auto consumer = photon::thread_create([](void* arg) -> void* {
        auto* args = (std::pair<Channel*, std::atomic<int>*>*)arg;
        auto* ch = args->first;
        auto* sum = args->second;
        for (int i = 0; i < 1000; i++) {
            int val = ch->recv();
            sum->fetch_add(val);
        }
        delete args;
        return nullptr;
    }, new std::pair<Channel*, std::atomic<int>*>(ch, &sum));
    photon::thread_enable_join(consumer);

    producer.join();
    photon::thread_join((photon::join_handle*)consumer);

    int expected = num_items * (num_items + 1) / 2;
    EXPECT_EQ(sum.load(), expected);
}

namespace {

// Suspend a producer exactly after its tail reservation, without sleeps or
// production fault-injection hooks. All accesses obey the real slot protocol.
template <size_t Capacity, typename MarkType = uint64_t>
struct ReservedQueue : LockfreeMPMCRingQueue<uint64_t, Capacity, MarkType> {
    using Base = LockfreeMPMCRingQueue<uint64_t, Capacity, MarkType>;

    ReservedQueue() = default;

protected:
    explicit ReservedQueue(size_t capacity) : Base(capacity) {}

public:
    uint64_t reserve() {
        auto position = this->tail.load(std::memory_order_acquire);
        EXPECT_EQ(this->last_turn_read(position),
                  this->slots[this->idx(position)].mark.load(std::memory_order_acquire));
        EXPECT_TRUE(this->tail.compare_exchange_strong(position, position + 1));
        return position;
    }

    void publish(uint64_t position, uint64_t value) {
        this->slots[this->idx(position)].data = value;
        this->slots[this->idx(position)].mark.store(this->this_turn_write(position),
                                                    std::memory_order_release);
    }

    uint64_t claims() const { return this->head.load(std::memory_order_acquire); }

    void seed_empty(uint64_t position) {
        this->head.store(position);
        this->tail.store(position);
        for (uint64_t n = 0; n < this->capacity; ++n)
            this->slots[this->idx(position + n)].mark.store(this->last_turn_read(position + n));
    }
};

struct ClaimChannel : photon::common::RingChannel<ReservedQueue<4>> {
    ClaimChannel() { default_park_usec = 3600ULL * 1000 * 1000; }
    void notify() { notify_recvers(); }
};

template <typename Predicate>
bool wait_photon(Predicate predicate) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        photon::thread_yield();
    }
    return true;
}

TEST(mpmc_claim, empty_pop_does_not_reserve_future_positions) {
    ReservedQueue<4> queue;
    uint64_t value = 999;
    for (unsigned n = 0; n < 100; ++n) EXPECT_FALSE(queue.pop(value));
    EXPECT_EQ(0U, queue.claims());
    EXPECT_EQ(999U, value);
    EXPECT_EQ(4U, queue.write_available());
    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.pop(value));
    EXPECT_EQ(1U, value);
    EXPECT_FALSE(queue.pop(value));
}

template <typename Queue>
void check_unpublished_slot(Queue &queue) {
    auto a = queue.reserve();
    EXPECT_TRUE(queue.push(2));
    EXPECT_TRUE(queue.push(3));
    std::promise<void> claimed;
    auto claimed_future = claimed.get_future();
    std::promise<uint64_t> first_result;
    auto first_future = first_result.get_future();
    std::thread first([&] {
        uint64_t value = 0;
        EXPECT_TRUE(queue.pop(value, [&] { claimed.set_value(); }));
        first_result.set_value(value);
    });
    DEFER({
        queue.publish(a, 1);
        first.join();
        EXPECT_EQ(1U, first_future.get());
    });
    ASSERT_EQ(std::future_status::ready, claimed_future.wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(1U, queue.claims());
    uint64_t value = 0;
    EXPECT_TRUE(queue.pop(value));
    EXPECT_EQ(2U, value);
    EXPECT_TRUE(queue.pop(value));
    EXPECT_EQ(3U, value);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(std::future_status::timeout, first_future.wait_for(std::chrono::seconds(0)));
    // The deferred publication also demonstrates that the claim is retained.
}

TEST(mpmc_claim, unpublished_slot_does_not_block_later_consumers) {
    ReservedQueue<4> queue;
    check_unpublished_slot(queue);
}

TEST(mpmc_claim, flexible_queue_isolates_unpublished_slot) {
    using Queue = FlexQueue<ReservedQueue<0>::packedslot, ReservedQueue<0>>;
    auto *queue = Queue::create(3);
    ASSERT_NE(nullptr, queue);
    DEFER(Queue::destroy(queue));
    EXPECT_EQ(4U, queue->capacity);
    check_unpublished_slot(*queue);
    EXPECT_EQ(4U, queue->write_available());
}

TEST(mpmc_claim, several_holes_leave_later_work_available) {
    ReservedQueue<4> queue;
    auto a = queue.reserve();
    auto b = queue.reserve();
    EXPECT_TRUE(queue.push(3));
    std::promise<void> claimed_a, claimed_b;
    auto ready_a = claimed_a.get_future();
    auto ready_b = claimed_b.get_future();
    std::array<uint64_t, 2> results{};
    std::thread first([&] { EXPECT_TRUE(queue.pop(results[0], [&] { claimed_a.set_value(); })); });
    std::thread second;
    DEFER({
        queue.publish(a, 1);
        queue.publish(b, 2);
        first.join();
        EXPECT_EQ(1U, results[0]);
        if (second.joinable()) {
            second.join();
            EXPECT_EQ(2U, results[1]);
        }
    });
    ASSERT_EQ(std::future_status::ready, ready_a.wait_for(std::chrono::seconds(5)));
    second =
        std::thread([&] { EXPECT_TRUE(queue.pop(results[1], [&] { claimed_b.set_value(); })); });
    ASSERT_EQ(std::future_status::ready, ready_b.wait_for(std::chrono::seconds(5)));
    uint64_t value = 0;
    EXPECT_TRUE(queue.pop(value));
    EXPECT_EQ(3U, value);
}

TEST(mpmc_claim, wraparound_does_not_reuse_a_claimed_slot) {
    ReservedQueue<4> queue;
    auto a = queue.reserve();
    EXPECT_TRUE(queue.push(2));
    EXPECT_TRUE(queue.push(3));
    EXPECT_TRUE(queue.push(4));
    std::promise<void> claimed;
    auto ready = claimed.get_future();
    std::thread first([&] {
        uint64_t value = 0;
        EXPECT_TRUE(queue.pop(value, [&] { claimed.set_value(); }));
        EXPECT_EQ(1U, value);
    });
    bool released = false;
    DEFER({
        if (!released) queue.publish(a, 1);
        if (first.joinable()) first.join();
    });
    ASSERT_EQ(std::future_status::ready, ready.wait_for(std::chrono::seconds(5)));
    uint64_t value = 0;
    for (uint64_t expected = 2; expected <= 4; ++expected) {
        EXPECT_TRUE(queue.pop(value));
        EXPECT_EQ(expected, value);
    }
    // head==tail does not imply reusable capacity: position zero is still held.
    EXPECT_TRUE(queue.empty());
    EXPECT_TRUE(queue.full());
    EXPECT_EQ(0U, queue.write_available());
    auto pushed = std::async(std::launch::async, [&] { return queue.push(5); });
    auto status = pushed.wait_for(std::chrono::seconds(5));
    EXPECT_EQ(std::future_status::ready, status);
    if (status == std::future_status::ready) {
        EXPECT_FALSE(pushed.get());
    }
    queue.publish(a, 1);
    released = true;
    first.join();
    EXPECT_FALSE(queue.full());
    for (uint64_t n = 5; n < 105; ++n) {
        EXPECT_TRUE(queue.push(n));
        EXPECT_TRUE(queue.pop(value));
        EXPECT_EQ(n, value);
    }
}

TEST(mpmc_claim, counter_wraparound_preserves_claim_order) {
    ReservedQueue<4> queue;
    queue.seed_empty(std::numeric_limits<uint64_t>::max() - 1);
    uint64_t value = 0;
    for (uint64_t n = 1; n <= 20; ++n) {
        EXPECT_TRUE(queue.push(n));
        EXPECT_TRUE(queue.pop(value));
        EXPECT_EQ(n, value);
        EXPECT_FALSE(queue.pop(value));
    }
}

TEST(mpmc_claim, narrow_marks_preserve_slot_reuse) {
    ReservedQueue<4, uint8_t> queue;
    uint64_t value = 0;
    // Reuse slots across multiple wraps of the configurable mark type.
    for (uint64_t n = 0; n < 1024; ++n) {
        ASSERT_TRUE(queue.push(n));
        ASSERT_TRUE(queue.pop(value));
        ASSERT_EQ(n, value);
    }
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.full());
    EXPECT_EQ(4U, queue.write_available());
}

TEST(mpmc_claim, parked_consumers_receive_baton_before_publication) {
    ClaimChannel channel;
    std::array<uint64_t, 2> results{};
    auto first = photon::thread_create11([&] { results[0] = channel.recv(0, 0); });
    auto second = photon::thread_create11([&] { results[1] = channel.recv(0, 0); });
    photon::thread_enable_join(first);
    photon::thread_enable_join(second);
    uint64_t a = 0;
    bool reserved = false;
    DEFER({
        if (reserved) {
            channel.publish(a, 1);
            channel.notify();
        } else {
            channel.send<PhotonPause>(1);
            channel.send<PhotonPause>(2);
        }
        photon::thread_join(reinterpret_cast<photon::join_handle *>(first));
        photon::thread_join(reinterpret_cast<photon::join_handle *>(second));
    });
    ASSERT_TRUE(wait_photon([&] {
        return photon::thread_stat(first) == photon::SLEEPING &&
               photon::thread_stat(second) == photon::SLEEPING;
    }));
    a = channel.reserve();
    reserved = true;
    ASSERT_TRUE(channel.push(2));
    // Only one initial notification. A's reader must notify B's reader before
    // waiting; the one-hour safety net cannot account for this progress.
    channel.notify();
    EXPECT_TRUE(wait_photon([&] { return results[0] == 2 || results[1] == 2; }));
    EXPECT_EQ(2U, results[0] + results[1]);
    EXPECT_TRUE(channel.empty());
}

TEST(mpmc_claim, last_claim_completes_without_a_publication_notification) {
    ClaimChannel channel;
    auto a = channel.reserve();
    uint64_t result = 0;
    auto reader = photon::thread_create11([&] { result = channel.recv(0, 0); });
    photon::thread_enable_join(reader);
    bool published = false;
    DEFER({
        if (!published) channel.publish(a, 1);
        // On failure, provide a cleanup item so a reader that incorrectly
        // entered empty-queue parking can wake and exit before we join it.
        // This happens after the assertions and cannot make the test pass.
        if (result != 1) channel.send<PhotonPause>(0);
        photon::thread_join(reinterpret_cast<photon::join_handle *>(reader));
    });
    ASSERT_TRUE(wait_photon(
        [&] { return channel.claims() == 1 && photon::thread_stat(reader) == photon::SLEEPING; }));
    EXPECT_TRUE(channel.empty());
    // No push/send/notify here: the last claim must poll publication rather than
    // park on the empty-queue notification path (whose safety net is one hour).
    channel.publish(a, 1);
    published = true;
    EXPECT_TRUE(wait_photon([&] { return result == 1; }));
}

TEST(mpmc_claim, concurrent_small_queue_has_no_loss_or_duplicates) {
    constexpr unsigned producers = 4, consumers = 4, per_producer = 4000;
    ReservedQueue<4> queue;
    std::vector<std::atomic<unsigned>> seen(producers * per_producer);
    for (auto &count : seen) count.store(0);
    std::vector<std::thread> readers, writers;
    for (unsigned c = 0; c < consumers; ++c) {
        readers.emplace_back([&] {
            for (unsigned n = 0; n < producers * per_producer / consumers; ++n) {
                uint64_t value = 0;
                while (!queue.pop(value)) std::this_thread::yield();
                if (value < seen.size()) seen[value].fetch_add(1);
                else ADD_FAILURE() << "invalid task " << value;
            }
        });
    }
    for (unsigned p = 0; p < producers; ++p) {
        writers.emplace_back([&, p] {
            for (unsigned n = 0; n < per_producer; ++n)
                while (!queue.push(p * per_producer + n)) std::this_thread::yield();
        });
    }
    for (auto &writer : writers) writer.join();
    for (auto &reader : readers) reader.join();
    for (auto &count : seen) EXPECT_EQ(1U, count.load());
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(4U, queue.write_available());
}

}  // namespace

int main(int argc, char **arg) {
    if (!photon::is_using_default_engine()) return 0;
    ::testing::InitGoogleTest(&argc, arg);
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    DEFER(luqueue.destroy(&luqueue));
    DEFER(lubqueue.destroy(&lubqueue));
    DEFER(cuqueue.destroy(&cuqueue));
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    return RUN_ALL_TESTS();
}
