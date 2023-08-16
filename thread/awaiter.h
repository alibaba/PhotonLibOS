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
    void suspend() { sem.wait(1); }
    void resume() { sem.signal(1); }
};

template <>
struct Awaiter<StdContext> {
    int err;
    std::promise<void> p;
    std::future<void> f;
    Awaiter() : f(p.get_future()) {}
    void suspend() { f.get(); }
    void resume() { return p.set_value(); }
};

template <>
struct Awaiter<AutoContext> {
    Awaiter<PhotonContext> pctx;
    Awaiter<StdContext> sctx;
    bool is_photon = false;
    Awaiter() : is_photon(photon::CURRENT) {}
    void suspend() {
        if (is_photon)
            pctx.suspend();
        else
            sctx.suspend();
    }
    void resume() {
        if (is_photon)
            pctx.resume();
        else
            sctx.resume();
    }
};
}  // namespace photon