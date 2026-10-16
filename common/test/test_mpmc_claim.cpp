#include <photon/common/lockfree_queue.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <thread>
#include <vector>

#include "../../test/gtest.h"

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
                  this->slots[this->idx(position)].mark.load(
                      std::memory_order_acquire));
        EXPECT_TRUE(this->tail.compare_exchange_strong(position, position + 1));
        return position;
    }

    void publish(uint64_t position, uint64_t value) {
        this->slots[this->idx(position)].data = value;
        this->slots[this->idx(position)].mark.store(
            this->this_turn_write(position), std::memory_order_release);
    }

    uint64_t claims() const {
        return this->head.load(std::memory_order_acquire);
    }

    void seed_empty(uint64_t position) {
        this->head.store(position);
        this->tail.store(position);
        for (uint64_t n = 0; n < this->capacity; ++n)
            this->slots[this->idx(position + n)].mark.store(
                this->last_turn_read(position + n));
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
void check_unpublished_slot(Queue& queue) {
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
    ASSERT_EQ(std::future_status::ready,
              claimed_future.wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(1U, queue.claims());
    uint64_t value = 0;
    EXPECT_TRUE(queue.pop(value));
    EXPECT_EQ(2U, value);
    EXPECT_TRUE(queue.pop(value));
    EXPECT_EQ(3U, value);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(std::future_status::timeout,
              first_future.wait_for(std::chrono::seconds(0)));
    // The deferred publication also demonstrates that the claim is retained.
}

TEST(mpmc_claim, unpublished_slot_does_not_block_later_consumers) {
    ReservedQueue<4> queue;
    check_unpublished_slot(queue);
}

TEST(mpmc_claim, flexible_queue_isolates_unpublished_slot) {
    using Queue = FlexQueue<ReservedQueue<0>::packedslot, ReservedQueue<0>>;
    auto* queue = Queue::create(3);
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
    std::thread first([&] {
        EXPECT_TRUE(queue.pop(results[0], [&] { claimed_a.set_value(); }));
    });
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
    ASSERT_EQ(std::future_status::ready,
              ready_a.wait_for(std::chrono::seconds(5)));
    second = std::thread([&] {
        EXPECT_TRUE(queue.pop(results[1], [&] { claimed_b.set_value(); }));
    });
    ASSERT_EQ(std::future_status::ready,
              ready_b.wait_for(std::chrono::seconds(5)));
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
    ASSERT_EQ(std::future_status::ready,
              ready.wait_for(std::chrono::seconds(5)));
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
    auto first =
        photon::thread_create11([&] { results[0] = channel.recv(0, 0); });
    auto second =
        photon::thread_create11([&] { results[1] = channel.recv(0, 0); });
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
        photon::thread_join(reinterpret_cast<photon::join_handle*>(first));
        photon::thread_join(reinterpret_cast<photon::join_handle*>(second));
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
    EXPECT_TRUE(
        wait_photon([&] { return results[0] == 2 || results[1] == 2; }));
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
        photon::thread_join(reinterpret_cast<photon::join_handle*>(reader));
    });
    ASSERT_TRUE(wait_photon([&] {
        return channel.claims() == 1 &&
               photon::thread_stat(reader) == photon::SLEEPING;
    }));
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
    for (auto& count : seen) count.store(0);
    std::vector<std::thread> readers, writers;
    for (unsigned c = 0; c < consumers; ++c) {
        readers.emplace_back([&] {
            ASSERT_EQ(0, photon::init(photon::INIT_EVENT_DEFAULT,
                                      photon::INIT_IO_NONE));
            DEFER(photon::fini());
            for (unsigned n = 0; n < producers * per_producer / consumers;
                 ++n) {
                uint64_t value = 0;
                while (!queue.pop(value)) std::this_thread::yield();
                if (value < seen.size())
                    seen[value].fetch_add(1);
                else
                    ADD_FAILURE() << "invalid task " << value;
            }
        });
    }
    for (unsigned p = 0; p < producers; ++p) {
        writers.emplace_back([&, p] {
            for (unsigned n = 0; n < per_producer; ++n)
                while (!queue.push(p * per_producer + n))
                    std::this_thread::yield();
        });
    }
    for (auto& writer : writers) writer.join();
    for (auto& reader : readers) reader.join();
    for (auto& count : seen) EXPECT_EQ(1U, count.load());
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(4U, queue.write_available());
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE) != 0)
        return 1;
    DEFER(photon::fini());
    return RUN_ALL_TESTS();
}
