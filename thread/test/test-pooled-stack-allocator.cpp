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
#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/stack-allocator.h>
#include <photon/thread/thread-pool.h>
#include <photon/thread/workerpool.h>

static constexpr uint64_t N = 10000;

uint64_t do_test(int mode) {
    photon::WorkPool pool(4, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE,
                          mode);
    photon::semaphore sem(0);
    auto start = photon::now;
    for (int i = 0; i < N; i++) {
        pool.async_call(new auto([&] { sem.signal(1); }));
    }
    sem.wait(N);
    auto done = photon::now;
    return done - start;
}

TEST(Normal, NoPool) {
    photon::set_photon_thread_stack_allocator();
    photon::init();
    DEFER(photon::fini());
    auto spend = do_test(0);
    LOG_TEMP("Spent ` us", spend);
}

TEST(Normal, ThreadPool) {
    photon::set_photon_thread_stack_allocator();
    photon::init();
    DEFER(photon::fini());
    auto spend = do_test(64);
    LOG_TEMP("Spent ` us", spend);
}

TEST(PooledAllocator, PooledStack) {
    photon::use_pooled_stack_allocator();
    photon::init();
    DEFER(photon::fini());
    auto spend = do_test(0);
    LOG_TEMP("Spent ` us", spend);
}

TEST(PooledAllocator, BypassThreadPool) {
    photon::use_pooled_stack_allocator();
    photon::set_bypass_threadpool();
    photon::init();
    DEFER(photon::fini());
    auto spend = do_test(64);
    LOG_TEMP("Spent ` us", spend);
}

int main(int argc, char** arg) {
    ::testing::InitGoogleTest(&argc, arg);
    set_log_output_level(ALOG_WARN);
    return RUN_ALL_TESTS();
}