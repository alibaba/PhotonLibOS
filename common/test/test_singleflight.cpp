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
#include <string>
#include <vector>
#include <atomic>

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

// Test void return type
TEST(VoidReturn, photonthreads) {
    photon::SingleFlight sc;
    std::vector<photon::join_handle*> jhs;
    std::atomic<int> count{0};
    for (int i = 0; i < 5; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                sc.Do([&] {
                    count.fetch_add(1);
                    photon::thread_sleep(1);
                });
            })));
    }
    for (auto& t : jhs) {
        photon::thread_join(t);
    }
    EXPECT_EQ(1, count.load());
}

// Test void return type with semaphore
TEST(VoidReturn, photonthreads_sem) {
    photon::SingleFlight sc;
    std::vector<photon::join_handle*> jhs;
    std::atomic<int> count{0};
    for (int i = 0; i < 5; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                sc.Do([&] {
                    count.fetch_add(1);
                    photon::thread_sleep(1);
                }, true);
            })));
    }
    for (auto& t : jhs) {
        photon::thread_join(t);
    }
    EXPECT_EQ(1, count.load());
}

// Test with string return type
TEST(StringReturn, photonthreads) {
    photon::SingleFlight sc;
    std::vector<photon::join_handle*> jhs;
    std::atomic<int> count{0};
    const std::string expected = "test_result_42";
    
    for (int i = 0; i < 10; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto result = sc.Do([&]() -> std::string {
                    count.fetch_add(1);
                    photon::thread_sleep(1);
                    return expected;
                });
                EXPECT_EQ(expected, result);
            })));
    }
    for (auto& t : jhs) {
        photon::thread_join(t);
    }
    EXPECT_EQ(1, count.load());
}

// Test sequential calls (no coalescing should happen)
TEST(Sequential, photonthreads) {
    photon::SingleFlight sc;
    std::atomic<int> count{0};
    
    for (int i = 0; i < 5; i++) {
        auto result = sc.Do([&] {
            count.fetch_add(1);
            return count.load();
        });
        EXPECT_EQ(i + 1, result);
    }
    EXPECT_EQ(5, count.load());
}

// Test with high concurrency
TEST(HighConcurrency, photonthreads) {
    photon::SingleFlight sc;
    std::vector<photon::join_handle*> jhs;
    std::atomic<int> count{0};
    const int num_threads = 100;
    
    for (int i = 0; i < num_threads; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto ret = sc.Do([&] {
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

// Test with high concurrency using semaphore
TEST(HighConcurrency, photonthreads_sem) {
    photon::SingleFlight sc;
    std::vector<photon::join_handle*> jhs;
    std::atomic<int> count{0};
    const int num_threads = 100;
    
    for (int i = 0; i < num_threads; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto ret = sc.Do([&] {
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

// Test mixed mode: some threads arrive late
TEST(MixedTiming, photonthreads) {
    photon::SingleFlight sc;
    std::vector<photon::join_handle*> jhs;
    std::atomic<int> count{0};
    
    // First wave of threads
    for (int i = 0; i < 5; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto ret = sc.Do([&] {
                    count.fetch_add(1);
                    photon::thread_sleep(2);
                    return count.load();
                });
                EXPECT_EQ(1, ret);
            })));
    }
    
    // Second wave arrives during execution
    photon::thread_sleep(1);
    for (int i = 0; i < 5; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto ret = sc.Do([&] {
                    count.fetch_add(1);
                    photon::thread_sleep(2);
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

// Test multiple independent SingleFlight instances
TEST(MultipleInstances, photonthreads) {
    photon::SingleFlight sc1;
    photon::SingleFlight sc2;
    std::atomic<int> count1{0};
    std::atomic<int> count2{0};
    std::vector<photon::join_handle*> jhs;
    
    // Threads using sc1
    for (int i = 0; i < 5; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                sc1.Do([&] {
                    count1.fetch_add(1);
                    photon::thread_sleep(1);
                });
            })));
    }
    
    // Threads using sc2
    for (int i = 0; i < 5; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                sc2.Do([&] {
                    count2.fetch_add(1);
                    photon::thread_sleep(1);
                });
            })));
    }
    
    for (auto& t : jhs) {
        photon::thread_join(t);
    }
    
    // Each instance should execute once
    EXPECT_EQ(1, count1.load());
    EXPECT_EQ(1, count2.load());
}

// Test rapid successive calls
TEST(RapidCalls, photonthreads) {
    photon::SingleFlight sc;
    std::atomic<int> total_executions{0};
    std::vector<photon::join_handle*> jhs;
    
    // Launch multiple batches rapidly
    for (int batch = 0; batch < 5; batch++) {
        for (int i = 0; i < 10; i++) {
            jhs.emplace_back(
                photon::thread_enable_join(photon::thread_create11([&] {
                    sc.Do([&] {
                        total_executions.fetch_add(1);
                        photon::thread_usleep(100 * 1000); // 100ms
                    });
                })));
        }
        photon::thread_usleep(10 * 1000); // Small delay between batches
    }
    
    for (auto& t : jhs) {
        photon::thread_join(t);
    }
    
    // Should execute at least once, but possibly more due to timing
    EXPECT_GE(total_executions.load(), 1);
    EXPECT_LE(total_executions.load(), 5); // At most one per batch
    LOG_INFO("Rapid calls executed ` times", total_executions.load());
}

// Test with very quick operation (no sleep)
TEST(QuickOperation, photonthreads) {
    photon::SingleFlight sc;
    std::atomic<int> count{0};
    std::vector<photon::join_handle*> jhs;
    
    for (int i = 0; i < 50; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto ret = sc.Do([&] {
                    count.fetch_add(1);
                    // No sleep - very quick operation
                    return count.load();
                });
                EXPECT_GE(ret, 1);
            })));
    }
    
    for (auto& t : jhs) {
        photon::thread_join(t);
    }
    
    // Due to quick execution, might execute multiple times
    LOG_INFO("Quick operation executed ` times", count.load());
    EXPECT_GE(count.load(), 1);
}

// Test mixed std::thread and photon threads
TEST(MixedThreads, mixed) {
    photon::SingleFlight sc;
    std::atomic<int> count{0};
    std::atomic<bool> executor_started{false};
    std::vector<std::thread> std_threads;
    std::vector<photon::join_handle*> photon_threads;
    
    // Launch std::threads
    for (int i = 0; i < 3; i++) {
        std_threads.emplace_back([&] {
            photon::init();
            DEFER(photon::fini());
            auto ret = sc.Do([&] {
                executor_started.store(true);
                count.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                return count.load();
            });
            // With mixed threads timing, multiple executions may occur
            EXPECT_GE(ret, 1);
        });
    }
    
    // Wait for executor to start
    while (!executor_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Now launch photon threads - should join the ongoing execution
    for (int i = 0; i < 3; i++) {
        photon_threads.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto ret = sc.Do([&] {
                    count.fetch_add(1);
                    photon::thread_sleep(2);
                    return count.load();
                });
                // Should get the result from the first execution
                EXPECT_GE(ret, 1);
            })));
    }
    
    for (auto& t : std_threads) {
        t.join();
    }
    for (auto& t : photon_threads) {
        photon::thread_join(t);
    }
    
    // Due to mixed thread timing, may execute once or twice
    LOG_INFO("Mixed threads executed ` times", count.load());
    EXPECT_GE(count.load(), 1);
    EXPECT_LE(count.load(), 2);
}

// Test return value correctness with complex type
struct ComplexResult {
    int value;
    std::string message;
    bool operator==(const ComplexResult& other) const {
        return value == other.value && message == other.message;
    }
};

TEST(ComplexReturn, photonthreads) {
    photon::SingleFlight sc;
    std::atomic<int> count{0};
    std::vector<photon::join_handle*> jhs;
    ComplexResult expected{42, "test_message"};
    
    for (int i = 0; i < 10; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create11([&] {
                auto result = sc.Do([&]() -> ComplexResult {
                    count.fetch_add(1);
                    photon::thread_sleep(1);
                    return expected;
                });
                EXPECT_EQ(expected.value, result.value);
                EXPECT_EQ(expected.message, result.message);
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
