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

// Exercise the production submission, drain and reaping paths with a memory
// ring. Publishing one CQE per reap makes completion ordering deterministic
// and does not require kernel io_uring support.
#include "../iouring-wrapper.cpp"
#include <algorithm>
#include <photon/photon.h>
#include "../../test/gtest.h"

struct CompletionRing {
    static constexpr unsigned CAPACITY = 8;
    photon::iouringEngine engine;
    io_uring ring{};
    io_uring_sqe sqes[CAPACITY]{};
    io_uring_cqe cqes[CAPACITY]{};
    unsigned sqHead = 0, cqHead = 0, cqTail = 0;

    CompletionRing() {
        ring.sq.khead = &sqHead;
        ring.sq.ring_mask = CAPACITY - 1;
        ring.sq.ring_entries = CAPACITY;
        ring.sq.sqes = sqes;
        ring.cq.khead = &cqHead;
        ring.cq.ktail = &cqTail;
        ring.cq.ring_mask = CAPACITY - 1;
        ring.cq.ring_entries = CAPACITY;
        ring.cq.cqes = cqes;
        engine.m_ring = &ring;
    }

    ~CompletionRing() {
        // The memory ring is owned by this fixture, not by liburing.
        engine.m_ring = nullptr;
    }

    void complete(unsigned sqeIndex, int result) {
        auto& cqe = cqes[cqTail & (CAPACITY - 1)];
        cqe = {};
        cqe.user_data = sqes[sqeIndex].user_data;
        cqe.res = result;
        ++cqTail;
        EXPECT_EQ(0, engine.reap_events());
        photon::thread_yield();
    }
};

static void interrupted_completion_order(const unsigned* order, unsigned count,
                                         int ioResult, int cancelResult,
                                         bool fullQueue = false, bool timed = true) {
    CompletionRing fixture;
    bool returned = false;
    int result = 0, error = 0;
    auto waiter = photon::thread_create11([&] {
        auto sqe = fixture.engine._get_sqe();
        io_uring_prep_nop(sqe);
        auto timeout = timed ? photon::Timeout(1000000) : photon::Timeout();
        result = fixture.engine._async_io(sqe, timeout, 0);
        error = errno;
        returned = true;
    });
    auto join = photon::thread_enable_join(waiter);
    photon::thread_yield();
    EXPECT_EQ(timed ? 2U : 1U, fixture.ring.sq.sqe_tail);
    if (fullQueue)
        fixture.ring.sq.sqe_tail = CompletionRing::CAPACITY;

    photon::thread_interrupt(waiter, EINTR);
    photon::thread_yield(); // enter the cancellation drain
    EXPECT_FALSE(returned);
    if (!fullQueue) {
        EXPECT_EQ(timed ? 3U : 2U, fixture.ring.sq.sqe_tail); // async cancel
    }

    for (unsigned i = 0; i < count; ++i) {
        auto index = order[i];
        unsigned cancelIndex = timed ? 2 : 1;
        int res = index == 0 ? ioResult : (index == cancelIndex ? cancelResult : -ECANCELED);
        fixture.complete(index, res);
        if (i + 1 < count) {
            EXPECT_FALSE(returned); // stack contexts must stay alive
            // A second external interruption must not end the drain early.
            photon::thread_interrupt(waiter, ESHUTDOWN);
            photon::thread_yield();
            EXPECT_FALSE(returned);
        }
    }
    EXPECT_TRUE(returned) << "final CQE left the cancellation drain asleep";
    if (!returned) {
        // All CQEs are consumed, so an extra wake can safely rescue a broken
        // implementation and let the test report failure instead of hanging.
        photon::thread_interrupt(waiter, EINTR);
    }
    photon::thread_join(join);
    EXPECT_EQ(-1, result);
    EXPECT_EQ(EINTR, error); // preserve the original external interruption
}

TEST(iouring_completion, interrupted_all_completion_orders) {
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE));
    DEFER(photon::fini());
    for (int ioResult : {1, -ECANCELED}) {
        for (int cancelResult : {0, -ENOENT, -ECANCELED}) {
            unsigned order[] = {0, 1, 2};
            do {
                SCOPED_TRACE(testing::Message() << "io=" << ioResult
                    << " cancel=" << cancelResult << " order="
                    << order[0] << order[1] << order[2]);
                interrupted_completion_order(order, 3, ioResult, cancelResult);
            } while (std::next_permutation(order, order + 3));
        }
    }
}

TEST(iouring_completion, untimed_completion_orders) {
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE));
    DEFER(photon::fini());
    for (int ioResult : {1, -ECANCELED}) {
        for (int cancelResult : {0, -ENOENT, -ECANCELED}) {
            unsigned order[] = {0, 1};
            do {
                SCOPED_TRACE(testing::Message() << "io=" << ioResult
                    << " cancel=" << cancelResult << " order="
                    << order[0] << order[1]);
                interrupted_completion_order(order, 2, ioResult, cancelResult, false, false);
            } while (std::next_permutation(order, order + 2));
        }
    }
}

TEST(iouring_completion, full_submission_queue_completion_orders) {
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE));
    DEFER(photon::fini());
    for (int ioResult : {1, -ECANCELED}) {
        unsigned order[] = {0, 1};
        do {
            SCOPED_TRACE(testing::Message() << "io=" << ioResult
                << " order=" << order[0] << order[1]);
            interrupted_completion_order(order, 2, ioResult, 0, true);
        } while (std::next_permutation(order, order + 2));
    }
}

TEST(iouring_completion, normal_success_and_timeout_wakeup) {
    ASSERT_EQ(0, photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE));
    DEFER(photon::fini());
    for (bool timeout : {false, true}) {
        CompletionRing fixture;
        bool returned = false;
        int result = 0, error = 0;
        auto waiter = photon::thread_create11([&] {
            auto sqe = fixture.engine._get_sqe();
            io_uring_prep_nop(sqe);
            result = fixture.engine._async_io(sqe, 1000000, 0);
            error = errno;
            returned = true;
        });
        auto join = photon::thread_enable_join(waiter);
        photon::thread_yield();
        // A canceled I/O defers to its timeout; a canceled timeout defers to
        // successful I/O. Neither may wake a normal waiter on its own.
        fixture.complete(timeout ? 0 : 1, -ECANCELED);
        EXPECT_FALSE(returned);
        fixture.complete(timeout ? 1 : 0, timeout ? -ETIME : 1);
        EXPECT_TRUE(returned);
        if (!returned) photon::thread_interrupt(waiter, EINTR);
        photon::thread_join(join);
        EXPECT_EQ(timeout ? -1 : 1, result);
        if (timeout) {
            EXPECT_EQ(ETIMEDOUT, error);
        }
    }
}

int main(int argc, char** argv) {
    set_log_output_level(ALOG_ERROR);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
