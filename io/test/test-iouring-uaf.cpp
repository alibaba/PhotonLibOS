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

// Regression tests for issue #1270: use-after-free on the early-return paths
// of iouringEngine::_async_io. The stack-allocated ioCtx must not go out of
// scope while its CQE is still in flight, even when the waiting thread gets
// interrupted by an external OS thread.

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/photon.h>
#include "../../test/gtest.h"

// io_uring requires Linux kernel >= 5.1. Prints a skip notice and returns
// false if unavailable. gtest 1.8 has no GTEST_SKIP, so callers must
// `return` on false.
static bool iouring_init() {
    if (photon::init(photon::INIT_EVENT_IOURING, photon::INIT_IO_NONE) != 0) {
        fprintf(stderr, "  [ SKIPPED ] io_uring is not supported on this kernel\n");
        return false;
    }
    return true;
}

// Sanity check: the normal success/timeout paths are untouched by the fix.
TEST(iouring_uaf, sanity) {
    if (!iouring_init())
        return;
    DEFER(photon::fini());

    int fds[2];
    ASSERT_EQ(0, ::pipe(fds));
    DEFER({ ::close(fds[0]); ::close(fds[1]); });

    // Timeout path
    ASSERT_EQ(-1, photon::wait_for_fd_readable(fds[0], 1000));
    ASSERT_EQ(ETIMEDOUT, errno);

    // Success path
    char buf[1] = {};
    ::write(fds[1], buf, 1);
    ASSERT_EQ(0, photon::wait_for_fd_readable(fds[0], 1000 * 1000));
}

// An external std::thread interrupts a photon thread that keeps issuing
// timed iouring I/O. Each interrupt drives _async_io into the cancel branch,
// whose wait loop must survive premature wake-ups and only return after all
// CQEs referring to the stack contexts have been reaped. Before the fix this
// scenario corrupts the photon thread's stack (typically crashing or hanging).
TEST(iouring_uaf, interrupt_storm) {
    if (!iouring_init())
        return;
    DEFER(photon::fini());

    int fds[2];
    ASSERT_EQ(0, ::pipe(fds));
    DEFER({ ::close(fds[0]); ::close(fds[1]); });

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> rounds{0};
    auto th = photon::thread_create11([&] {
        while (!stop.load(std::memory_order_acquire)) {
            // 1ms timeout, so the linked timer SQE is always present and the
            // interrupt races against submit/complete/timeout at all phases
            photon::wait_for_fd_readable(fds[0], 1000);
            rounds.fetch_add(1, std::memory_order_relaxed);
        }
    });
    photon::thread_enable_join(th);

    std::thread interrupter([&] {
        for (int i = 0; i < 5000; ++i) {
            photon::thread_interrupt(th);
            // Vary the interrupt timing to hit different interleavings
            if (i % 4 == 0)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        // The last interrupt happens-before this store, so `th` is still
        // alive whenever thread_interrupt touches it
        stop.store(true, std::memory_order_release);
    });
    photon::thread_join((photon::join_handle*) th);
    interrupter.join();
    LOG_INFO("survived ` rounds of interrupted I/O", rounds.load());
}

// Same storm as above, but through multiple concurrent photon threads, so
// that io/cancel/timer CQEs of different requests interleave in reap batches.
TEST(iouring_uaf, interrupt_storm_multi_threads) {
    if (!iouring_init())
        return;
    DEFER(photon::fini());

    constexpr int kThreads = 8;
    int fds[2];
    ASSERT_EQ(0, ::pipe(fds));
    DEFER({ ::close(fds[0]); ::close(fds[1]); });

    std::atomic<bool> stop{false};
    photon::thread* workers[kThreads];
    for (int i = 0; i < kThreads; ++i) {
        workers[i] = photon::thread_create11([&] {
            while (!stop.load(std::memory_order_acquire))
                photon::wait_for_fd_readable(fds[0], 1000);
        });
        photon::thread_enable_join(workers[i]);
    }

    std::thread interrupter([&] {
        for (int i = 0; i < 5000; ++i) {
            photon::thread_interrupt(workers[i % kThreads]);
            if (i % 4 == 0)
                std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        stop.store(true, std::memory_order_release);
    });
    for (auto* w : workers)
        photon::thread_join((photon::join_handle*) w);
    interrupter.join();
}

// Interrupt an in-flight I/O whose timeout is far in the future, then join
// the thread and go through photon::fini. The join can only return after the
// cancel-path wait loop has seen all its CQEs reaped, which guarantees the
// engine is drained before it gets destroyed by fini. Note that calling fini
// while a thread is still inside _async_io is not a supported usage, since
// fini deletes the master engine before waiting for the remaining threads.
TEST(iouring_uaf, shutdown_with_interrupted_inflight_io) {
    if (!iouring_init())
        return;

    int fds[2];
    ASSERT_EQ(0, ::pipe(fds));

    auto th = photon::thread_create11([&] {
        // Long timeout: only the interrupt below can terminate this I/O
        photon::wait_for_fd_readable(fds[0], 100ULL * 1000 * 1000);
    });
    photon::thread_enable_join(th);
    photon::thread_yield();     // let the I/O get prepared and submitted
    photon::thread_interrupt(th);
    photon::thread_join((photon::join_handle*) th);

    ::close(fds[0]);
    ::close(fds[1]);
    ASSERT_EQ(0, photon::fini());
}

int main(int argc, char** argv) {
    set_log_output_level(ALOG_INFO);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
