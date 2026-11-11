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

// Hunts for waiters of photon::semaphore that never wake up. Such a bug is not a wrong
// number, it is a hang: every case therefore runs under a deadline and fails loudly
// instead of stalling, and every case is repeated, since the windows are only a few
// instructions wide.
//
// The shapes that hurt:
//   - many vCPUs signalling one count at a time against many waiters, where the supply
//     matches the demand exactly, so a single missed wake-up hangs the case (stream);
//   - waiters requiring different counts, which is what puts the queue head out of reach
//     of the count in hand and drives the out-of-order resume path (mixed_counts, ooo);
//   - waiters leaving by timeout or interruption, which removes them from the queue
//     without the semaphore's spinlock (timeouts, interrupts).

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/thread/thread11.h>
#include "../../test/gtest.h"

using namespace photon;
using clk = std::chrono::steady_clock;

// generous enough to never trip on a loaded CI machine, yet far below any real hang: the
// heaviest case here takes tens of milliseconds
static uint64_t DEADLINE_SEC = 60;

// dumped when a deadline expires, to tell a genuine lost wake-up (count sitting in the
// semaphore while everybody sleeps) from a mere accounting mistake in the test
static std::function<std::string()> g_state;
static std::atomic<uint64_t> g_case_seq{0};     // bumped whenever a case finishes
static const char* g_case_name = "";

// The in-coroutine deadline below can only fire if the photon scheduler still runs the
// main thread. Under a spinlock convoy it may not, so the ultimate watchdog has to be a
// plain OS thread, independent of anything photon does.
static void start_watchdog() {
    std::thread([] {
        for (uint64_t last = 0, stalls = 0;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            auto seq = g_case_seq.load();
            if (seq != last) { last = seq; stalls = 0; continue; }
            if (++stalls * 5 < DEADLINE_SEC) continue;
            // ALOG may be blocked behind a lock held by a spinning thread, so use write()
            auto st = g_state ? g_state() : std::string();
            auto msg = std::string("\nWATCHDOG: case '") + g_case_name +
                       "' made no progress in " + std::to_string(stalls * 5) +
                       "s, state: " + st + "\n";
            auto r = write(2, msg.data(), msg.size());
            (void)r;
            _exit(2);
        }
    }).detach();
}

struct Deadline {
    clk::time_point end = clk::now() + std::chrono::seconds(DEADLINE_SEC);
    bool expired() const { return clk::now() > end; }
};

// spins photon threads until `done` turns true, and reports a hang as a test failure
// rather than letting the suite stall forever
#define AWAIT(done, what) do {                                                  \
    Deadline dl;                                                                \
    while (!(done)) {                                                           \
        ASSERT_FALSE(dl.expired()) << "lost wake-up: " << what << " stuck, "     \
                                   << (g_state ? g_state() : std::string());    \
        photon::thread_yield();                                                 \
    }                                                                           \
} while (0)

// A pack of OS threads hammering signal() on a shared semaphore. They are plain std
// threads turned into vCPUs, which is how the WorkPool notifies its workers.
struct Signalers {
    std::vector<std::thread> ths;
    std::atomic<uint64_t> ready{0};
    std::atomic<bool> go{false};
    std::atomic<uint64_t> signaled{0};      // total count handed to the semaphore

    template <typename Fn>
    void start(uint64_t n, Fn fn) {
        for (uint64_t i = 0; i < n; ++i) {
            ths.emplace_back([this, i, fn] {
                photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
                DEFER(photon::fini());
                ready++;
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                fn(i);
            });
        }
        while (ready.load() < n) photon::thread_yield();
        go.store(true, std::memory_order_release);
    }

    void join() {
        for (auto& th : ths) th.join();
        ths.clear();
    }
};

// N vCPUs signal 1 at a time; M waiters each consume a fixed quota of 1. The sum matches
// exactly, so any single lost wake-up leaves a waiter asleep forever.
void run_stream(uint64_t nsig, uint64_t ops, uint64_t nwait, bool in_order) {
    g_case_name = in_order ? "stream_in_order" : "stream_ooo";
    semaphore sem(0, in_order);
    auto total = nsig * ops;
    std::atomic<uint64_t> consumed{0};
    volatile uint64_t live = nwait;
    for (uint64_t i = 0; i < nwait; ++i) {
        auto quota = total / nwait + (i < total % nwait);
        thread_create11([&, quota] {
            for (uint64_t k = 0; k < quota; ++k) {
                ASSERT_EQ(0, sem.wait(1));
                consumed++;
                g_case_seq++;
            }
            live = live - 1;
        });
    }
    thread_yield();     // let them all reach wait()

    Signalers sig;
    sig.start(nsig, [&](uint64_t) {
        for (uint64_t k = 0; k < ops; ++k) sem.signal(1);
    });
    AWAIT(!live, "stream waiter");
    sig.join();
    EXPECT_EQ(total, consumed.load());
    EXPECT_EQ(0UL, sem.count());
}

// The out-of-order branch of try_resume() in isolation, and by construction rather than
// by chance: the head of the queue demands more than what is signalled, so an in-order
// semaphore would keep everybody waiting, while an ooo one must reach past the head and
// serve the smaller waiter behind it. A single vCPU is enough, which makes it fully
// deterministic -- and it used to deadlock every single time.
void run_ooo_bypass() {
    g_case_name = "ooo_bypass";
    semaphore sem(0, false);
    std::atomic<uint64_t> got_big{0}, got_small{0};
    thread_create11([&] { sem.wait(10); got_big++; });
    thread_yield();     // it queues up first, hence becomes the head
    thread_create11([&] { sem.wait(1); got_small++; });
    thread_yield();
    sem.signal(1);      // satisfies the second waiter only
    AWAIT(got_small.load(), "ooo waiter behind a bigger one");
    EXPECT_EQ(0UL, got_big.load());
    EXPECT_EQ(0UL, sem.count());
    sem.signal(10);     // the head is still there, and its turn has come
    AWAIT(got_big.load(), "ooo queue head");
    EXPECT_EQ(0UL, sem.count());
    g_case_seq++;
}

// The nastiest shape: waiters requiring different counts, handed out in units of 1. The
// queue head is therefore out of reach most of the time, which is exactly what an
// out-of-order semaphore has to look past, and what an in-order one has to hold the line
// on until the head's own demand is met.
void run_mixed_counts(uint64_t nsig, uint64_t rounds, bool in_order) {
    g_case_name = in_order ? "mixed_counts_in_order" : "mixed_counts_ooo";
    // requirements 1..8, repeated: the queue head keeps changing its demand
    static const uint64_t reqs[] = {1, 4, 2, 8, 3, 1, 5, 2};
    const uint64_t nwait = sizeof(reqs) / sizeof(reqs[0]);
    uint64_t per_round = 0;
    for (auto r : reqs) per_round += r;

    semaphore sem(0, in_order);
    std::atomic<uint64_t> consumed{0};
    volatile uint64_t live = nwait;
    std::vector<std::atomic<uint64_t>> progress(nwait);  // rounds finished, per waiter
    for (auto& p : progress) p.store(0);
    auto total = per_round * rounds;
    g_state = [&, total] {
        std::ostringstream os;
        os << "count=" << sem.count() << " consumed=" << consumed.load()
           << "/" << total << " live=" << live << " rounds_done=";
        for (uint64_t i = 0; i < nwait; ++i)
            os << reqs[i] << ":" << progress[i].load() << " ";
        return os.str();
    };
    DEFER(g_state = nullptr);
    for (uint64_t i = 0; i < nwait; ++i) {
        auto need = reqs[i];
        thread_create11([&, need, i] {
            for (uint64_t k = 0; k < rounds; ++k) {
                ASSERT_EQ(0, sem.wait(need));
                consumed += need;
                progress[i].store(k + 1, std::memory_order_relaxed);
                g_case_seq++;       // feed the watchdog: real progress was made
            }
            live = live - 1;
        });
    }
    thread_yield();

    // the signalers together hand out exactly what the waiters need, in units of 1, so
    // the count spends most of its time below the largest requirement. The quota is fixed
    // per signaler rather than claimed from a shared counter, so that the supply matches
    // the demand exactly and no signaler can outlive the test.
    Signalers sig;
    sig.start(nsig, [&, total, nsig](uint64_t i) {
        auto quota = total / nsig + (i < total % nsig);
        for (uint64_t k = 0; k < quota; ++k) sem.signal(1);
    });
    AWAIT(!live, "mixed-count waiter");
    sig.join();
    EXPECT_EQ(total, consumed.load());
    EXPECT_EQ(0UL, sem.count());
}

// Waiters that keep timing out join and leave the queue constantly, and they do so without
// the semaphore's spinlock. A signaler may thus find the queue empty at any moment, and
// must still not lose the count that the waiters eventually deserve.
void run_timeouts(uint64_t nsig, uint64_t ops, uint64_t nwait) {
    g_case_name = "timeouts";
    semaphore sem(0);
    auto total = nsig * ops;
    std::atomic<uint64_t> consumed{0};
    volatile uint64_t live = nwait;
    for (uint64_t i = 0; i < nwait; ++i) {
        auto quota = total / nwait + (i < total % nwait);
        thread_create11([&, quota] {
            for (uint64_t k = 0; k < quota; ++k) {
                // a short timeout makes most attempts bail out and re-enter, so the
                // waiter is constantly joining and leaving the queue
                while (sem.wait(1, 200) < 0) {
                    EXPECT_EQ(ETIMEDOUT, errno);
                }
                consumed++;
                g_case_seq++;
            }
            live = live - 1;
        });
    }
    thread_yield();

    Signalers sig;
    sig.start(nsig, [&](uint64_t) {
        for (uint64_t k = 0; k < ops; ++k) sem.signal(1);
    });
    AWAIT(!live, "timing-out waiter");
    sig.join();
    EXPECT_EQ(total, consumed.load());
    EXPECT_EQ(0UL, sem.count());
}

// A single waiter demanding the whole sum: it stays unsatisfiable until the very last
// signal arrives, so the count accumulates across thousands of signals and the wake-up
// hinges on that one last increment.
void run_batch(uint64_t nsig, uint64_t ops) {
    g_case_name = "batch";
    semaphore sem(0);
    auto total = nsig * ops;
    volatile bool done = false;
    g_state = [&] {
        std::ostringstream os;
        os << "count=" << sem.count() << "/" << total;
        return os.str();
    };
    DEFER(g_state = nullptr);
    thread_create11([&] {
        ASSERT_EQ(0, sem.wait(total));
        done = true;
    });
    thread_yield();

    Signalers sig;
    sig.start(nsig, [&](uint64_t) {
        // the only progress here is the signalling itself, so it feeds the watchdog
        for (uint64_t k = 0; k < ops; ++k) {
            sem.signal(1);
            if ((k & 4095) == 0) g_case_seq++;
        }
    });
    AWAIT(done, "batch waiter");
    sig.join();
    EXPECT_EQ(0UL, sem.count());
}

// One waiter is interrupted over and over while others are being served: the interrupted
// one leaves with the count possibly already in the semaphore, and is responsible for
// handing it over to whoever is still queued.
void run_interrupts(uint64_t nsig, uint64_t ops) {
    g_case_name = "interrupts";
    semaphore sem(0);
    auto total = nsig * ops;
    std::atomic<uint64_t> consumed{0};
    volatile uint64_t live = 2;
    volatile bool stop = false;

    // the victim keeps getting interrupted, and never consumes anything
    auto victim = thread_create11([&] {
        while (!stop) {
            if (sem.wait_interruptible(1000000000) == 0)
                consumed += 1000000000;     // must never happen
        }
        live = live - 1;
    });
    // the worker consumes everything, one at a time
    thread_create11([&] {
        for (uint64_t k = 0; k < total; ++k) {
            ASSERT_EQ(0, sem.wait(1));
            consumed++;
            g_case_seq++;
        }
        live = live - 1;
    });
    thread_yield();

    Signalers sig;
    sig.start(nsig, [&](uint64_t) {
        for (uint64_t k = 0; k < ops; ++k) sem.signal(1);
    });
    // keep kicking the victim out of the queue while the count flows through
    Deadline dl;
    while (consumed.load() < total) {
        ASSERT_FALSE(dl.expired()) << "lost wake-up: interrupted worker stuck";
        thread_interrupt(victim, EINTR);
        thread_yield();
    }
    stop = true;
    thread_interrupt(victim, EINTR);
    AWAIT(!live, "interrupt victim");
    sig.join();
    EXPECT_EQ(total, consumed.load());
    EXPECT_EQ(0UL, sem.count());
}

TEST(SemaphoreLostWakeup, stream_in_order) {
    for (int r = 0; r < 5; ++r) run_stream(8, 20000, 4, true);
}

TEST(SemaphoreLostWakeup, stream_ooo) {
    for (int r = 0; r < 5; ++r) run_stream(8, 20000, 4, false);
}

TEST(SemaphoreLostWakeup, stream_single_waiter) {
    for (int r = 0; r < 5; ++r) run_stream(8, 20000, 1, true);
}

TEST(SemaphoreLostWakeup, mixed_counts_in_order) {
    for (int r = 0; r < 5; ++r) run_mixed_counts(8, 2000, true);
}

TEST(SemaphoreLostWakeup, ooo_bypass) {
    for (int r = 0; r < 5; ++r) run_ooo_bypass();
}

// This case is the first one to ever enter the out-of-order branch of
// semaphore::try_resume(): the branch requires both a semaphore constructed with
// in_order_resume = false (nothing in the tree does, the default is in-order, which is why
// even throttle's mixed-count traffic never went there) and a queue head demanding more
// than the count in hand. It used to deadlock on itself, as it resumed a waiter while
// holding the wait queue's lock, which prelocked_thread_interrupt() locks again.
TEST(SemaphoreLostWakeup, mixed_counts_ooo) {
    for (int r = 0; r < 5; ++r) run_mixed_counts(8, 2000, false);
}

TEST(SemaphoreLostWakeup, timeouts) {
    for (int r = 0; r < 3; ++r) run_timeouts(4, 5000, 4);
}

TEST(SemaphoreLostWakeup, batch) {
    for (int r = 0; r < 5; ++r) run_batch(8, 20000);
}

TEST(SemaphoreLostWakeup, interrupts) {
    for (int r = 0; r < 3; ++r) run_interrupts(4, 5000);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    set_log_output_level(ALOG_INFO);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE) != 0)
        return -1;
    DEFER(photon::fini());
    start_watchdog();
    return RUN_ALL_TESTS();
}
