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

// Three-way stack allocator benchmark:
//   malloc  -> default allocator (posix_memalign + mprotect, glibc-backed)
//   pooled  -> vcpu-local pooled_stack_allocator
//   global  -> process-wide global_pooled_stack_allocator
//
// Run one allocator with --allocator=malloc|pooled|global, or --allocator=all
// (default) to fork a child per allocator and compare in a single invocation.
// Patterns: create_join, burst, migrate_heavy, steady_churn (or --pattern=all).

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/stack-allocator.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>

DEFINE_string(allocator, "all", "malloc | pooled | global | all");
DEFINE_string(pattern, "all", "create_join | burst | migrate_heavy | steady_churn | all");
DEFINE_uint64(vcpu_num, 4, "worker vCPU num");
DEFINE_uint64(fires, 200000, "operations per pattern");
DEFINE_uint64(stack_size, 8ull << 20, "stack size in bytes");
DEFINE_uint64(concurrency, 256, "in-flight photon threads for burst/migrate");

using namespace photon;
using clk = std::chrono::steady_clock;

static std::atomic<uint64_t> g_counter{0};
static semaphore* g_sem;

static uint64_t ns_since(clk::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t).count();
}
static size_t peak_rss_kb() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (size_t)ru.ru_maxrss;   // KB on Linux
}
static size_t cur_vsz_kb() {
    // /proc/self/statm: size resident ... (in pages)
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long sz = 0;
    if (fscanf(f, "%lu", &sz) != 1) sz = 0;
    fclose(f);
    return sz * (sysconf(_SC_PAGESIZE) / 1024);
}

static void* worker(void*) {
    g_counter.fetch_add(1, std::memory_order_relaxed);
    g_sem->signal(1);
    return nullptr;
}

static void report(const char* alloc, const char* pat, uint64_t total_ns) {
    uint64_t qps = FLAGS_fires * 1000000000ull / (total_ns ? total_ns : 1);
    LOG_INFO("[`] `: QPS=`, avg=` ns/op, peakRSS=` MB, VSZ=` MB",
             alloc, pat, qps, total_ns / FLAGS_fires,
             peak_rss_kb() / 1024, cur_vsz_kb() / 1024);
}

// create + join on the current vcpu: pure alloc/dealloc, same vcpu.
static void pat_create_join(const char* alloc) {
    auto start = clk::now();
    for (uint64_t i = 0; i < FLAGS_fires; i++) {
        auto th = thread_create11(FLAGS_stack_size, [] {});
        thread_enable_join(th);
        thread_join((join_handle*)th);
    }
    report(alloc, "create_join", ns_since(start));
}

// burst: keep `concurrency` non-joinable threads in flight, then drain.
static void pat_burst(const char* alloc) {
    semaphore sem(0);
    g_sem = &sem;
    auto start = clk::now();
    uint64_t done = 0;
    while (done < FLAGS_fires) {
        uint64_t batch = std::min<uint64_t>(FLAGS_concurrency, FLAGS_fires - done);
        for (uint64_t i = 0; i < batch; i++)
            thread_create(&worker, nullptr, FLAGS_stack_size);
        sem.wait(batch);
        done += batch;
    }
    report(alloc, "burst", ns_since(start));
}

// migrate_heavy: allocate on this vcpu, run/free on a worker vcpu (cross-vcpu).
static void pat_migrate_heavy(const char* alloc, WorkPool& pool) {
    semaphore sem(0);
    g_sem = &sem;
    auto start = clk::now();
    uint64_t done = 0;
    while (done < FLAGS_fires) {
        uint64_t batch = std::min<uint64_t>(FLAGS_concurrency, FLAGS_fires - done);
        for (uint64_t i = 0; i < batch; i++) {
            auto th = thread_create(&worker, nullptr, FLAGS_stack_size);
            pool.thread_migrate(th, i % FLAGS_vcpu_num);
        }
        sem.wait(batch);
        done += batch;
    }
    report(alloc, "migrate_heavy", ns_since(start));
}

// steady_churn: fan tasks to the pool, each creating a thread on a worker vcpu.
static void pat_steady_churn(const char* alloc, WorkPool& pool) {
    semaphore sem(0);
    g_sem = &sem;
    auto start = clk::now();
    for (uint64_t i = 0; i < FLAGS_fires; i++) {
        pool.async_call(new auto([] { g_sem->signal(1); }));
        if ((i & 0x3ff) == 0x3ff) thread_yield();
    }
    sem.wait(FLAGS_fires);
    report(alloc, "steady_churn", ns_since(start));
}

static bool want(const char* pat) {
    return FLAGS_pattern == "all" || FLAGS_pattern == pat;
}

static int run_one(const std::string& alloc) {
    if (alloc == "pooled") {
        use_pooled_stack_allocator();
        pooled_stack_trim_threshold(-1ull);
    } else if (alloc == "global") {
        GlobalStackPoolOptions opt;
        opt.max_pooled_bytes = 4ull << 30;
        opt.max_pending_bytes = 1ull << 30;
        opt.max_cold_bytes = 4ull << 30;
        use_global_pooled_stack_allocator(opt);
    }
    if (init(INIT_EVENT_DEFAULT, INIT_IO_NONE) != 0) return -1;
    DEFER(fini());
    WorkPool pool(FLAGS_vcpu_num, INIT_EVENT_DEFAULT, INIT_IO_NONE, 0);

    g_counter = 0;
    if (want("create_join"))   pat_create_join(alloc.c_str());
    if (want("burst"))         pat_burst(alloc.c_str());
    if (want("migrate_heavy")) pat_migrate_heavy(alloc.c_str(), pool);
    if (want("steady_churn"))  pat_steady_churn(alloc.c_str(), pool);

    if (alloc == "global") {
        auto s = global_pooled_stack_stats();
        LOG_INFO("[global] stats: mapped=` MB, pooled=` MB, cold=` MB, "
                 "hits=`, misses=`, os_maps=`, os_unmaps=`",
                 s.mapped_bytes >> 20, s.pooled_bytes >> 20, s.cold_bytes >> 20,
                 s.hits, s.misses, s.os_maps, s.os_unmaps);
    }
    return 0;
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_log_output_level(ALOG_INFO);

    std::vector<std::string> allocs;
    if (FLAGS_allocator == "all") allocs = {"malloc", "pooled", "global"};
    else allocs = {FLAGS_allocator};

    if (allocs.size() == 1) return run_one(allocs[0]);

    // Fork a child per allocator to isolate global allocator/pool state.
    for (auto& a : allocs) {
        pid_t pid = fork();
        if (pid == 0) {
            _exit(run_one(a) == 0 ? 0 : 1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
    }
    return 0;
}
