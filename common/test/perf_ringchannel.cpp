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

// Micro-benchmark of the RingChannel *notification* path, i.e. what happens
// when a consumer has to go idle and be woken up again. The spin budget is set
// to zero in most cases (recv(0, 0)) so that every single item pays the full
// park + wake-up cost, which is exactly the part that a park-slot design
// replaces.
//
// Cases:
//   1. wake_latency  -- one producer, one always-idle consumer, strict
//                       ping-pong. Measures the round-trip of
//                       "push + notify + wake + pop".
//   2. fanout        -- N idle consumers on N vCPUs, bursts of N items.
//                       Measures how fast a burst spreads over all consumers.
//   3. hot_send      -- no consumer at all: measures the producer's fast path
//                       (the barrier + idle check that every send pays).
//   4. steady        -- one producer, one consumer with the default spin
//                       budget, no artificial pacing: end-to-end throughput.
//   5. same_vcpu     -- producer and consumer are photon threads of the *same*
//                       vCPU, so a wake-up needs no eventfd/epoll kick at all.
//                       This is the purest measure of the notification
//                       bookkeeping itself.
//   6. mp_contend    -- P producer OS threads against C consumers that park on
//                       every item: measures how well the notification path
//                       scales when many producers notify concurrently.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <photon/common/alog.h>
#include <photon/common/lockfree_queue.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

DEFINE_uint64(rounds, 100000, "iterations per case");
DEFINE_uint64(consumers, 4, "consumer num of the fan-out case");
DEFINE_uint64(producers, 4, "producer num of the contention case");

using Queue = LockfreeMPMCRingQueue<uint64_t, 4096>;
using Channel = photon::common::RingChannel<Queue>;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 1. strict ping-pong against an always-idle consumer.
static void case_wake_latency() {
    Channel ch;
    std::atomic<uint64_t> acked{0};
    std::atomic<bool> stop{false};

    std::thread consumer([&] {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());
        for (;;) {
            auto x = ch.recv(0, 0);  // no spinning: always park
            if (x == 0) break;
            acked.store(x, std::memory_order_release);
        }
    });

    auto start = now_ns();
    for (uint64_t i = 1; i <= FLAGS_rounds; ++i) {
        ch.send<ThreadPause>(i);
        // Spin (not sleep) on the ack, so the measured time is the channel's
        // wake-up path and not this thread's own scheduling.
        while (acked.load(std::memory_order_acquire) != i) CPUPause::pause();
    }
    auto cost = now_ns() - start;
    stop.store(true);
    ch.send<ThreadPause>(0);
    consumer.join();

    LOG_INFO("wake_latency : ` round-trips, ` ns/round-trip",
             FLAGS_rounds, cost / FLAGS_rounds);
}

// 2. burst of N items against N idle consumers, N vCPUs.
static void case_fanout() {
    Channel ch;
    auto n = FLAGS_consumers;
    std::atomic<uint64_t> done{0};
    std::vector<std::thread> consumers;
    for (uint64_t i = 0; i < n; ++i) {
        consumers.emplace_back([&] {
            photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
            DEFER(photon::fini());
            for (;;) {
                auto x = ch.recv(0, 0);
                if (x == 0) break;
                done.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }
    // let every consumer reach its idle state
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    uint64_t bursts = FLAGS_rounds / n;
    auto start = now_ns();
    for (uint64_t b = 0; b < bursts; ++b) {
        auto target = (b + 1) * n;
        for (uint64_t i = 0; i < n; ++i) ch.send<ThreadPause>(b * n + i + 1);
        while (done.load(std::memory_order_acquire) < target) CPUPause::pause();
    }
    auto cost = now_ns() - start;
    for (uint64_t i = 0; i < n; ++i) ch.send<ThreadPause>(0);
    for (auto& t : consumers) t.join();

    LOG_INFO("fanout       : ` bursts of `, ` ns/burst, ` ns/item",
             bursts, n, cost / bursts, cost / (bursts * n));
}

// 3. producer fast path, nobody is waiting on the other end.
static void case_hot_send() {
    Channel ch;
    uint64_t rounds = FLAGS_rounds;
    auto start = now_ns();
    // The queue holds 4096 entries; drain it in place (single thread, so no
    // notification is ever needed) to keep measuring send() only.
    for (uint64_t i = 0; i < rounds; ++i) {
        ch.send<ThreadPause>(i + 1);
        uint64_t x;
        ch.pop(x);
    }
    auto cost = now_ns() - start;
    LOG_INFO("hot_send     : ` sends, ` ns/send (no idle consumer)",
             rounds, cost / rounds);
}

// 4. steady state with the default spin budget.
static void case_steady() {
    Channel ch;
    std::atomic<uint64_t> received{0};
    std::thread consumer([&] {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());
        for (;;) {
            auto x = ch.recv();
            if (x == 0) break;
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto start = now_ns();
    for (uint64_t i = 1; i <= FLAGS_rounds; ++i) ch.send<ThreadPause>(i);
    while (received.load(std::memory_order_relaxed) < FLAGS_rounds)
        CPUPause::pause();
    auto cost = now_ns() - start;
    ch.send<ThreadPause>(0);
    consumer.join();

    LOG_INFO("steady       : ` items, ` ns/item, QPS `", FLAGS_rounds,
             cost / FLAGS_rounds, FLAGS_rounds * 1000000000ULL / cost);
}

// 5. producer and consumer live on the same vCPU: no OS-level wake-up is
// involved, so what remains is exactly the notification bookkeeping plus two
// coroutine context switches.
static void case_same_vcpu() {
    Channel ping, pong;
    uint64_t rounds = FLAGS_rounds;

    auto consumer = photon::thread_create11([&] {
        for (;;) {
            auto x = ping.recv(0, 0);   // no spinning: always park
            pong.send<PhotonPause>(x);
            if (x == 0) break;
        }
    });
    photon::thread_enable_join(consumer);

    auto start = now_ns();
    for (uint64_t i = 1; i <= rounds; ++i) {
        ping.send<PhotonPause>(i);
        auto x = pong.recv(0, 0);
        if (x != i) LOG_ERROR("unexpected `, want `", x, i);
    }
    auto cost = now_ns() - start;
    ping.send<PhotonPause>(0);
    pong.recv(0, 0);
    photon::thread_join((photon::join_handle*)consumer);

    LOG_INFO("same_vcpu    : ` round-trips, ` ns/round-trip (2 park+wake each)",
             rounds, cost / rounds);
}

// 6. many producers notifying concurrently, consumers park on every item.
static void case_mp_contend() {
    Channel ch;
    auto np = FLAGS_producers, nc = FLAGS_consumers;
    std::atomic<uint64_t> done{0};
    std::vector<std::thread> consumers, producers;
    for (uint64_t i = 0; i < nc; ++i) {
        consumers.emplace_back([&] {
            photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
            DEFER(photon::fini());
            for (;;) {
                auto x = ch.recv(0, 0);
                if (x == 0) break;
                done.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    uint64_t per = FLAGS_rounds / np, total = per * np;
    auto start = now_ns();
    for (uint64_t p = 0; p < np; ++p) {
        producers.emplace_back([&, p] {
            for (uint64_t i = 0; i < per; ++i) ch.send<ThreadPause>(p * per + i + 1);
        });
    }
    for (auto& t : producers) t.join();
    while (done.load(std::memory_order_relaxed) < total) CPUPause::pause();
    auto cost = now_ns() - start;
    for (uint64_t i = 0; i < nc; ++i) ch.send<ThreadPause>(0);
    for (auto& t : consumers) t.join();

    LOG_INFO("mp_contend   : ` producers x ` consumers, ` items, ` ns/item",
             np, nc, total, cost / total);
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_log_output_level(ALOG_INFO);
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());

    case_hot_send();
    case_same_vcpu();
    case_wake_latency();
    case_steady();
    case_fanout();
    case_mp_contend();
    return 0;
}
