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

#include <gflags/gflags.h>
#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>

#include <chrono>
#include <thread>
#include <vector>
#include "photon/thread/thread.h"

DEFINE_uint64(threads, 8, "threads requires lock");
DEFINE_uint64(turn, 100000, "turns requiring lock for each thread");

template <typename locktype>
void lockperf(const char* name) {
    std::vector<std::thread> ths;
    locktype mtx;
    for (size_t i = 0; i < FLAGS_threads; i++) {
        ths.emplace_back([&] {
            photon::init();
            DEFER(photon::fini());
            auto start = std::chrono::system_clock::now();
            for (size_t j = 0; j < FLAGS_turn; j++) {
                mtx.lock();
                mtx.unlock();
            }
            auto end = std::chrono::system_clock::now();
            LOG_TEMP("` time: `", name, (end - start).count());
        });
    }
    for (auto& x : ths) x.join();
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    log_output_level = ALOG_WARN;
    photon::init();
    DEFER(photon::fini());
    lockperf<photon::spinlock>("spin");
    lockperf<photon::qspinlock>("qspin");
    lockperf<photon::mutex>("ticket");
    return 0;
}