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

// migrate_to_pool / check_pool_engines: the two primitives every transport uses
// to hand its serving coroutines to a caller-supplied photon::WorkPool. Tested
// here rather than through a transport because the failure modes are properties
// of the primitives -- an empty pool is a division by zero inside WorkPool, and
// a pool whose vcpus have no event engine turns every fd wait into a hot spin.

#include "../utils.h"
#include "../../test/gtest.h"

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/thread/thread.h>
#include <photon/thread/workerpool.h>

#include <set>
#include <vector>

using namespace photon;
using namespace photon::blk;

namespace {

struct Landing {
    photon::semaphore done;
    // The probe coroutines run on different OS threads concurrently, so there
    // must be no shared writes: each writes only its own slots[i].
    std::vector<vcpu_base*> slots;
};

struct ProbeArg {
    Landing* l;
    int index;
};

void* record_vcpu(void* a) {
    auto* p = (ProbeArg*)a;
    p->l->slots[p->index] = photon::get_vcpu();
    p->l->done.signal(1);
    return nullptr;
}

// Spawn `n` coroutines, migrate each into `pool`, and collect the vcpus they ran
// on. The caller's own vcpu is included in the result when migration is a no-op,
// which is how the degenerate cases are distinguished from real fan-out.
std::set<vcpu_base*> land(int n, photon::WorkPool* pool) {
    Landing l;
    l.slots.resize(n, nullptr);
    std::vector<ProbeArg> args(n);   // lives in this frame; the joins below keep it alive until every coroutine has finished
    for (int i = 0; i < n; i++)
        args[i] = {&l, i};
    auto self = photon::get_vcpu();
    std::vector<photon::thread*> ths;
    for (int i = 0; i < n; i++) {
        auto th = photon::thread_create(&record_vcpu, &args[i]);
        EXPECT_NE(nullptr, th);
        photon::thread_enable_join(th);   // photon threads are detached by default: without this, a finished record_vcpu disposes itself and the join below is a use-after-free
        migrate_to_pool(pool, th);
        ths.push_back(th);
    }
    l.done.wait(n);
    for (auto th : ths)
        photon::thread_join((photon::join_handle*)th);
    std::set<vcpu_base*> vcpus(l.slots.begin(), l.slots.end());
    if (vcpus.empty())
        vcpus.insert(self);
    return vcpus;
}

}   // namespace

TEST(blk_pool, null_pool_leaves_coroutine_on_caller_vcpu) {
    auto self = photon::get_vcpu();
    auto v = land(3, nullptr);
    ASSERT_EQ(1UL, v.size());
    EXPECT_EQ(self, *v.begin());
}

TEST(blk_pool, null_thread_is_not_an_error) {
    migrate_to_pool(nullptr, nullptr);
    photon::WorkPool pool(2, INIT_EVENT_EPOLL, INIT_IO_NONE);
    migrate_to_pool(&pool, nullptr);   // must not crash, must not migrate anything
}

// The guard this exists for: WorkPool::get_vcpu_in_pool does `vcpu_index++ % size`
// for an out-of-range index, and migrate_to_pool always passes an out-of-range
// index on purpose (that is what makes WorkPool hand out its own round-robin
// cursor). size == 0 is therefore a SIGFPE, not a clean failure.
TEST(blk_pool, empty_pool_does_not_divide_by_zero) {
    photon::WorkPool pool(0);
    EXPECT_EQ(0, pool.get_vcpu_num());
    auto self = photon::get_vcpu();
    auto v = land(3, &pool);
    ASSERT_EQ(1UL, v.size());
    EXPECT_EQ(self, *v.begin());   // fell back to the caller's vcpu
}

TEST(blk_pool, fewer_queues_than_vcpus_land_on_distinct_vcpus) {
    photon::WorkPool pool(4, INIT_EVENT_EPOLL, INIT_IO_NONE);
    ASSERT_EQ(4, pool.get_vcpu_num());
    auto v = land(3, &pool);
    EXPECT_EQ(3UL, v.size());   // n <= m: one vcpu each, no sharing
}

TEST(blk_pool, more_queues_than_vcpus_use_every_vcpu) {
    photon::WorkPool pool(2, INIT_EVENT_EPOLL, INIT_IO_NONE);
    ASSERT_EQ(2, pool.get_vcpu_num());
    auto v = land(6, &pool);
    EXPECT_EQ(2UL, v.size());   // n > m: shared, but every vcpu is used
}

// Two devices on one pool must not both pile onto vcpu 0. The cursor is
// WorkPool's own, shared across callers, so the second batch continues where the
// first left off rather than restarting.
TEST(blk_pool, cursor_spreads_two_devices) {
    photon::WorkPool pool(3, INIT_EVENT_EPOLL, INIT_IO_NONE);
    auto a = land(2, &pool);
    auto b = land(2, &pool);
    EXPECT_EQ(2UL, a.size());
    EXPECT_EQ(2UL, b.size());
    std::set<vcpu_base*> all(a);
    all.insert(b.begin(), b.end());
    EXPECT_EQ(3UL, all.size());   // four queues over three vcpus touch all three
}

TEST(blk_pool, engines_null_pool_is_accepted) {
    EXPECT_EQ(0, check_pool_engines(nullptr));
    photon::WorkPool empty(0);
    EXPECT_EQ(0, check_pool_engines(&empty));
}

// WorkPool's constructor defaults are ev_engine = 0, io_engine = 0, and
// INIT_EVENT_NONE is 0 -- so the natural `WorkPool pool(4);` produces vcpus whose
// master engine is the NullEventEngine, whose wait_for_fd returns -1 without
// setting errno. A serving coroutine parked there hot-spins and logs every pass
// instead of sleeping. That is a configuration error, so start() must refuse it
// rather than serve pathologically.
TEST(blk_pool, engines_reject_a_pool_with_no_event_engine) {
    photon::WorkPool pool(2);   // defaults: no event engine, no io engine
    ASSERT_EQ(2, pool.get_vcpu_num());
    errno = 0;
    EXPECT_EQ(-1, check_pool_engines(&pool));
    EXPECT_EQ(EINVAL, errno);
}

TEST(blk_pool, engines_accept_a_matching_pool) {
    // the caller's own vcpu is what the requirement is derived from
    photon::WorkPool pool(2, photon::get_event_engine(), photon::get_io_engine());
    ASSERT_EQ(2, pool.get_vcpu_num());
    EXPECT_EQ(0, check_pool_engines(&pool));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
#ifdef __linux__
    int ev = INIT_EVENT_EPOLL;
#else
    int ev = INIT_EVENT_KQUEUE;
#endif
    if (photon::init(ev, INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    set_log_output_level(1);
    return RUN_ALL_TESTS();
}
