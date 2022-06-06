#include <fcntl.h>
#include <gtest/gtest.h>
#include <photon/common/alog.h>
#include <photon/common/executor/executor.h>
#include <photon/common/executor/stdlock.h>
#include <photon/common/utility.h>
#include <photon/fs/exportfs.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread.h>
#include <sched.h>

#include <chrono>
#include <thread>

using namespace photon;

std::atomic<int> count(0), start(0);

int ftask(photon::Executor *eth, int i) {
    eth->async_perform([i] {
        // sleep for 3 secs
        LOG_INFO("Async work ` start", i);
        start++;
        photon::thread_sleep(3);
        LOG_INFO("Async work ` done", i);
        count++;
    });
    return 0;
}

TEST(std_executor, test) {
    photon::Executor eth;

    printf("Task applied, wait for loop\n");

    for (int i = 0; i < 10; i++) {
        ftask(&eth, i);
    }
    EXPECT_LT(count.load(), 10);
    while (start.load() != count.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(10, start.load());
    EXPECT_EQ(start.load(), count.load());
}
