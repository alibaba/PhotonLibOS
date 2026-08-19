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

// Regression tests for issue #1291: use-after-free of the caller's stack
// OutOfOrderContext when it times out while the receiver is collecting it
// (i.e. the receiver yielded inside do_collect() after taking the pointer
// out of the map).

#include <queue>
#include <vector>
#include "../../test/gtest.h"
#include "../out-of-order-execution.h"
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/thread11.h>

using namespace photon;
using namespace photon::rpc;

// A fake completion source that simulates out-of-order completion without a
// real network: completed tags are pushed into a shared FIFO in an arbitrary
// order (by do_issue or by the test body), and do_completion pops one, which
// is not necessarily the tag of the calling context.
struct FakeSource {
    std::queue<uint64_t> done;
    int issue(OutOfOrderContext* args) {
        done.push(args->tag);
        return 0;
    }
    int complete(OutOfOrderContext* args) {
        while (done.empty())
            thread_yield();
        args->tag = done.front();
        done.pop();
        return 0;
    }
};

// A do_collect that yields for a while, opening the window in which the
// receiver holds a bare context pointer across a coroutine switch.
struct SlowCollect {
    uint64_t sleep_us;
    int collected = 0;
    int collect(OutOfOrderContext*) {
        thread_usleep(sleep_us);
        collected++;
        return 0;
    }
};

// A do_issue that pushes its tag first (so the receiver can see the
// response) and reports failure only after a yield, by which time the
// receiver may have taken the context out of the map into do_collect().
struct FailAfterCollectIssue {
    FakeSource* src;
    uint64_t sleep_us;
    int issue(OutOfOrderContext* args) {
        src->done.push(args->tag);
        thread_usleep(sleep_us);
        return -1;
    }
};

static int null_op(void*, OutOfOrderContext*) {
    return 0;
}

// The caller times out while the receiver is yielding inside do_collect()
// with the caller's context pointer in hand. The caller must wait for the
// hand-back instead of destroying its stack frame, and then return the
// collected result as a success.
TEST(OutOfOrderTimeout, timeout_during_collect_yield) {
    auto engine = new_ooo_execution_engine();
    DEFER(delete_ooo_execution_engine(engine));
    FakeSource src;

    // caller A has no timeout; it becomes the receiver and blocks in
    // complete() until a tag is pushed
    OutOfOrderContext a;
    a.engine = engine;
    a.do_issue.bind(nullptr, &null_op);
    a.do_completion.bind(&src, &FakeSource::complete);
    a.do_collect.bind(nullptr, &null_op);
    int a_ret = -100;
    auto tha = thread_enable_join(thread_create11([&] {
        ASSERT_EQ(0, ooo_issue_operation(a));
        a_ret = ooo_wait_completion(a);
    }));
    thread_yield();  // let A issue and become the receiver
    // do_completion overwrites a.tag with whatever it pops, so save the
    // original tag now for completing A's own operation later
    auto a_tag = a.tag;

    // caller B times out (10ms) while its result is being collected (100ms)
    SlowCollect sc{100 * 1000};
    OutOfOrderContext b;
    b.engine = engine;
    b.do_issue.bind(nullptr, &null_op);
    b.do_completion.bind(&src, &FakeSource::complete);
    b.do_collect.bind(&sc, &SlowCollect::collect);
    b.timeout = Timeout(10 * 1000);
    int b_ret = -100;
    auto thb = thread_enable_join(thread_create11([&] {
        ASSERT_EQ(0, ooo_issue_operation(b));
        b_ret = ooo_wait_completion(b);
    }));
    thread_yield();  // let B issue and enter the waiting queue

    // complete B's operation: the receiver A takes B's context out of the
    // map and yields in do_collect(); B's timeout fires inside this window
    src.done.push(b.tag);
    thread_join(thb);

    // B waited for the hand-back and returned the collected result
    EXPECT_EQ(0, b_ret);
    EXPECT_EQ(1, sc.collected);
    EXPECT_EQ((int)OooPhase::COLLECTED, (int)b.phase);
    // B cleared `th` so that the receiver skips thread_interrupt()
    EXPECT_EQ(nullptr, b.th);

    // the receiver must have taken the `!th -> continue` branch and kept
    // looping for its own result, instead of aborting with -2/ENOENT
    src.done.push(a_tag);
    thread_join(tha);
    EXPECT_EQ(0, a_ret);
    EXPECT_EQ(0, ooo_get_queue_count(engine));
}

// The caller yields between issue and wait_completion, and the receiver
// takes its context out of the map in between. wait_completion() then does
// not find the tag in the map, but must not return EINVAL while the
// receiver is still collecting the context.
TEST(OutOfOrderTimeout, collected_between_issue_and_wait) {
    auto engine = new_ooo_execution_engine();
    DEFER(delete_ooo_execution_engine(engine));
    FakeSource src;

    OutOfOrderContext a;
    a.engine = engine;
    a.do_issue.bind(nullptr, &null_op);
    a.do_completion.bind(&src, &FakeSource::complete);
    a.do_collect.bind(nullptr, &null_op);
    int a_ret = -100;
    auto tha = thread_enable_join(thread_create11([&] {
        ASSERT_EQ(0, ooo_issue_operation(a));
        a_ret = ooo_wait_completion(a);
    }));
    thread_yield();  // let A issue and become the receiver
    // do_completion overwrites a.tag with whatever it pops, so save the
    // original tag now for completing A's own operation later
    auto a_tag = a.tag;

    SlowCollect sc{50 * 1000};
    OutOfOrderContext b;
    b.engine = engine;
    b.do_issue.bind(&src, &FakeSource::issue);  // completes immediately
    b.do_completion.bind(&src, &FakeSource::complete);
    b.do_collect.bind(&sc, &SlowCollect::collect);
    int b_ret = -100;
    auto thb = thread_enable_join(thread_create11([&] {
        ASSERT_EQ(0, ooo_issue_operation(b));
        // yield into the window where the receiver has erased b's tag from
        // the map and is sleeping in do_collect()
        thread_usleep(10 * 1000);
        b_ret = ooo_wait_completion(b);
    }));
    thread_join(thb);

    EXPECT_EQ(0, b_ret);
    EXPECT_EQ(1, sc.collected);
    EXPECT_EQ((int)OooPhase::COLLECTED, (int)b.phase);
    EXPECT_EQ(nullptr, b.th);

    src.done.push(a_tag);
    thread_join(tha);
    EXPECT_EQ(0, a_ret);
    EXPECT_EQ(0, ooo_get_queue_count(engine));
}

// do_issue() fails after the receiver has already taken the context out of
// the map (the tag was pushed before the failure, and do_issue yielded).
// The failing caller must wait for the hand-back in the do_issue failure
// path before destroying its stack frame.
TEST(OutOfOrderTimeout, issue_failure_during_collect) {
    log_output = log_output_null;  // silence the expected do_issue error
    DEFER(log_output = log_output_stdout);
    auto engine = new_ooo_execution_engine();
    DEFER(delete_ooo_execution_engine(engine));
    FakeSource src;

    OutOfOrderContext a;
    a.engine = engine;
    a.do_issue.bind(nullptr, &null_op);
    a.do_completion.bind(&src, &FakeSource::complete);
    a.do_collect.bind(nullptr, &null_op);
    int a_ret = -100;
    auto tha = thread_enable_join(thread_create11([&] {
        ASSERT_EQ(0, ooo_issue_operation(a));
        a_ret = ooo_wait_completion(a);
    }));
    thread_yield();  // let A issue and become the receiver
    // do_completion overwrites a.tag with whatever it pops, so save the
    // original tag now for completing A's own operation later
    auto a_tag = a.tag;

    // B's do_issue pushes b.tag and yields for 10ms before failing; the
    // receiver A pops the tag, takes B's context out of the map and sleeps
    // 100ms in do_collect(), so do_issue reports failure inside the window
    SlowCollect sc{100 * 1000};
    FailAfterCollectIssue fi{&src, 10 * 1000};
    OutOfOrderContext b;
    b.engine = engine;
    b.do_issue.bind(&fi, &FailAfterCollectIssue::issue);
    b.do_completion.bind(&src, &FakeSource::complete);
    b.do_collect.bind(&sc, &SlowCollect::collect);
    int b_ret = -100;
    auto thb = thread_enable_join(thread_create11([&] {
        b_ret = ooo_issue_operation(b);
    }));
    thread_join(thb);

    // issue failed, but only after the hand-back was done
    EXPECT_EQ(-1, b_ret);
    EXPECT_EQ(1, sc.collected);
    EXPECT_EQ((int)OooPhase::COLLECTED, (int)b.phase);
    EXPECT_EQ(nullptr, b.th);

    src.done.push(a_tag);
    thread_join(tha);
    EXPECT_EQ(0, a_ret);
    EXPECT_EQ(0, ooo_get_queue_count(engine));
}

// N concurrent callers with tiny timeouts against slow, rotating receivers.
// Callers may be collected in time (0), time out before being collected
// (-1/ETIMEDOUT), or abort as a receiver on a stale tag (-2/ENOENT); in all
// cases there must be no crash and the map must drain completely.
TEST(OutOfOrderTimeout, concurrent_timeout_stress) {
    log_output = log_output_null;  // silence expected timeout/drop errors
    DEFER(log_output = log_output_stdout);
    auto engine = new_ooo_execution_engine();
    DEFER(delete_ooo_execution_engine(engine));
    FakeSource src;
    SlowCollect sc{5 * 1000};

    constexpr int N = 50;
    int done = 0;
    std::vector<join_handle*> ths;
    for (int i = 0; i < N; ++i) {
        ths.push_back(thread_enable_join(thread_create11([&] {
            OutOfOrderContext args;
            args.engine = engine;
            args.do_issue.bind(&src, &FakeSource::issue);
            args.do_completion.bind(&src, &FakeSource::complete);
            args.do_collect.bind(&sc, &SlowCollect::collect);
            args.timeout = Timeout(1000);
            int ret = ooo_issue_wait(args);
            EXPECT_TRUE(ret == 0 || ret == -1 || ret == -2);
            done++;
        })));
    }
    for (auto th : ths)
        thread_join(th);
    EXPECT_EQ(N, done);
    EXPECT_EQ(0, ooo_get_queue_count(engine));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    return RUN_ALL_TESTS();
}
