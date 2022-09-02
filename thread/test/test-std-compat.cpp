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

#include <gtest/gtest.h>

#include <photon/thread/std-compat.h>
#include <photon/common/alog.h>

#define DO_LOG(...) do {                                    \
    std::hash<photon::std::thread::id> hasher;              \
    auto id = hasher(photon::std::this_thread::get_id());   \
    LOG_DEBUG("THREAD ID(`): ", id, __VA_ARGS__);           \
} while(0)

void func(int* x) {
    DO_LOG("sleep 1 second");
    photon::std::this_thread::sleep_for(std::chrono::seconds(1));
    (*x)++;
}

struct A {
    void func(int* x) {
        DO_LOG("sleep 1 second");
        photon::std::this_thread::sleep_for(std::chrono::seconds(1));
        (*x)++;
    }
};

TEST(std, thread) {
    int x = 0;

    // type 1
    photon::std::thread t1(func, &x);
    t1.join();

    // type 2
    auto t2 = photon::std::thread([&x]() {
        DO_LOG("sleep 1 second");
        photon::std::this_thread::sleep_for(std::chrono::seconds(1));
        x++;
    });
    t2.join();

    // type 3
    A a;
    photon::std::thread t3(&A::func, &a, &x);
    t3.detach();

    // wait all threads finished
    photon::thread_sleep(2);
    ASSERT_EQ(3, x);
}

TEST(std, unique_lock) {
    {
        photon::std::unique_lock<photon::std::mutex> a;
        ASSERT_FALSE(a.owns_lock());
    }

    photon::std::mutex mu;
    photon::std::thread th([&]{
        {
            // sleep 1 second, should fail to get lock
            photon::std::this_thread::sleep_for(std::chrono::seconds(1));
            photon::std::unique_lock<photon::std::mutex> lock(mu, std::try_to_lock);
            ASSERT_FALSE(lock.owns_lock());
        }
        {
            // still got 2 seconds remaining, keeps failing
            photon::std::unique_lock<photon::std::mutex> lock(mu, std::defer_lock);
            ASSERT_FALSE(lock.try_lock_until(std::chrono::system_clock::now() + std::chrono::seconds(1)));
        }
        {
            // try with extra 2 seconds, should succeed
            photon::std::unique_lock<photon::std::mutex> lock(mu, std::defer_lock);
            ASSERT_TRUE(lock.try_lock_for(std::chrono::seconds(2)));
        }
    });

    {
        // hold mutex for 3 seconds
        auto lock = photon::std::unique_lock<photon::std::mutex>(mu);
        photon::std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    th.join();
}

TEST(std, cv) {
    photon::std::mutex mu;
    photon::std::condition_variable cv;

    photon::std::thread th([&] {
        DO_LOG("sleep 1 second");
        photon::std::this_thread::sleep_for(std::chrono::seconds(1));
        photon::std::lock_guard<photon::std::mutex> lock(mu);
        cv.notify_one();
    });

    {
        photon::std::unique_lock<photon::std::mutex> lock(mu);
        cv.wait(lock);
        DO_LOG("wait done");
    }

    th.join();
}

TEST(std, cv_timeout) {
    photon::std::mutex mu;
    photon::std::condition_variable cv;

    photon::std::thread th([&]{
        DO_LOG("sleep 1 second");
        photon::std::this_thread::sleep_for(std::chrono::seconds(1));
        photon::std::lock_guard<photon::std::mutex> lock(mu);
        cv.notify_all();
    });

    {
        photon::std::unique_lock<photon::std::mutex> lock(mu);
        ASSERT_EQ(std::cv_status::timeout, cv.wait_for(lock, std::chrono::milliseconds(900)));
        DO_LOG("wait timeout done");
    }

    {
        photon::std::unique_lock<photon::std::mutex> lock(mu);
        ASSERT_EQ(std::cv_status::no_timeout, cv.wait_for(lock, std::chrono::milliseconds(1100)));
        DO_LOG("wait no_timeout done");
    }

    th.join();
}

TEST(std, exception) {
    photon::std::mutex mu;
    photon::std::unique_lock<photon::std::mutex> lock;
    try {
        lock.lock();
    } catch (std::system_error& err) {
        ASSERT_EQ(EPERM, err.code().value());
    }
}

int main(int argc, char** arg) {
    photon::thread_init();
    DEFER(photon::thread_fini());
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}