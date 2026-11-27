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

// Work-stealing scheduler microbenchmark suite:
//   yield_no_ws            -> 2 fibers switching on a vCPU without WS flags
//   yield_passive_ws       -> 2 stealable fibers switching on a passive-WS vCPU
//   single_under_stealers  -> 1 paused fiber + idler on passive-WS vCPU with active stealers
//   paused_under_stealers  -> 2 paused fibers switching on passive-WS vCPU with active stealers
//   scoped_pause_nested    -> outer + inner SCOPED_PAUSE_WORK_STEALING guard overhead
//   batch_steal            -> multi-vCPU batch task creation + active stealing throughput

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/stack-allocator.h>
#include <photon/thread/thread.h>

namespace photon {
void thread_yield_fast();
}

using namespace photon;

DEFINE_string(pattern, "all",
              "yield_no_ws | yield_passive_ws | single_under_stealers | "
              "paused_under_stealers | scoped_pause_nested | batch_steal | all");
DEFINE_uint64(iters, 2000000, "iterations per microbenchmark");
DEFINE_uint64(batch_tasks, 200000, "total tasks for batch_steal");
DEFINE_uint64(batch_size, 64, "tasks per batch for batch_steal");
DEFINE_uint64(stealers, 4, "number of background active+passive stealer vCPUs");
DEFINE_uint64(runs, 5, "repetitions per case (reports median ns/op)");

using clk = std::chrono::steady_clock;

static uint64_t elapsed_ns(clk::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clk::now() - start)
        .count();
}

static bool want(const char* name) {
    return FLAGS_pattern == "all" || FLAGS_pattern == name;
}

template <typename Fn>
static void run_median(const char* name, uint64_t ops_per_run, Fn&& fn) {
    fn(); // warmup
    std::vector<double> samples;
    samples.reserve(FLAGS_runs);
    for (uint64_t r = 0; r < FLAGS_runs; r++) {
        uint64_t ns = fn();
        samples.push_back((double)ns / (double)ops_per_run);
    }
    std::sort(samples.begin(), samples.end());
    double med = samples[samples.size() / 2];
    double min_v = samples.front();
    double max_v = samples.back();
    double mops = 1000.0 / (med > 0.0 ? med : 1e-9);
    std::printf("%-32s median=%6.2f ns/op  (min=%6.2f, max=%6.2f)  %7.2f Mops/s\n",
                name, med, min_v, max_v, mops);
    std::fflush(stdout);
}

struct BenchStealerPool {
    std::vector<std::thread> threads;
    std::atomic<uint64_t> ready{0};
    std::atomic<bool> stop{false};

    explicit BenchStealerPool(
        size_t n,
        uint64_t flags = photon::VCPU_ENABLE_ACTIVE_WORK_STEALING |
                         photon::VCPU_ENABLE_PASSIVE_WORK_STEALING) {
        threads.reserve(n);
        for (size_t i = 0; i < n; i++) {
            threads.emplace_back([this, flags] {
                photon::vcpu_init(flags);
                DEFER(photon::vcpu_fini());
                ready.fetch_add(1, std::memory_order_release);
                while (!stop.load(std::memory_order_acquire)) {
                    photon::thread_usleep(1);
                }
            });
        }
        while (ready.load(std::memory_order_acquire) < n) {
            ::usleep(50);
        }
    }

    ~BenchStealerPool() {
        stop.store(true, std::memory_order_release);
        for (auto& t : threads) {
            t.join();
        }
    }
};

enum YieldKind { YIELD_FULL, YIELD_FAST, YIELD_TO };

static void bench_two_fiber_yield(const char* label, uint64_t vcpu_flags,
                                  uint64_t thread_flags, YieldKind kind,
                                  bool pause_ws = false,
                                  size_t active_stealers = 0) {
    std::thread host([&] {
        photon::vcpu_init(vcpu_flags);
        DEFER(photon::vcpu_fini());

        std::unique_ptr<BenchStealerPool> pool;

        struct PairState {
            photon::thread* th_a = nullptr;
            photon::thread* th_b = nullptr;
            photon::semaphore start_a{0};
            photon::semaphore done_run{0};
            uint64_t half_iters = 0;
            uint64_t last_ns = 0;
            YieldKind kind = YIELD_FULL;
            bool pause_ws = false;
            bool go = false;
            bool quit = false;
        } st;
        st.half_iters = FLAGS_iters / 2;
        st.kind = kind;
        st.pause_ws = pause_ws;

        st.th_b = photon::thread_create(
            +[](void* arg) -> void* {
                auto s = (PairState*)arg;
                if (s->pause_ws)
                    photon::thread_pause_work_stealing(true);
                while (!s->quit) {
                    if (!s->go) {
                        photon::thread_yield_fast();
                        continue;
                    }
                    s->go = false;
                    uint64_t n = s->half_iters;
                    if (s->kind == YIELD_FULL) {
                        for (uint64_t i = 0; i < n; i++)
                            photon::thread_yield();
                    } else if (s->kind == YIELD_FAST) {
                        for (uint64_t i = 0; i < n; i++)
                            photon::thread_yield_fast();
                    } else {
                        auto target = s->th_a;
                        for (uint64_t i = 0; i < n; i++)
                            photon::thread_yield_to(target);
                    }
                }
                if (s->pause_ws)
                    photon::thread_pause_work_stealing(false);
                return nullptr;
            },
            &st, 64 * 1024, 0, thread_flags | photon::THREAD_JOINABLE);

        st.th_a = photon::thread_create(
            +[](void* arg) -> void* {
                auto s = (PairState*)arg;
                if (s->pause_ws)
                    photon::thread_pause_work_stealing(true);
                for (;;) {
                    s->start_a.wait(1);
                    if (s->quit)
                        break;
                    s->go = true;
                    auto t0 = clk::now();
                    uint64_t n = s->half_iters;
                    if (s->kind == YIELD_FULL) {
                        for (uint64_t i = 0; i < n; i++)
                            photon::thread_yield();
                    } else if (s->kind == YIELD_FAST) {
                        for (uint64_t i = 0; i < n; i++)
                            photon::thread_yield_fast();
                    } else {
                        auto target = s->th_b;
                        for (uint64_t i = 0; i < n; i++)
                            photon::thread_yield_to(target);
                    }
                    s->last_ns = elapsed_ns(t0);
                    s->done_run.signal(1);
                }
                if (s->pause_ws)
                    photon::thread_pause_work_stealing(false);
                return nullptr;
            },
            &st, 64 * 1024, 0, thread_flags | photon::THREAD_JOINABLE);

        if (pause_ws) {
            photon::thread_pause_work_stealing(true, st.th_b);
            photon::thread_pause_work_stealing(true, st.th_a);
        }
        if (active_stealers > 0) {
            pool.reset(new BenchStealerPool(active_stealers));
        }
        // For YIELD_TO: 2 switches per loop (th_a <-> th_b).
        // For YIELD_FULL / YIELD_FAST: 3 switches per loop (th_a -> th_b -> idle_worker -> th_a).
        uint64_t switches_per_run =
            (kind == YIELD_TO) ? (st.half_iters * 2) : (st.half_iters * 3);

        run_median(label, switches_per_run, [&]() -> uint64_t {
            uint64_t sw0 = photon::get_vcpu()->switch_count;
            st.start_a.signal(1);
            st.done_run.wait(1);
            uint64_t sw1 = photon::get_vcpu()->switch_count;
            if (sw1 - sw0 < switches_per_run) {
                std::fprintf(stderr, "BUG: %s expected >= %lu switches, got %lu\n",
                             label, (unsigned long)switches_per_run,
                             (unsigned long)(sw1 - sw0));
                std::abort();
            }
            return st.last_ns;
        });

        st.quit = true;
        st.start_a.signal(1);
        photon::thread_join((photon::join_handle*)st.th_a);
        photon::thread_join((photon::join_handle*)st.th_b);
    });
    host.join();
}

static void bench_single_under_stealers() {
    std::thread host([&] {
        photon::vcpu_init(photon::VCPU_ENABLE_PASSIVE_WORK_STEALING);
        DEFER(photon::vcpu_fini());

        struct SingleCtx {
            photon::semaphore start_sem{0};
            photon::semaphore done_sem{0};
            uint64_t iters = 0;
            uint64_t ns = 0;
            bool quit = false;
        } ctx;
        ctx.iters = FLAGS_iters / 2;

        auto worker = photon::thread_create(
            +[](void* arg) -> void* {
                auto c = (SingleCtx*)arg;
                SCOPED_PAUSE_WORK_STEALING;
                for (;;) {
                    c->start_sem.wait(1);
                    if (c->quit)
                        break;
                    auto t0 = clk::now();
                    for (uint64_t i = 0; i < c->iters; i++) {
                        // Switches worker -> idle_worker -> worker (2 switches/iter)
                        photon::thread_yield_fast();
                    }
                    c->ns = elapsed_ns(t0);
                    c->done_sem.signal(1);
                }
                return nullptr;
            },
            &ctx, 64 * 1024, 0,
            photon::THREAD_ENABLE_WORK_STEALING | photon::THREAD_JOINABLE);
        photon::thread_yield(); // let worker enter SCOPED_PAUSE_WORK_STEALING and wait on start_sem

        BenchStealerPool pool(FLAGS_stealers);

        // Main fiber sleeps on done_sem, so runq has ONLY {idle_worker, worker}!
        run_median("single_under_stealers", ctx.iters * 2, [&]() -> uint64_t {
            ctx.start_sem.signal(1);
            ctx.done_sem.wait(1);
            return ctx.ns;
        });

        ctx.quit = true;
        ctx.start_sem.signal(1);
        photon::thread_join((photon::join_handle*)worker);
    });
    host.join();
}

__attribute__((noinline))
static void inner_io_like_pause_guard() {
    SCOPED_PAUSE_WORK_STEALING;
    asm volatile("" ::: "memory");
}

static void bench_scoped_pause_nested() {
    std::thread host([&] {
        photon::vcpu_init(photon::VCPU_ENABLE_PASSIVE_WORK_STEALING);
        DEFER(photon::vcpu_fini());

        uint64_t n = FLAGS_iters * 2;
        run_median("scoped_pause_single", n, [&]() -> uint64_t {
            auto t0 = clk::now();
            for (uint64_t i = 0; i < n; i++) {
                inner_io_like_pause_guard();
            }
            return elapsed_ns(t0);
        });

        run_median("scoped_pause_nested", n, [&]() -> uint64_t {
            auto t0 = clk::now();
            for (uint64_t i = 0; i < n; i++) {
                SCOPED_PAUSE_WORK_STEALING;
                inner_io_like_pause_guard();
            }
            return elapsed_ns(t0);
        });
    });
    host.join();
}

static void bench_batch_steal() {
    std::thread host([&] {
        photon::GlobalStackPoolOptions opt;
        opt.max_pooled_bytes = 512ull << 20;
        opt.max_pending_bytes = 128ull << 20;
        opt.max_cold_bytes = 512ull << 20;
        photon::use_global_pooled_stack_allocator(opt);

        photon::vcpu_init(photon::VCPU_ENABLE_PASSIVE_WORK_STEALING);
        DEFER(photon::vcpu_fini());

        BenchStealerPool pool(FLAGS_stealers,
                              photon::VCPU_ENABLE_ACTIVE_WORK_STEALING);

        run_median("batch_steal_tasks", FLAGS_batch_tasks, [&]() -> uint64_t {
            auto t0 = clk::now();
            uint64_t remaining = FLAGS_batch_tasks;
            while (remaining > 0) {
                uint64_t b = std::min<uint64_t>(FLAGS_batch_size, remaining);
                std::atomic<uint64_t> done{0};
                for (uint64_t i = 0; i < b; i++) {
                    photon::thread_create(
                        +[](void* arg) -> void* {
                            auto d = (std::atomic<uint64_t>*)arg;
                            d->fetch_add(1, std::memory_order_release);
                            return nullptr;
                        },
                        &done, 64 * 1024, 0, photon::THREAD_ENABLE_WORK_STEALING);
                }
                while (done.load(std::memory_order_acquire) < b) {
                    photon::spin_wait();
                }
                remaining -= b;
            }
            return elapsed_ns(t0);
        });
    });
    host.join();
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_log_output_level(ALOG_WARN);
    photon::timestamp_updater_init();
    DEFER(photon::timestamp_updater_fini());

    if (want("yield_no_ws")) {
        bench_two_fiber_yield("yield_no_ws/yield", 0, 0, YIELD_FULL);
        bench_two_fiber_yield("yield_no_ws/yield_fast", 0, 0, YIELD_FAST);
        bench_two_fiber_yield("yield_no_ws/yield_to", 0, 0, YIELD_TO);
    }
    if (want("yield_passive_ws")) {
        bench_two_fiber_yield("yield_passive_ws/yield",
                              photon::VCPU_ENABLE_PASSIVE_WORK_STEALING,
                              photon::THREAD_ENABLE_WORK_STEALING, YIELD_FULL);
        bench_two_fiber_yield("yield_passive_ws/yield_fast",
                              photon::VCPU_ENABLE_PASSIVE_WORK_STEALING,
                              photon::THREAD_ENABLE_WORK_STEALING, YIELD_FAST);
        bench_two_fiber_yield("yield_passive_ws/yield_to",
                              photon::VCPU_ENABLE_PASSIVE_WORK_STEALING,
                              photon::THREAD_ENABLE_WORK_STEALING, YIELD_TO);
    }
    if (want("single_under_stealers")) {
        bench_single_under_stealers();
    }
    if (want("paused_under_stealers")) {
        bench_two_fiber_yield("paused_pair_under_stealers",
                              photon::VCPU_ENABLE_PASSIVE_WORK_STEALING,
                              photon::THREAD_ENABLE_WORK_STEALING, YIELD_FAST,
                              /*pause_ws=*/true, FLAGS_stealers);
    }
    if (want("scoped_pause_nested")) {
        bench_scoped_pause_nested();
    }
    if (want("batch_steal")) {
        bench_batch_steal();
    }
    return 0;
}
