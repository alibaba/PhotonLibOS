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

#include <errno.h>
#include <photon/thread/thread.h>
#include <photon/common/timeout.h>
#include <atomic>
#include <future>

namespace photon {

// Special context that supports calling executor in photon thread
struct PhotonContext {};

// Context for stdthread calling executor
struct StdContext {};

// Special context that can automaticlly choose `PhotonContext` or `StdContext`
// by if photon initialized in current environment
struct AutoContext {};

template <typename T>
struct Awaiter;

template <>
struct Awaiter<PhotonContext> {
    photon::semaphore sem;
    Awaiter() {}
    void resume() { sem.signal(1); }
    int suspend(Timeout timeout = {}) {
        return sem.wait(1, timeout);
    }
};

template <>
struct Awaiter<StdContext> {
    std::promise<void> p;
    void resume() { return p.set_value(); }
    int suspend(Timeout timeout = {}) {
        auto duration = timeout.std_duration();
        if (duration == std::chrono::microseconds().max()) {
            p.get_future().wait();
        } else {
            auto ret = p.get_future().wait_for(duration);
            if (ret == std::future_status::timeout) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
        return 0;
    }
};

template <>
struct Awaiter<AutoContext> {
    Awaiter<PhotonContext> pctx;
    Awaiter<StdContext> sctx;
    bool is_photon = false;
    Awaiter() : is_photon(photon::CURRENT) {}
    int suspend(Timeout timeout = {}) {
        return is_photon ? pctx.suspend(timeout) :
                           sctx.suspend(timeout) ;
    }
    void resume() {
        return is_photon ? pctx.resume() :
                           sctx.resume() ;
    }
};
}  // namespace photon