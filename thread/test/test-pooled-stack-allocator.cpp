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

#include "../../test/gtest.h"
#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/stack-allocator.h>
#include <photon/thread/thread-pool.h>
#include <photon/thread/workerpool.h>

using namespace photon;

static constexpr uint64_t N = 10000;

void do_test(int pool_mode, const PhotonOptions& options) {
    init(INIT_EVENT_DEFAULT, INIT_IO_NONE, options);
    WorkPool pool(4, INIT_EVENT_DEFAULT, INIT_IO_NONE, pool_mode);
    semaphore sem(0);
    auto start = now;
    for (uint64_t i = 0; i < N; i++) {
        pool.async_call(new auto([&] { sem.signal(1); }));
    }
    sem.wait(N);
    LOG_TEMP("Spent ` us", now - start);
    fini();
}
/*
TEST(Normal, NoPool) {
    do_test(0, {});
}

TEST(Normal, ThreadPool) {
    do_test(64, {});
}
*/
TEST(PooledAllocator, PooledStack) {
    PhotonOptions opt;
    opt.use_pooled_stack_allocator = true;
    do_test(0, opt);
}

TEST(PooledAllocator, BypassThreadPool) {
    PhotonOptions opt;
    opt.use_pooled_stack_allocator = true;
    opt.bypass_threadpool = true;
    do_test(64, opt);
}

int main(int argc, char** arg) {
    ::testing::InitGoogleTest(&argc, arg);
    set_log_output_level(ALOG_WARN);
    return RUN_ALL_TESTS();
}
