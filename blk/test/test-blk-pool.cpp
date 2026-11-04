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
// of the primitives -- an empty pool is a division by zero inside WorkPool, and a
// vcpu with no master event engine cannot host an fd wait at all. The second
// needs no exotic setup: ev_engine defaults to 0 and INIT_EVENT_NONE is 0, so the
// natural `WorkPool pool(4);` installs the NullEventEngine, whose wait_for_fd
// answers -1 at once, drops the timeout and leaves errno alone. The consequence
// for a serving coroutine differs per transport -- tcmu among them still serves,
// at the price of a poll -- and is documented at check_pool_engines.
//
// A third failure mode is a lifetime rather than a configuration one: the probe
// check_pool_engines migrates onto a pool vcpu is handed the address of a frame in
// the caller's own stack, so a caller that gives up waiting has to retire the probe
// before it unwinds. The case at the bottom of this file drives the wakeup that is
// not the probe's signal -- an interruption delivered through the public
// thread_interrupt(th, error_number) API -- against a vcpu held busy so the probe is
// genuinely still pending when it lands.

#include "../utils.h"
#include "../../test/gtest.h"
#include "harness.h"        // only for main()'s consumer-child dispatch

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>   // DEFER
#include <photon/thread/thread.h>
#include <photon/thread/workerpool.h>

#include <atomic>      // the gate's release flag, read on a pool vcpu
#include <cerrno>      // ESHUTDOWN, the errno the interrupted probe must report
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

// A null thread is refused by migrate_to_pool rather than passed down, and the
// refusal is not redundant with photon's own null check: WorkPool::thread_migrate
// resolves the index BEFORE it calls photon::thread_migrate, and it resolves the
// out-of-range index migrate_to_pool always passes with `vcpu_index++ % size`. A null
// call that got that far would therefore be rejected only after consuming a slot of
// the pool-wide round-robin cursor, shifting every placement that follows it --
// including another device's. Over a 2-vcpu pool that shift is visible as a lost
// alternation: one cursor step per landing alternates, two steps land back where they
// started, which is what the last EXPECT_NE reads. Both landings are first shown to be
// pool vcpus rather than the caller's, so a pair that differs cannot be one that
// differs because a migration silently failed and left a coroutine at home.
TEST(blk_pool, null_thread_is_not_an_error) {
    auto self = photon::get_vcpu();
    migrate_to_pool(nullptr, nullptr);
    photon::WorkPool pool(2, test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE);
    ASSERT_EQ(2, pool.get_vcpu_num());
    auto a = land(1, &pool);
    migrate_to_pool(&pool, nullptr);   // must not crash, must not migrate anything
    auto b = land(1, &pool);
    ASSERT_EQ(1UL, a.size());
    ASSERT_EQ(1UL, b.size());
    EXPECT_NE(self, *a.begin());
    EXPECT_NE(self, *b.begin());
    EXPECT_NE(*a.begin(), *b.begin());
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
    photon::WorkPool pool(4, test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE);
    ASSERT_EQ(4, pool.get_vcpu_num());
    auto v = land(3, &pool);
    EXPECT_EQ(3UL, v.size());   // n <= m: one vcpu each, no sharing
}

TEST(blk_pool, more_queues_than_vcpus_use_every_vcpu) {
    photon::WorkPool pool(2, test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE);
    ASSERT_EQ(2, pool.get_vcpu_num());
    auto v = land(6, &pool);
    EXPECT_EQ(2UL, v.size());   // n > m: shared, but every vcpu is used
}

// Two devices on one pool must not both pile onto vcpu 0. The cursor is
// WorkPool's own, shared across callers, so the second batch continues where the
// first left off rather than restarting.
TEST(blk_pool, cursor_spreads_two_devices) {
    photon::WorkPool pool(3, test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE);
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
// master engine is the NullEventEngine, whose wait_for_fd returns -1 immediately,
// discards the timeout, and never writes errno. A vcpu with no master engine
// cannot host an fd wait at all; what a serving coroutine does then differs per
// transport, and is documented at check_pool_engines. That is a configuration
// error, so start() must refuse it rather than serve pathologically.
TEST(blk_pool, engines_reject_a_pool_with_no_event_engine) {
    photon::WorkPool pool(2);   // defaults: no event engine, no io engine
    ASSERT_EQ(2, pool.get_vcpu_num());
    errno = 0;
    EXPECT_EQ(-1, check_pool_engines(&pool));
    EXPECT_EQ(EINVAL, errno);
}

TEST(blk_pool, engines_accept_a_matching_pool) {
    // The same request the caller's own vcpu got, which is what the requirement
    // is derived from -- see TEST_EVENT_ENGINE for why it is a shared constant.
    photon::WorkPool pool(2, test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE);
    ASSERT_EQ(2, pool.get_vcpu_num());
    EXPECT_EQ(0, check_pool_engines(&pool));
}

// ---------------------------------------------------------------------------
// An interrupted probe.
//
// check_pool_engines hands a stack-allocated PoolProbe to a coroutine it migrates
// onto the pool vcpu, and the join it performs before returning is the ONLY thing
// that keeps the probe from writing into a dead frame. This case drives the one
// wakeup that is not the probe's signal: semaphore::wait also answers -1 for
// ETIMEDOUT and for ESHUTDOWN, and with no timeout passed at the call site the
// source of either is a caller's own thread_interrupt(th, error_number) -- a
// supported public API. thread_shutdown is NOT the trigger: it interrupts with
// EPERM, which wait()'s retry loop swallows.
//
// errno is the discriminator. Unfixed, the wait's return was ignored, so an
// interrupted caller read the still-empty p.ev, reported it as an engine MISMATCH
// with EINVAL, and destroyed the frame while the delayed probe still held its
// address -- EINVAL here, the interrupter's own ESHUTDOWN after the fix. An ASAN
// build turns the same run into a hard use-after-free: the probe's write to p.ev
// lands in a poisoned frame, and the signal that follows it lands on a destroyed
// semaphore.
//
// The gate coroutine is what makes the delay real instead of a coin flip. It holds
// the target vcpu in a spin with no yield, so a probe migrated there is not
// scheduled until the gate is released -- which is not until after the interrupt has
// landed. Nothing steals it back either: neither photon::init() nor WorkPool passes
// VCPU_ENABLE_ACTIVE_WORK_STEALING or VCPU_ENABLE_PASSIVE_WORK_STEALING to
// vcpu_init, so try_work_stealing answers false on every vcpu in this test.
//
// The migrate-failure half of the same fix is NOT covered, and cannot be honestly.
// photon::thread_migrate refuses only a null argument, a thread that is not on the
// current vcpu, or one that is not READY, and check_pool_engines arrives with a
// thread the create just left READY on this vcpu with nothing in between that
// yields. There is no public way to fail that call without changing the function
// under test, so the join on that path is argued in its own comment rather than
// pinned here.
// ---------------------------------------------------------------------------

namespace {

// Holds its vcpu. Spins on `release` without yielding, which is the whole point:
// the coroutine migrated here behind it does not run until the spin ends. `started`
// is signalled from the vcpu itself, so a caller that has seen it knows the gate is
// in place and ahead of anything migrated later -- photon's runq is FIFO and the
// gate never yields, so the newcomer cannot pass it.
struct Gate {
    std::atomic<bool> release{false};
    photon::semaphore started{0};   // NSDMI, not {}: semaphore's ctor is explicit
};

void* gate_thunk(void* a) {
    auto* g = (Gate*)a;
    g->started.signal(1);
    while (!g->release.load(std::memory_order_relaxed))
        ;   // no yield: this vcpu being unavailable IS the delay
    return nullptr;
}

struct InterruptedCaller {
    photon::WorkPool* pool = nullptr;
    int rc = 0;
    int err = 0;
    photon::semaphore done{0};   // NSDMI, not {}: semaphore's ctor is explicit
};

void* caller_thunk(void* a) {
    auto* c = (InterruptedCaller*)a;
    errno = 0;
    c->rc = check_pool_engines(c->pool);
    c->err = errno;   // errno is this OS thread's, shared by every coroutine on it
    c->done.signal(1);
    return nullptr;
}

}   // namespace

TEST(blk_pool, engines_report_an_interrupted_probe_rather_than_a_mismatch) {
    photon::WorkPool pool(1, test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE);
    ASSERT_EQ(1, pool.get_vcpu_num());
    Gate gate;
    InterruptedCaller c;
    c.pool = &pool;
    photon::thread *gth = nullptr, *cth = nullptr;
    // Every exit, a gtest ASSERT's included, has to let the gate off its vcpu and
    // retire both coroutines BEFORE `pool` dies: WorkPool's teardown waits its vcpus
    // out, and a spin nothing can end turns an assertion failure into a hang.
    // Declared after `pool` so it runs first, and it releases before it joins so a
    // gate that never got migrated still exits at once. Null means the create itself
    // failed and there is nothing to retire.
    DEFER({
        gate.release.store(true);
        if (cth) photon::thread_join((photon::join_handle*)cth);
        if (gth) photon::thread_join((photon::join_handle*)gth);
    });

    gth = photon::thread_create(&gate_thunk, &gate);
    ASSERT_NE(nullptr, gth);
    photon::thread_enable_join(gth);
    ASSERT_EQ(0, pool.thread_migrate(gth, (size_t)0));
    gate.started.wait(1);   // vcpu 0 is spinning from here on

    cth = photon::thread_create(&caller_thunk, &c);
    ASSERT_NE(nullptr, cth);
    photon::thread_enable_join(cth);

    // Wait for the caller to park rather than guess how long that takes. SLEEPING is
    // reachable here only from the probe's semaphore wait: nothing between
    // check_pool_engines' entry and it yields -- get_engine_name is a field read,
    // thread_create only queues, and WorkPool::thread_migrate forwards to photon's,
    // which does not switch for a thread that is not the caller. Bounded so a change
    // that removes the wait fails this case instead of hanging the suite.
    int spins = 0;
    while (photon::thread_stat(cth) != photon::states::SLEEPING && ++spins < 1000000)
        photon::thread_yield();
    ASSERT_EQ(photon::states::SLEEPING, photon::thread_stat(cth))
        << "the caller never parked in the probe wait, so the probe was not delayed "
           "and nothing below measures the interrupted path";

    photon::thread_interrupt(cth, ESHUTDOWN);
    // Released AFTER the interrupt, and it has to be released: with the fix the
    // caller joins the probe, so a gate that stayed closed would hang the case
    // rather than pass it.
    gate.release.store(true);

    c.done.wait(1);
    // A short yield before the DEFER retires the caller. On the fixed code the
    // caller has already joined its probe, so this costs one sleep and proves
    // nothing. On the unfixed code it is what usually keeps the demonstration a
    // failed assertion: the probe still writes p.ev and signals p.done after the
    // frame is gone, but the joinable caller's stack is only unwound, not yet
    // returned to photon's allocator, so the write lands in mapped memory instead of
    // racing the disposal. It does not make that code correct, and ASAN reports the
    // use-after-free on the poisoned frame either way.
    photon::thread_usleep(20 * 1000);

    EXPECT_EQ(-1, c.rc);
    EXPECT_EQ(ESHUTDOWN, c.err)
        << "EINVAL here is the unfixed path: the interrupted wait's return was "
           "ignored, so the still-empty probe view was reported as an event-engine "
           "mismatch and the frame was destroyed under a probe that had not run";
}

int main(int argc, char** argv) {
    // A consumer child is this binary re-executed with a sentinel in argv[1]:
    // dispatch it before photon::init() and before gtest sees that argument.
    int cons = photon::blk::test::consumer_child_main(argc, argv);
    if (cons != photon::blk::test::CONS_NOT_A_CHILD)
        return cons;
    ::testing::InitGoogleTest(&argc, argv);
    if (photon::init(test::TEST_EVENT_ENGINE, test::TEST_IO_ENGINE))
        return -1;
    DEFER(photon::fini());
    set_log_output_level(1);
    return RUN_ALL_TESTS();
}
