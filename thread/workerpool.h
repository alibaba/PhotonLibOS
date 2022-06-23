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

#include <photon/common/callback.h>
#include <photon/common/tuple-assistance.h>

#include <memory>
#include <utility>

namespace photon {

class WorkPool {
public:
    WorkPool(int thread_num, int ev_engine = 0, int io_engine = 0);

    WorkPool(const WorkPool& other) = delete;
    WorkPool& operator=(const WorkPool& rhs) = delete;

    ~WorkPool();

    template <typename F, typename... Args>
    void call(F&& f, Args&&... args) {
        auto task = [&] { f(std::forward<Args>(args)...); };
        do_call(task);
    }

    template <typename F>
    void call(F&& f) {  // in case f is a lambda
        do_call(f);
    }

    template <typename F, typename... Args>
    void async_call(F&& f, Args&&... args) {
#if __cplusplus < 201300L
        // capture by reference in C++11
        // it will acturally copy f and args once
        auto task = new auto([&]() mutable { f(std::forward<Args>(args)...); });
#else
        // or by value/move in C++14 on
        auto task = new auto([f = std::forward<F>(f),
                              pack = std::make_tuple(
                                  std::forward<Args>(args)...)]() mutable {
            tuple_assistance::apply(f, pack);
        });
#endif

        void (*func)(void*);
        func = [](void* task_) {
            using Task = decltype(task);
            auto t = (Task)task_;
            (*t)();
            delete t;
        };
        enqueue({func, task});
    }

protected:
    class impl;  // does not depend on T
    std::unique_ptr<impl> pImpl;
    // send delegate to run at a workerthread,
    // Caller should keep callable object and resources alive
    void do_call(Delegate<void> call);
    void enqueue(Delegate<void> call);
};

}  // namespace photon
