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

// Hunts for lost notifications in RingChannel's park-slot notification path. A
// lost notification is not a wrong number, it is a stall, and in production the
// safety-net timeout inside ParkStack::park() hides it as a 100ms hiccup. Every
// case here therefore runs the channel with an explicit park timeout:
//
//   - NO_NET (longer than the whole test): the safety net can never fire, so a
//     single lost notification turns into a hang, which the watchdog reports.
//   - a few tens of microseconds: every claim races against a timeout wake-up.
//     That is the hardest window of the state machine -- the claimer finds the
//     slot COMMITTED and interrupts a thread that is already running again --
//     and the interrupt it issued must be absorbed inside park() instead of
//     surfacing at whatever the caller of recv() does next.
//
// The windows that have to stay closed:
//   - a consumer published its slot but is not asleep yet, so a claimer may not
//     interrupt it (the interrupt would be dropped and the sleep lost);
//   - a claimer holds the whole idle stack while it hands the rest back, so a
//     producer pushing right then finds nobody parked and skips its wake-up:
//     the consumer that does get woken has to pass the baton on;
//   - a consumer that goes back to sleep after an unrelated interrupt or a
//     timeout must re-arm its slot before sleeping again.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <photon/common/alog.h>
#include <photon/common/lockfree_queue.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>

#include "../../test/gtest.h"

using Queue = LockfreeMPMCRingQueue<uint64_t, 4096>;

// A channel whose safety net is under the test's control. `default_park_usec`
// is protected exactly so that it can be reached from here.
struct TestChannel : photon::common::RingChannel<Queue> {
    explicit TestChannel(uint64_t park_usec) {
        default_park_usec = park_usec;
    }
    size_t pending_items() { return read_available(); }

    // A producer is an order of magnitude faster than a wake-up, so left alone
    // it would keep the queue non-empty and the consumers would never park at
    // all. Waiting for the queue to drain first is what makes every item pay
    // the notification path -- and it aims the push at the very moment when
    // the consumers are publishing their slots.
    void send_paced(uint64_t x) {
        while (pending_items() > 0) std::this_thread::yield();
        send<ThreadPause>(x);
    }
};

// longer than any of the cases below: the safety net must never fire
static const uint64_t NO_NET = 3600ULL * 1000 * 1000;
static const uint64_t DEADLINE_SEC = 30;
static const uint64_t SENTINEL = 0;     // tells a consumer to leave

static std::atomic<uint64_t> g_progress{0};     // any item received, anywhere
static const char* g_case = "";
static std::function<std::string()> g_state;

// A stalled channel means every thread is asleep, so nothing photon-based can
// be trusted to report it: the watchdog is a plain OS thread, and it writes
// with write() because ALOG may be blocked behind a lock.
static void start_watchdog() {
    std::thread([] {
        for (uint64_t last = 0, stalls = 0;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            auto p = g_progress.load();
            if (p != last) { last = p; stalls = 0; continue; }
            if (++stalls * 2 < DEADLINE_SEC) continue;
            auto msg = std::string("\nWATCHDOG: case '") + g_case +
                       "' made no progress in " + std::to_string(stalls * 2) +
                       "s, received=" + std::to_string(p) +
                       (g_state ? ", " + g_state() : std::string()) +
                       "\nlost notification\n";
            auto r = write(2, msg.data(), msg.size());
            (void)r;
            _exit(2);
        }
    }).detach();
}

struct Deadline {
    std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now() + std::chrono::seconds(DEADLINE_SEC);
    bool expired() const { return std::chrono::steady_clock::now() > end; }
};

// Waits for `done` while failing the test rather than stalling forever. The
// caller is never a parked consumer, so blocking the OS thread is fine.
#define AWAIT(done, what) do {                                              \
    Deadline dl;                                                            \
    while (!(done)) {                                                       \
        ASSERT_FALSE(dl.expired()) << "lost notification: " << what;        \
        std::this_thread::yield();                                          \
    }                                                                       \
} while (0)

// Strict ping-pong: the channel is empty again before the next push, so every
// single item has to travel through publish -> claim -> wake. `check_leak`
// makes the consumer verify after each item that no wake-up interrupt of its
// own is still pending -- a leaked one would hit the next sleep of whoever
// called recv().
static void run_pingpong(uint64_t rounds, uint64_t park_usec, bool check_leak) {
    TestChannel ch(park_usec);
    std::atomic<uint64_t> acked{0};
    std::atomic<uint64_t> leaked{0};

    std::thread consumer([&] {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());
        for (;;) {
            auto x = ch.recv(0, 0);     // no spin budget: park on every item
            if (check_leak && photon::thread_usleep(1) < 0)
                leaked.fetch_add(1, std::memory_order_relaxed);
            acked.store(x, std::memory_order_release);
            if (x == SENTINEL) break;
            g_progress.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (uint64_t i = 1; i <= rounds; ++i) {
        ch.send<ThreadPause>(i);
        AWAIT(acked.load(std::memory_order_acquire) == i, "ping-pong ack " << i);
    }
    ch.send<ThreadPause>(SENTINEL);
    consumer.join();

    EXPECT_EQ(0UL, leaked.load());
    EXPECT_EQ(0UL, ch.notification_pending());
}

TEST(ring_channel, pingpong_no_safety_net) {
    g_case = "pingpong_no_safety_net";
    run_pingpong(50000, NO_NET, false);
}

TEST(ring_channel, pingpong_racing_safety_net) {
    g_case = "pingpong_racing_safety_net";
    // 20us: most items are delivered by a claim, but often to a consumer that
    // the safety net has just woken up
    run_pingpong(5000, 20, true);
}

// Many producers against many parked consumers. This is where a claimer, which
// holds the whole idle stack while it gives the surplus back, makes a
// concurrent producer believe that nobody is parked.
static void run_mpmc(uint64_t nprod, uint64_t ncons, uint64_t per_prod) {
    TestChannel ch(NO_NET);
    auto total = nprod * per_prod;
    std::atomic<uint64_t> received{0};
    std::vector<std::thread> cons, prod;
    g_state = [&] {
        return "received=" + std::to_string(received.load()) + "/" +
               std::to_string(total) + " queued=" +
               std::to_string(ch.pending_items());
    };
    DEFER(g_state = nullptr);

    for (uint64_t i = 0; i < ncons; ++i) {
        cons.emplace_back([&] {
            photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
            DEFER(photon::fini());
            for (;;) {
                if (ch.recv(0, 0) == SENTINEL) break;
                received.fetch_add(1, std::memory_order_relaxed);
                g_progress.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // give every consumer the time to park, or the burst would be consumed
    // without any notification at all
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    for (uint64_t p = 0; p < nprod; ++p) {
        prod.emplace_back([&] {
            for (uint64_t i = 0; i < per_prod; ++i) ch.send_paced(i + 1);
        });
    }
    for (auto& t : prod) t.join();
    AWAIT(received.load(std::memory_order_relaxed) >= total, "mpmc drain");

    for (uint64_t i = 0; i < ncons; ++i) ch.send<ThreadPause>(SENTINEL);
    for (auto& t : cons) t.join();

    EXPECT_EQ(total, received.load());
    EXPECT_EQ(0UL, ch.notification_pending());
}

TEST(ring_channel, mpmc_burst_no_safety_net) {
    g_case = "mpmc_burst_no_safety_net";
    run_mpmc(4, 4, 8000);
}

TEST(ring_channel, single_producer_many_consumers) {
    g_case = "single_producer_many_consumers";
    // one producer can only ever hand out one wake-up at a time, so the baton
    // has to be passed along the consumers
    run_mpmc(1, 8, 20000);
}

// An unrelated thread_interrupt() on a parked consumer must not swallow an
// item: the consumer has to re-arm its slot before it sleeps again, otherwise
// the next producer would find it parked and wake nobody.
TEST(ring_channel, external_interrupt_keeps_slot_armed) {
    g_case = "external_interrupt_keeps_slot_armed";
    TestChannel ch(NO_NET);
    constexpr uint64_t kItems = 20000;
    std::atomic<uint64_t> received{0};
    std::atomic<bool> stop{false};
    g_state = [&] { return "received=" + std::to_string(received.load()); };
    DEFER(g_state = nullptr);

    std::thread consumer([&] {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());
        auto self = photon::CURRENT;
        auto pest = photon::thread_create11([&, self] {
            while (!stop.load(std::memory_order_relaxed)) {
                photon::thread_interrupt(self, EINTR);
                photon::thread_usleep(10);
            }
        });
        photon::thread_enable_join(pest);
        for (;;) {
            if (ch.recv(0, 0) == SENTINEL) break;
            received.fetch_add(1, std::memory_order_relaxed);
            g_progress.fetch_add(1, std::memory_order_relaxed);
        }
        stop.store(true, std::memory_order_relaxed);
        photon::thread_join((photon::join_handle*)pest);
    });

    for (uint64_t i = 1; i <= kItems; ++i) ch.send_paced(i);
    AWAIT(received.load(std::memory_order_relaxed) >= kItems, "interrupted recv");
    ch.send<ThreadPause>(SENTINEL);
    consumer.join();

    EXPECT_EQ(kItems, received.load());
    EXPECT_EQ(0UL, ch.notification_pending());
}

// Producer and consumer on the same vCPU: a claim never needs to kick an event
// engine, and the producer regularly claims the slot of a consumer that is not
// asleep yet -- or even its own slot, when the queue turns out to be non-empty
// right after publishing it.
TEST(ring_channel, same_vcpu_pairing) {
    g_case = "same_vcpu_pairing";
    TestChannel ping(NO_NET), pong(NO_NET);
    constexpr uint64_t kRounds = 20000;

    auto consumer = photon::thread_create11([&] {
        for (;;) {
            auto x = ping.recv(0, 0);
            pong.send<PhotonPause>(x);
            if (x == SENTINEL) break;
        }
    });
    photon::thread_enable_join(consumer);

    for (uint64_t i = 1; i <= kRounds; ++i) {
        ping.send<PhotonPause>(i);
        ASSERT_EQ(i, pong.recv(0, 0));
        g_progress.fetch_add(1, std::memory_order_relaxed);
    }
    ping.send<PhotonPause>(SENTINEL);
    EXPECT_EQ(SENTINEL, pong.recv(0, 0));
    photon::thread_join((photon::join_handle*)consumer);

    EXPECT_EQ(0UL, ping.notification_pending());
    EXPECT_EQ(0UL, pong.notification_pending());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    start_watchdog();
    return RUN_ALL_TESTS();
}
