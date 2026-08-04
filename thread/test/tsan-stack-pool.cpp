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

// ThreadSanitizer stress harness for the global stack pool. It hammers the
// allocator from many real OS threads (bypassing photon's fiber machinery, so
// TSan sees only the allocator's own synchronization) while other threads call
// stats() and trim() concurrently. This exercises the exact races the audit
// flagged: concurrent class registration vs reclaim/stats/trim (n_classes),
// per-vcpu hit/miss counters read by stats(), and the depot locks.
//
// Build with -fsanitize=thread and run manually; success == TSan reports no
// data races and the process exits 0.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <unistd.h>

#include <photon/thread/stack-allocator.h>

using namespace photon;

static std::atomic<bool> g_stop{false};

// Distinct sizes so up to 8 classes register concurrently at startup (the
// n_classes publication window), plus one oversized passthrough size.
static const size_t kSizes[] = {
    64 * 1024, 128 * 1024, 192 * 1024, 256 * 1024,
    320 * 1024, 384 * 1024, 448 * 1024, 512 * 1024,
};

static void worker(int id) {
    const size_t sz = kSizes[id % 8];
    uint64_t n = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        void* p = global_pooled_stack_alloc(nullptr, sz);
        if (p) {
            ((volatile char*)p)[getpagesize()] = (char)id;   // touch usable page
            global_pooled_stack_dealloc(nullptr, p, sz);
        }
        // Occasionally exercise the passthrough path (unpooled, >MAX_CLASS_SIZE).
        if ((++n & 0x3fff) == 0) {
            void* big = global_pooled_stack_alloc(nullptr, 300ull * 1024 * 1024);
            if (big) global_pooled_stack_dealloc(nullptr, big, 300ull * 1024 * 1024);
        }
    }
}

static void observer() {
    while (!g_stop.load(std::memory_order_relaxed)) {
        auto s = global_pooled_stack_stats();
        (void)s;
    }
}

static void trimmer() {
    while (!g_stop.load(std::memory_order_relaxed)) {
        global_pooled_stack_trim(4ull * 1024 * 1024);
    }
}

int main() {
    GlobalStackPoolOptions opt;
    opt.max_pooled_bytes = 32ull * 1024 * 1024;
    opt.max_pending_bytes = 16ull * 1024 * 1024;
    opt.max_cold_bytes = 64ull * 1024 * 1024;
    opt.per_vcpu_cache_bytes = 2ull * 1024 * 1024;
    use_global_pooled_stack_allocator(opt);

    std::vector<std::thread> ts;
    for (int i = 0; i < 12; i++) ts.emplace_back(worker, i);
    ts.emplace_back(observer);
    ts.emplace_back(observer);
    ts.emplace_back(trimmer);

    std::this_thread::sleep_for(std::chrono::seconds(3));
    g_stop.store(true, std::memory_order_relaxed);
    for (auto& t : ts) t.join();
    return 0;
}
