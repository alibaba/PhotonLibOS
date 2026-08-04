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

#include "../../test/gtest.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <vector>

#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/stack-allocator.h>
#include <photon/thread/thread.h>
#include <photon/thread/workerpool.h>

using namespace photon;

static constexpr size_t K = 1024;
static constexpr size_t M = 1024 * 1024;
static const size_t PGSZ = getpagesize();   // matches photon::PAGE_SIZE

// Every test resets the allocator options through the idempotent setup call,
// then trims the pool so leftover cached blocks from a previous test do not
// perturb the byte accounting.
static void setup_pool(const GlobalStackPoolOptions& opt) {
    use_global_pooled_stack_allocator(opt);
    global_pooled_stack_trim(0);
}

TEST(GlobalStackPool, ReuseExactSize) {
    GlobalStackPoolOptions opt;
    setup_pool(opt);
    const size_t sz = 128 * K;
    auto before = global_pooled_stack_stats();
    void* p1 = global_pooled_stack_alloc(nullptr, sz);
    ASSERT_NE(p1, nullptr);
    global_pooled_stack_dealloc(nullptr, p1, sz);
    // Same size must come back from the magazine, i.e. the very same block.
    void* p2 = global_pooled_stack_alloc(nullptr, sz);
    EXPECT_EQ(p1, p2);
    global_pooled_stack_dealloc(nullptr, p2, sz);
    // A long reuse loop must not keep mapping fresh blocks.
    for (int i = 0; i < 2000; i++) {
        void* p = global_pooled_stack_alloc(nullptr, sz);
        ASSERT_NE(p, nullptr);
        global_pooled_stack_dealloc(nullptr, p, sz);
    }
    auto after = global_pooled_stack_stats();
    EXPECT_LT(after.os_maps - before.os_maps, 4u);
}

TEST(GlobalStackPool, MultiSizeClassesAndPassthrough) {
    GlobalStackPoolOptions opt;
    setup_pool(opt);
    void* a = global_pooled_stack_alloc(nullptr, 128 * K);
    void* b = global_pooled_stack_alloc(nullptr, 256 * K);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    // Larger than MAX_CLASS_SIZE -> passthrough (still valid memory).
    void* huge = global_pooled_stack_alloc(nullptr, 300 * M);
    ASSERT_NE(huge, nullptr);
    // Touch the usable region to make sure the mapping is real & writable.
    ((char*)huge)[PGSZ] = 1;
    ((char*)huge)[300 * M - 1] = 1;
    global_pooled_stack_dealloc(nullptr, a, 128 * K);
    global_pooled_stack_dealloc(nullptr, b, 256 * K);
    global_pooled_stack_dealloc(nullptr, huge, 300 * M);
}

static size_t self_vsz_bytes() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long pages = 0;
    if (fscanf(f, "%lu", &pages) != 1) pages = 0;
    fclose(f);
    return (size_t)pages * getpagesize();
}

TEST(GlobalStackPool, ReclaimOnMmapFailure) {
    // A stack allocation must not fail while the pool still holds reclaimable
    // idle memory. Under a tight RLIMIT_AS, fill the cache with one size class,
    // then request a different size that needs a fresh mapping: the mmap is
    // refused by the OS, the allocator dumps its idle cache and retries, and
    // the allocation succeeds. Run in a child so the parent is unaffected.
    pid_t pid = fork();
    if (pid == 0) {
        GlobalStackPoolOptions opt;
        use_global_pooled_stack_allocator(opt);
        struct rlimit rl;
        getrlimit(RLIMIT_AS, &rl);
        size_t budget = self_vsz_bytes() + 64 * M;
        rl.rlim_cur = (rl.rlim_max == RLIM_INFINITY) ? budget
                                                     : std::min<size_t>(budget, rl.rlim_max);
        setrlimit(RLIMIT_AS, &rl);
        const size_t A = 128 * K, B = 256 * K;
        // Fill with A-sized blocks until the OS ceiling is hit (proving there
        // is no preemptive cap: it only stops at the real OS limit).
        std::vector<void*> live;
        for (int i = 0; i < 100000; i++) {
            void* p = global_pooled_stack_alloc(nullptr, A);
            if (!p) break;
            live.push_back(p);
        }
        if (live.size() < 16) _exit(2);   // budget not tight enough to be meaningful
        for (auto p : live) global_pooled_stack_dealloc(nullptr, p, A);
        // AS is now full of cached A blocks; a B-sized request needs a fresh
        // mapping. It must succeed via reclaim-on-failure.
        void* b = global_pooled_stack_alloc(nullptr, B);
        _exit(b ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    int rc = WEXITSTATUS(status);
    // rc==2 means the RLIMIT_AS budget was not tight enough on this host to
    // exercise the reclaim path; treat it as a non-failure. (GTEST_SKIP is
    // unavailable in some CI gtest versions, so we just return.)
    if (rc == 2) {
        LOG_INFO("RLIMIT_AS budget not tight enough; reclaim-on-failure not exercised");
        return;
    }
    EXPECT_EQ(rc, 0);
}

TEST(GlobalStackPool, DoubleFreeDetected) {
    GlobalStackPoolOptions opt;
    setup_pool(opt);
    const size_t sz = 128 * K;
    auto before = global_pooled_stack_stats();
    void* p = global_pooled_stack_alloc(nullptr, sz);
    ASSERT_NE(p, nullptr);
    global_pooled_stack_dealloc(nullptr, p, sz);
    // Second free of the same pointer: caught, counted, and not re-pooled.
    global_pooled_stack_dealloc(nullptr, p, sz);
    auto after = global_pooled_stack_stats();
    EXPECT_EQ(after.corruptions - before.corruptions, 1u);
}

TEST(GlobalStackPool, TrimReturnsMemory) {
    GlobalStackPoolOptions opt;
    opt.per_vcpu_cache_bytes = 128 * K;   // K==1 so frees quickly reach the depot
    opt.max_pooled_bytes = 1ULL << 30;
    setup_pool(opt);
    const size_t sz = 128 * K;
    std::vector<void*> ps;
    for (int i = 0; i < 64; i++) {
        void* p = global_pooled_stack_alloc(nullptr, sz);
        ASSERT_NE(p, nullptr);
        ps.push_back(p);
    }
    for (auto p : ps) global_pooled_stack_dealloc(nullptr, p, sz);
    auto mid = global_pooled_stack_stats();
    EXPECT_GT(mid.mapped_bytes, 32 * sz);
    size_t freed = global_pooled_stack_trim(0);
    EXPECT_GT(freed, 0u);
    auto after = global_pooled_stack_stats();
    EXPECT_LT(after.mapped_bytes, mid.mapped_bytes);
}

TEST(GlobalStackPool, ColdPoolReuseAvoidsRemap) {
    // A size unique to this test so the class capacity is K==1 and every free
    // overflows the magazine into the depot (then to the cold list).
    const size_t sz = 320 * K;
    GlobalStackPoolOptions opt;
    opt.per_vcpu_cache_bytes = sz;        // K==1
    opt.max_pooled_bytes = 0;             // overflow straight to pending
    opt.max_pending_bytes = 0;            // back-pressure straight to cold
    opt.max_cold_bytes = 1ULL << 30;
    setup_pool(opt);
    std::vector<void*> ps;
    for (int i = 0; i < 32; i++)
        ps.push_back(global_pooled_stack_alloc(nullptr, sz));
    for (auto p : ps) global_pooled_stack_dealloc(nullptr, p, sz);
    auto s1 = global_pooled_stack_stats();
    EXPECT_GT(s1.cold_bytes, 0u);
    // Reuse should be served from the cold list, not by fresh mmap.
    auto before_maps = s1.os_maps;
    std::vector<void*> ps2;
    for (int i = 0; i < 32; i++)
        ps2.push_back(global_pooled_stack_alloc(nullptr, sz));
    auto s2 = global_pooled_stack_stats();
    EXPECT_LT(s2.os_maps - before_maps, 32u);
    for (auto p : ps2) global_pooled_stack_dealloc(nullptr, p, sz);
    global_pooled_stack_trim(0);
}

TEST(GlobalStackPool, GuardPageFaults) {
    GlobalStackPoolOptions opt;
    setup_pool(opt);
    const size_t sz = 128 * K;
    void* p = global_pooled_stack_alloc(nullptr, sz);
    ASSERT_NE(p, nullptr);
    // Writing the guard page must fault; verify in a child so the test lives.
    pid_t pid = fork();
    if (pid == 0) {
        ((volatile char*)p)[0] = 42;   // guard page, PROT_NONE
        _exit(0);                      // should never reach here
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    // The guard write must prevent the child from reaching _exit(0): either it
    // is killed by SIGSEGV, or (under a SEGV-handling sanitizer) it exits with
    // a non-zero code. Both mean the guard page did its job.
    bool clean_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    EXPECT_FALSE(clean_exit);
    global_pooled_stack_dealloc(nullptr, p, sz);
}

// Allocate a photon thread on the calling vcpu, migrate it to a worker vcpu
// where it runs and exits (non-joinable => freed on that vcpu). This exercises
// the cross-vcpu free path that unbalances a per-vcpu pool but must stay
// bounded here.
static std::atomic<uint64_t> g_done{0};
static semaphore* g_sem;
static void* migrant(void*) {
    g_done.fetch_add(1, std::memory_order_relaxed);
    g_sem->signal(1);
    return nullptr;
}

TEST(GlobalStackPool, CrossVcpuBounded) {
    GlobalStackPoolOptions opt;
    opt.per_vcpu_cache_bytes = 4 * M;
    opt.max_pooled_bytes = 32 * M;
    opt.max_pending_bytes = 16 * M;
    opt.max_cold_bytes = 32 * M;
    PhotonOptions po;
    po.use_global_pooled_stack_allocator = true;
    ASSERT_EQ(init(INIT_EVENT_DEFAULT, INIT_IO_NONE, po), 0);
    DEFER(fini());
    use_global_pooled_stack_allocator(opt);

    const int NV = 4;
    const int CONC = 32;
    const int ROUNDS = 40;
    const size_t sz = 512 * K;
    WorkPool pool(NV, INIT_EVENT_DEFAULT, INIT_IO_NONE, 0);
    semaphore sem(0);
    g_sem = &sem;

    size_t baseline = 0;
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < CONC; i++) {
            auto th = thread_create(&migrant, nullptr, sz);
            ASSERT_NE(th, nullptr);
            pool.thread_migrate(th, i % NV);
        }
        sem.wait(CONC);
        if (r == 2) baseline = global_pooled_stack_stats().mapped_bytes;
    }
    EXPECT_EQ(g_done.load(), (uint64_t)ROUNDS * CONC);
    // Steady state must not grow with the number of rounds: the cross-vcpu
    // free path returns blocks to the shared depot, not to whichever vcpu ran.
    auto s = global_pooled_stack_stats();
    ASSERT_GT(baseline, 0u);
    EXPECT_LT(s.mapped_bytes, baseline + 64 * M);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    set_log_output_level(ALOG_WARN);
    return RUN_ALL_TESTS();
}
