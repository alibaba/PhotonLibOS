#include <gtest/gtest.h>
#include <photon/common/alog.h>
#include <photon/common/throttle.h>
#include <photon/common/utility.h>
#include <photon/photon.h>

#include <chrono>

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
    EXPECT_GT(duration.count(), 9UL * 1000 * 1000);
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
    EXPECT_GT(duration.count(), 9UL * 1000 * 1000);
    EXPECT_LT(duration.count(), 20UL * 1000 * 1000);
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
    EXPECT_GT(duration.count(), 9UL * 1000 * 1000);
    EXPECT_LT(duration.count(), 20UL * 1000 * 1000);
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
    EXPECT_GT(duration.count(), 1UL * 1000 * 1000);
    EXPECT_LT(duration.count(), 2UL * 1000 * 1000);
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

int main(int argc, char** argv) {
    photon::init(0, 0);
    DEFER(photon::fini());
    testing::InitGoogleTest(&argc, argv);
    int ret = RUN_ALL_TESTS();
    LOG_INFO(VALUE(ret));
    return ret;
}
