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

// Measures photon::semaphore under multi-vCPU contention, where signal() is called
// concurrently by many OS threads on a single shared semaphore. This is the WorkPool
// notification pattern (see perf_workpool.cpp), in which the semaphore easily becomes
// the hot spot of the whole system.
//
// Three shapes are measured, all with N signaler vCPUs firing signal(1):
//   1. batch  : one waiter blocked in a single wait(total) -- nothing is ever resumable,
//               so this measures the pure cost of the signal path.
//   2. stream : M waiters each doing wait(1) in a loop -- every signal may resume one,
//               so this measures the cost of the resume path as well.
//   3. no-wait: no waiter at all -- the baseline of an uncontended counter.
//
// Aggregate throughput alone is misleading here: a signaler that keeps hammering signal()
// back-to-back tends to win its own spinlock again and again, since the cache line is
// still in its L1, while the others are stuck in exponential backoff. That convoy inflates
// the total ops/s while starving everybody else. So --work_ns simulates the work a real
// worker does between two notifications, and the per-vCPU elapsed spread plus the sampled
// signal() latency are reported to expose the time wasted in spinning.

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <gflags/gflags.h>
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/thread/thread11.h>

DEFINE_uint64(signalers, 4, "number of OS threads (vCPUs) calling signal(1)");
DEFINE_uint64(waiters, 4, "number of photon threads calling wait(1), in the stream case");
DEFINE_uint64(ops, 50000, "number of signal(1) calls per signaler");
DEFINE_uint64(work_ns, 0, "nanoseconds of work simulated before each signal(1)");

// one out of every SAMPLE_EVERY signal() calls is timed. Reading the clock costs more
// than an uncontended signal() itself, so sampling has to stay sparse.
static const uint64_t SAMPLE_EVERY = 256;

// a lost wakeup does not show up as a wrong number, it shows up as a hang. Give every
// case a hard deadline, so that such a bug is reported against the case that hit it,
// instead of silently stalling the whole run.
static const uint64_t DEADLINE_SEC = 60;

using clk = std::chrono::steady_clock;

struct Stat {
    uint64_t elapsed_ns = 0;    // the whole signal loop of one signaler
    uint64_t sampled_sum = 0;   // sum of the sampled signal() latencies
    uint64_t sampled_max = 0;   // the worst sampled signal() latency
    uint64_t samples = 0;
};

static photon::semaphore* g_sem;
static std::atomic<bool> g_start{false};
static std::atomic<uint64_t> g_ready{0};
static std::vector<std::thread> g_signalers;
static std::vector<Stat> g_stats;
static bool g_failed = false;

static uint64_t total_ops() { return FLAGS_signalers * FLAGS_ops; }

static uint64_t ns_since(clk::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t).count();
}

static void spin_ns(uint64_t ns) {
    if (!ns) return;
    auto until = clk::now() + std::chrono::nanoseconds(ns);
    while (clk::now() < until) { }
}

static void signaler_routine(Stat* st) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    DEFER(photon::fini());
    g_ready++;
    while (!g_start.load(std::memory_order_acquire))
        std::this_thread::yield();
    auto begin = clk::now();
    for (uint64_t i = 0; i < FLAGS_ops; ++i) {
        spin_ns(FLAGS_work_ns);
        if (i % SAMPLE_EVERY) {
            g_sem->signal(1);
            continue;
        }
        auto t = clk::now();
        g_sem->signal(1);
        auto lat = ns_since(t);
        st->sampled_sum += lat;
        st->samples++;
        if (lat > st->sampled_max) st->sampled_max = lat;
    }
    st->elapsed_ns = ns_since(begin);
}

// spawn the signalers and wait until every one of them has become a vCPU, so that the
// measured region contains nothing but the signal(1) calls themselves
static void spawn_signalers() {
    g_ready.store(0);
    g_start.store(false);
    g_stats.assign(FLAGS_signalers, {});
    for (uint64_t i = 0; i < FLAGS_signalers; ++i)
        g_signalers.emplace_back(&signaler_routine, &g_stats[i]);
    while (g_ready.load() < FLAGS_signalers)
        photon::thread_yield();
}

static void join_signalers() {
    for (auto& th : g_signalers) th.join();
    g_signalers.clear();
}

static void report(const char* name, uint64_t ns) {
    auto n = total_ops();
    if (!ns) ns = 1;
    uint64_t lo = -1, hi = 0, sum = 0, lat_sum = 0, lat_max = 0, samples = 0;
    for (auto& st : g_stats) {
        if (st.elapsed_ns < lo) lo = st.elapsed_ns;
        if (st.elapsed_ns > hi) hi = st.elapsed_ns;
        sum += st.elapsed_ns;
        lat_sum += st.sampled_sum;
        lat_max = (st.sampled_max > lat_max) ? st.sampled_max : lat_max;
        samples += st.samples;
    }
    if (!samples) samples = 1;
    LOG_INFO("`: ` ops by ` vCPU in ` us, ` ns/op, QPS `", name, n, FLAGS_signalers,
             ns / 1000, ns / n, n * 1000000000 / ns);
    LOG_INFO("    per-vCPU elapsed min/avg/max ` / ` / ` us, signal() sampled avg/max ` / ` ns",
             lo / 1000, sum / FLAGS_signalers / 1000, hi / 1000, lat_sum / samples, lat_max);
}

// the signalers never block, so they are always joinable; the process is then killed
// right away, as a semaphore with a permanently sleeping waiter must not be destructed
static void hang_detected(const char* name) {
    LOG_ERROR("` did not finish in ` seconds, likely a lost wakeup, ",
              name, DEADLINE_SEC, VALUE(g_sem->count()));
    join_signalers();
    _exit(1);
}

static void check_drained(photon::semaphore& sem, uint64_t expected) {
    if (sem.count() == expected) return;
    g_failed = true;
    LOG_ERROR("unexpected count left in the semaphore, ` ", VALUE(sem.count()), VALUE(expected));
}

// one waiter blocked in wait(total): every signal finds the head unsatisfiable
static void case_batch() {
    photon::semaphore sem(0), done(0);
    g_sem = &sem;
    volatile bool entered = false;
    auto waiter = [&] {
        entered = true;
        sem.wait(total_ops());
        done.signal(1);
    };
    photon::thread_create11(waiter);
    while (!entered) photon::thread_yield();
    photon::thread_yield();     // let it block inside wait()

    spawn_signalers();
    auto start = clk::now();
    g_start.store(true, std::memory_order_release);
    // the last signal(1) resumes the waiter
    if (done.wait(1, photon::Timeout(DEADLINE_SEC * 1000 * 1000)) < 0)
        hang_detected("batch (1 waiter in wait(total))");
    auto ns = ns_since(start);
    join_signalers();

    report("batch (1 waiter in wait(total))", ns);
    check_drained(sem, 0);
}

// M waiters each doing wait(1) in a loop: every signal may resume one of them
static void case_stream() {
    photon::semaphore sem(0);
    g_sem = &sem;
    auto n = total_ops(), w = FLAGS_waiters;
    volatile uint64_t entered = 0, running = w;
    auto waiter = [&](uint64_t quota) {
        entered = entered + 1;
        for (uint64_t i = 0; i < quota; ++i) sem.wait(1);
        running = running - 1;
    };
    for (uint64_t i = 0; i < w; ++i)
        photon::thread_create11(waiter, n / w + (i < n % w));
    while (entered < w) photon::thread_yield();
    photon::thread_yield();     // let the last one block inside wait()

    spawn_signalers();
    auto start = clk::now();
    g_start.store(true, std::memory_order_release);
    auto deadline = clk::now() + std::chrono::seconds(DEADLINE_SEC);
    while (running) {
        if (clk::now() > deadline)
            hang_detected("stream (M waiters in wait(1))");
        photon::thread_yield();
    }
    auto ns = ns_since(start);
    join_signalers();

    report("stream (M waiters in wait(1))", ns);
    check_drained(sem, 0);
}

// no waiter at all: the baseline cost of an atomic counter plus its cache line traffic
static void case_no_waiter() {
    photon::semaphore sem(0);
    g_sem = &sem;
    spawn_signalers();
    auto start = clk::now();
    g_start.store(true, std::memory_order_release);
    join_signalers();           // no waiter to run, so blocking the vCPU is harmless
    auto ns = ns_since(start);

    report("no waiter", ns);
    check_drained(sem, total_ops());
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_log_output_level(ALOG_INFO);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE) != 0)
        return -1;
    DEFER(photon::fini());
    LOG_INFO("` signalers x ` ops, ` waiters, ` ns work before each signal",
             FLAGS_signalers, FLAGS_ops, FLAGS_waiters, FLAGS_work_ns);
    case_no_waiter();
    case_batch();
    case_stream();
    return g_failed ? -1 : 0;
}
