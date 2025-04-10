#include <cstdlib>
#include <thread>
#include <chrono>
#include <photon/common/alog.h>
#include <photon/common/throttle.h>
#include <photon/common/utility.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>
#include "../../test/gtest.h"
#include "../../test/ci-tools.h"

TEST(Throttle, basic) {
    // baseline
    uint64_t total = 10UL * 1024 * 1024;
    auto start = std::chrono::steady_clock::now();
    while (total) {
        // assume each step may consume about 4K ~ 1M
        auto step = rand() % (1UL * 1024 * 1024 - 4096) + 4096;
        if (step > total) step = total;
        total -= step;
    }
    auto end = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    auto baseline = duration.count();
    LOG_INFO("cosume 10M unit in ` us", DEC(baseline).comma(true));

    // try update time
    photon::thread_yield();
    // using throttle to limit increasing count 1M in seconds
    photon::throttle t(1UL * 1024 * 1024);

    // suppose to be done in at least 9 seconds
    total = 10UL * 1024 * 1024;
    start = std::chrono::steady_clock::now();
    while (total) {
        // assume each step may consume about 4K ~ 1M
        auto step = rand() % (1UL * 1024 * 1024 - 4096) + 4096;
        if (step > total) step = total;
        t.consume(step);
        total -= step;
    }
    end = std::chrono::steady_clock::now();
    duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    LOG_INFO("cosume 10M with 1M throttle in ` us",
             DEC(duration.count()).comma(true));
    EXPECT_GT((uint64_t) duration.count(), 9UL * 1000 * 1000);
}

TEST(Throttle, restore) {
    // try update time
    photon::thread_yield();
    // using throttle to limit increasing count 1M in seconds
    photon::throttle t(1UL * 1024 * 1024);

    // suppose to be done in at least 9 seconds
    auto total = 10UL * 1024 * 1024;
    uint64_t submit = 0, restore = 0;
    auto start = std::chrono::steady_clock::now();
    while (total) {
        // assume each step may consume about 4K ~ 1M
        auto step = rand() % (1UL * 1024 * 1024 - 4096) + 4096;
        if (step > total) step = total;
        submit += step;
        t.consume(step);
        if (rand() % 2) {
            // 1 of 2 chance to fail and restore consumed chance
            t.restore(step);
            restore += step;
        } else {
            total -= step;
        }
    }
    auto end = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    LOG_INFO("cosume 10M with 1M throttle in ` us",
             DEC(duration.count()).comma(true));
    LOG_INFO("submit ` unit resource acquire, restored ` unit",
             DEC(submit).comma(true), DEC(restore).comma(true));
    EXPECT_GT((uint64_t) duration.count(), 9UL * 1000 * 1000);
    EXPECT_LT((uint64_t) duration.count(), 20UL * 1000 * 1000);
}

TEST(Throttle, pulse) {
    // try update time
    photon::thread_yield();
    // using throttle to limit increasing count 1M in seconds
    photon::throttle t(1UL * 1024 * 1024);

    // suppose to be done in at least 9 seconds
    auto total = 10UL * 1024 * 1024;
    auto start = std::chrono::steady_clock::now();
    while (total) {
        // assume each step may consume 256K
        auto step = 256UL * 1024;
        if (step > total) step = total;
        t.consume(step);
        total -= step;
        photon::thread_usleep(110UL * 1000);
    }
    auto end = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    LOG_INFO("cosume 10M with 1M throttle in ` us",
             DEC(duration.count()).comma(true));
    EXPECT_GT((uint64_t) duration.count(), 9UL * 1000 * 1000);
    EXPECT_LT((uint64_t) duration.count(), 20UL * 1000 * 1000);
}

template <typename IDLE>
void test_with_idle(IDLE&& idle) {
    // try update time
    photon::thread_yield();
    // using throttle to limit increasing count 1M in seconds
    photon::throttle t(1UL * 1024 * 1024);

    // suppose to be done in at least 9 seconds
    auto start = std::chrono::steady_clock::now();
    t.consume(1UL * 1024 * 1024);
    // now all throttled resources are consumed
    idle();
    t.consume(1UL * 1024 * 1024);
    auto end = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    LOG_INFO("cosume 2M with 1M throttle in ` us",
             DEC(duration.count()).comma(true));
    EXPECT_GT((uint64_t) duration.count(), 1UL * 1000 * 1000);
    EXPECT_LT((uint64_t) duration.count(), 2UL * 1000 * 1000);
}

TEST(Throttle, no_sleep) {
    test_with_idle([] {});
}

TEST(Throttle, short_sleep) {
    test_with_idle([] { photon::thread_usleep(200); });
}

TEST(Throttle, long_sleep) {
    test_with_idle([] { photon::thread_usleep(1100UL * 1000); });
}

TEST(Throttle, try_consume) {
    // using throttle to limit action
    photon::throttle t(100);
    // try update time
    photon::thread_yield();
    auto start = photon::now;
    uint64_t count = 0;
    uint64_t failure = 0;
    while (photon::now - start < 10UL * 1000 * 1000) {
        if (t.try_consume(1) == 0) {
            count++;
        } else {
            failure++;
        }
        photon::thread_yield();
    }
    LOG_INFO("Act ` times in 10 sec, prevent ` acts", DEC(count).comma(true),
             DEC(failure).comma(true));
    EXPECT_LT(count, 11000UL);
}

////////////////////////////////////////
// The sleep and semaphore in macOS is less efficient and always cause variance, so skip macOS
#ifndef __APPLE__

struct FindAppropriateSliceNumSuite {
    uint64_t slice_num;
    double performance_loss_max_ratio;
};

class FindAppropriateSliceNumTest : public testing::TestWithParam<FindAppropriateSliceNumSuite> {
};

// More slices in a time window means sleep more frequently.
// In a fixed total time (10s), we measure how many IO were throttled (consumed),
// and compare to the dest amount, figure out the loss ratio brought by the throttler.
TEST_P(FindAppropriateSliceNumTest, run) {
    const auto& p = GetParam();

    const uint64_t test_time_sec = 10;
    const uint64_t bw = 100'000'000UL;
    const uint64_t time_window = 1'000'000UL;
    const uint64_t slice_num = p.slice_num;
    const uint64_t io_interval = time_window / slice_num;
    const uint64_t bs_per_io = bw / (time_window / io_interval);

    photon::throttle t(bw, time_window, slice_num);
    std::atomic<bool> running{true};
    uint64_t bytes = 0;

    std::thread([&] {
        ::sleep(test_time_sec);
        running = false;
    }).detach();

    while (running) {
        photon::thread_usleep(io_interval);
        if (!running) break;
        t.consume(bs_per_io);
        bytes += bs_per_io;
    }
    auto goal = bw * test_time_sec;
    auto diff = int64_t(bytes) - int64_t(goal);
    auto loss = double(std::abs(diff)) / double(goal);
    LOG_INFO("Consume ` bytes in ` seconds, loss ratio `", bytes, test_time_sec, loss);
    GTEST_ASSERT_LE(loss, p.performance_loss_max_ratio);
}

#ifdef INSTANTIATE_TEST_SUITE_P
#define INSTANTIATE_TEST_P INSTANTIATE_TEST_SUITE_P
#else
#define INSTANTIATE_TEST_P INSTANTIATE_TEST_CASE_P
#endif

INSTANTIATE_TEST_P(Throttle,
        FindAppropriateSliceNumTest, testing::Values(
        FindAppropriateSliceNumSuite{10, 0.01},
        FindAppropriateSliceNumSuite{50, 0.02},
        FindAppropriateSliceNumSuite{100, 0.03},
        FindAppropriateSliceNumSuite{500, 0.08},
        FindAppropriateSliceNumSuite{1000, 0.08},
        FindAppropriateSliceNumSuite{5000, 0.85}    // Unacceptable
));

////////////////////////////////////////

struct PriorityTestSuite {
    enum Type {
        Simulate,
        RealSocket,
    };
    struct IOConfig {
        uint64_t bw;    // bandwidth per second
        uint64_t bs;    // block size per IO
        photon::throttle::Priority prio;
    };

    Type type;
    uint64_t limit_bw;
    IOConfig io1;
    IOConfig io2;
    double bw1_ratio_min, bw1_ratio_max;
    double bw2_ratio_min, bw2_ratio_max;
};

class ThrottlePriorityTest : public testing::TestWithParam<PriorityTestSuite> {
protected:
    std::atomic<bool> running_{true};
};

INSTANTIATE_TEST_P(Throttle, ThrottlePriorityTest, testing::Values(
        PriorityTestSuite{
                // 0. Simulate same priority and equally divide the BW
                PriorityTestSuite::Simulate,
                100'000'000,
                {50'000'000, 100'000, photon::throttle::Priority::High},
                {50'000'000, 100'000, photon::throttle::Priority::High},
                0.4, 0.6,
                0.4, 0.6,
        },
        PriorityTestSuite{
                // 1. Simulate same priority but different BW, results are still the same
                PriorityTestSuite::Simulate,
                100'000'000,
                {50'000'000, 1'000'000, photon::throttle::Priority::High},
                {150'000'000, 2'000'000, photon::throttle::Priority::High},
                0.4, 0.6,
                0.4, 0.6,
        },
        PriorityTestSuite{
                // 2. Simulate different priorities with the same BW. Total BW exceeds limit
                PriorityTestSuite::Simulate,
                100'000'000,
                {100'000'000, 500'000, photon::throttle::Priority::High},
                {100'000'000, 500'000, photon::throttle::Priority::Low},
                0.9, 1.0,
                0.0, 0.1,
        },
        PriorityTestSuite{
                // 3. Simulate different priorities with the same BW. Total BW under the limit
                PriorityTestSuite::Simulate,
                100'000'000,
                {30'000'000, 1'000'000, photon::throttle::Priority::High},
                {70'000'000, 1'000'000, photon::throttle::Priority::Low},
                0.25, 0.35,
                0.3, 0.7,
        },
        PriorityTestSuite{
                // 4. Simulate different priorities with different BW. Total BW exceeds limit
                PriorityTestSuite::Simulate,
                100'000'000,
                {50'000'000, 5'000'000, photon::throttle::Priority::High},
                {200'000'000, 10'000'000, photon::throttle::Priority::Low},
                0.4, 0.55,
                0.45, 0.6,
        },
        PriorityTestSuite{
                // 5. Real socket. For now there is no way to balance throttle throughput of the same priority.
                // Maybe we need a WFQ in the future.
                PriorityTestSuite::RealSocket,
                1'000'000'000,
                {1'000'000'000, 1048576, photon::throttle::Priority::High},
                {1'000'000'000, 1048576, photon::throttle::Priority::High},
                0.0, 1.0,
                0.0, 1.0,
        },
        PriorityTestSuite{
                // 6. Real socket. High priority get most BW
                PriorityTestSuite::RealSocket,
                1'000'000'000,
                {800'000'000, 32768, photon::throttle::Priority::High},
                {800'000'000, 32768, photon::throttle::Priority::Low},
                0.7, 1.1,
                0.1, 0.3,
        },
        PriorityTestSuite{
                // 7. Real socket. Low priority gets the rest BW that high priority doesn't need
                PriorityTestSuite::RealSocket,
                100'000'000,
                {50'000'000, 10'000, photon::throttle::Priority::High},
                {1'000'000'000, 4'000'000, photon::throttle::Priority::Low},
                0.4, 0.6,
                0.4, 0.6,
        }
));

static void run_real_socket(const std::atomic<bool>& running, const PriorityTestSuite& p,
                            uint64_t& bw1, uint64_t& bw2) {
    photon::semaphore stream_counter;
    photon::throttle t(p.limit_bw);
    uint64_t buf_size = std::max(p.io1.bs, p.io2.bs);
    auto server = photon::net::new_tcp_socket_server();
    ASSERT_NE(nullptr, server);
    DEFER(delete server);

    auto handler = [&](photon::net::ISocketStream* sock) -> int {
        char buf[buf_size];
        while (running) {
            ssize_t ret = sock->recv(buf, buf_size);
            if (ret <= 0) break;
            photon::thread_yield();
        }
        stream_counter.signal(1);
        return 0;
    };

    int ret = server->setsockopt<int>(SOL_SOCKET, SO_REUSEPORT, 1);
    ASSERT_EQ(0, ret);
    server->set_handler(handler);
    ret = server->bind_v4any(0);
    ASSERT_EQ(0, ret);
    ret = server->listen();
    ASSERT_EQ(0, ret);
    ret = server->start_loop(false);
    ASSERT_EQ(0, ret);

    auto server_ep = server->getsockname();
    auto cli = photon::net::new_tcp_socket_client();
    ASSERT_NE(nullptr, cli);
    DEFER(delete cli);

    auto client_th1 = photon::thread_create11([&] {
        photon::throttle src(p.io1.bw);
        auto conn = cli->connect(server_ep);
        if (!conn) exit(1);
        DEFER(delete conn);
        char buf[buf_size] = {};
        while (running) {
            src.consume(p.io1.bs);
            ssize_t ret = conn->send(buf, p.io1.bs);
            if (ret <= 0) break;
            bw1 += p.io1.bs;
            t.consume(p.io1.bs, p.io1.prio);
        }
    });
    thread_enable_join(client_th1);

    auto client_th2 = photon::thread_create11([&] {
        photon::throttle src(p.io2.bw);
        auto conn = cli->connect(server_ep);
        if (!conn) exit(1);
        DEFER(delete conn);
        char buf[buf_size] = {};
        while (running) {
            src.consume(p.io2.bs);
            ssize_t ret = conn->send(buf, p.io2.bs);
            if (ret <= 0) break;
            bw2 += p.io2.bs;
            t.consume(p.io2.bs, p.io2.prio);
        }
    });
    thread_enable_join(client_th2);

    photon::thread_join((photon::join_handle*) client_th1);
    photon::thread_join((photon::join_handle*) client_th2);

    stream_counter.wait(2);
}

static void run_simulate(const std::atomic<bool>& running, const PriorityTestSuite& p,
                         uint64_t& bw1, uint64_t& bw2) {
    photon::throttle t(p.limit_bw);
    photon::semaphore sem;
    photon::thread_create11([&] {
        uint64_t sleep_interval = 1'000'000UL / (p.io1.bw / p.io1.bs);
        while (running) {
            photon::thread_usleep(sleep_interval);
            t.consume(p.io1.bs, p.io1.prio);
            bw1 += p.io1.bs;
        }
        sem.signal(1);
    });
    photon::thread_create11([&] {
        uint64_t sleep_interval = 1'000'000UL / (p.io2.bw / p.io2.bs);
        while (running) {
            photon::thread_usleep(sleep_interval);
            t.consume(p.io2.bs, p.io2.prio);
            bw2 += p.io2.bs;
        }
        sem.signal(1);
    });
    sem.wait(2);
}

TEST_P(ThrottlePriorityTest, run) {
    const auto& p = GetParam();
    const uint64_t test_time_sec = 10;
    uint64_t bw1 = 0, bw2 = 0;

    std::thread watcher([&] {
        ::sleep(test_time_sec);
        running_ = false;
    });

    if (p.type == PriorityTestSuite::Simulate)
        run_simulate(running_, p, bw1, bw2);
    else if (p.type == PriorityTestSuite::RealSocket)
        run_real_socket(running_, p, bw1, bw2);

    bw1 /= test_time_sec;
    bw2 /= test_time_sec;
    double ratio1 = double(bw1) / double(p.limit_bw);
    double ratio2 = double(bw2) / double(p.limit_bw);
    LOG_INFO(VALUE(bw1), VALUE(bw2), VALUE(ratio1), VALUE(ratio2));
    EXPECT_GE(ratio1, p.bw1_ratio_min);
    EXPECT_LE(ratio1, p.bw1_ratio_max);
    EXPECT_GE(ratio2, p.bw2_ratio_min);
    EXPECT_LE(ratio2, p.bw2_ratio_max);

    watcher.join();
}
#endif

int main(int argc, char** argv) {
    int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    if (ret) return -1;
    DEFER(photon::fini());
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
