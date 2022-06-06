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

#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

namespace photon {
struct StdCond {
    std::condition_variable cond;
    std::mutex &mtx;
    StdCond(std::mutex &mtx) : mtx(mtx) {}
    template <typename Lock>
    void wait(Lock &lock) {
        cond.wait(lock);
    }
    template <typename Lock>
    bool wait_for(Lock &lock, int64_t timeout) {
        if (timeout < 0) {
            cond.wait(lock);
            return true;
        }
        return cond.wait_for(lock, std::chrono::microseconds(timeout)) ==
               std::cv_status::no_timeout;
    }
    void notify_one() { cond.notify_one(); }
    void notify_all() { cond.notify_all(); }

    void lock() { mtx.lock(); }

    void unlock() { mtx.unlock(); }
};

struct StdContext {
    using Cond = StdCond;
    using CondLock = std::unique_lock<std::mutex>;
    using Lock = std::unique_lock<std::mutex>;
    using Mutex = std::mutex;
};

}  // namespace photon
