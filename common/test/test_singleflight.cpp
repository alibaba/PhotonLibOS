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

#include <photon/common/singleflight.h>
#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>

#include <chrono>
#include <thread>

#include "../../test/gtest.h"
#include "../../test/ci-tools.h"

TEST(Basic, stdthreads) {
    photon::SingleFlight sc;
    std::vector<std::thread> jhs;
    std::atomic<int> count{0};
    for (int i = 0; i < 3; i++) {
        jhs.emplace_back([&] {
            photon::init();
            DEFER(photon::fini());
            auto ret = sc.Do([&] {
                LOG_INFO("DO at `", photon::CURRENT);
                count.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                return count.load();
            });
            EXPECT_EQ(1, ret);
        });
    }
    for (auto& t : jhs) {
        t.join();
    }
    EXPECT_EQ(1, count.load());
}

TEST(Basic, photonthreads) {
    photon::SingleFlight sc;
    std::vector<photon::join_handle*> jhs;
    std::atomic<int> count{0};
    for (int i = 0; i < 3; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto ret = sc.Do([&] {
                    LOG_INFO("DO at `", photon::CURRENT);
                    count.fetch_add(1);
                    photon::thread_sleep(1);
                    return count.load();
                });
                EXPECT_EQ(1, ret);
            })));
    }
    for (auto& t : jhs) {
        photon::thread_join(t);
    }
    EXPECT_EQ(1, count.load());
}

TEST(UseSem, stdthreads) {
    photon::SingleFlight sc;
    std::vector<std::thread> jhs;
    std::atomic<int> count{0};
    for (int i = 0; i < 3; i++) {
        jhs.emplace_back([&] {
            photon::init();
            DEFER(photon::fini());
            auto ret = sc.Do([&] {
                LOG_INFO("DO at `", photon::CURRENT);
                count.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                return count.load();
            }, true);
            EXPECT_EQ(1, ret);
        });
    }
    for (auto& t : jhs) {
        t.join();
    }
    EXPECT_EQ(1, count.load());
}

TEST(UseSem, photonthreads) {
    photon::SingleFlight sc;
    std::vector<photon::join_handle*> jhs;
    std::atomic<int> count{0};
    for (int i = 0; i < 3; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto ret = sc.Do([&] {
                    LOG_INFO("DO at `", photon::CURRENT);
                    count.fetch_add(1);
                    photon::thread_sleep(1);
                    return count.load();
                }, true);
                EXPECT_EQ(1, ret);
            })));
    }
    for (auto& t : jhs) {
        photon::thread_join(t);
    }
    EXPECT_EQ(1, count.load());
}

int main(int argc, char** arg) {
    if (!photon::is_using_default_engine()) return 0;
    ::testing::InitGoogleTest(&argc, arg);
    photon::init();
    DEFER(photon::fini());
    return RUN_ALL_TESTS();
}
