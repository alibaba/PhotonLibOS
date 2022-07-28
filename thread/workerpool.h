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
#include <photon/thread/thread11.h>

#include <memory>
#include <utility>

namespace photon {

/**
 * @brief `WorkPool` is a helper to create background vcpu pool
 * to deploy works.
 * It will create `vcpu_num` std threads, and initialize photon environment,
 * make them works as a VCPU.
 * When `WorkPool` during destruction, all controlled std threads will be join.
 */
class WorkPool {
public:
    explicit WorkPool(size_t vcpu_num, int ev_engine = 0, int io_engine = 0);

    WorkPool(const WorkPool& other) = delete;
    WorkPool& operator=(const WorkPool& rhs) = delete;

    ~WorkPool();

    /**
     * @brief `call` method deploy a callable object (usually lambda function)
     * into one of workpool vcpu, and wait till this function finished.
     * 
     * @param f Callable object as a work task. Noticed that the return value of f 
     *          will not being collected.
     * @param args Arguments calling `f`
     */
    template <typename F, typename... Args>
    void call(F&& f, Args&&... args) {
        auto task = [&] { f(std::forward<Args>(args)...); };
        do_call(task);
    }

    template <typename F>
    void call(F&& f) {  // in case f is a lambda
        do_call(f);
    }

    /**
     * @brief `async_call` just like `call`, but do not wait for task done.
     * 
     * @param f Callable object as a work task. Noticed that the return value of f 
     *          will not being collected.
     * @param args Arguments calling `f`
     */
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

    /**
     * @brief `thread_migrate` takes a thread to migrate to one of work-pool managed
     * vcpu.
     * 
     * @param th Photon thread that goint to migrate
     * @param index Which vcpu in pool to migrate to. if index is not in range [0, vcpu_num),
     *              it will choose random one in pool.
     * @return int 0 for success, and <0 means failed to migrate.
     */
    int thread_migrate(photon::thread* th = CURRENT, size_t index = -1UL) {
        return photon::thread_migrate(th, get_vcpu_in_pool(index));
    }

    /**
     * @brief `thread_create` create a photon thread in one of work-pool vcpu
     * 
     * @param args as same as defined in `thread_create11`
     * @return int 0 for success, and <0 means failed to create
     */
    template <typename... Args>
    int thread_create(Args&&... args) {
        auto th = photon::thread_create11(std::forward<Args>(args)...);
        return thread_migrate(th);
    }

protected:
    class impl;  // does not depend on T
    std::unique_ptr<impl> pImpl;
    // send delegate to run at a workerthread,
    // Caller should keep callable object and resources alive
    void do_call(Delegate<void> call);
    void enqueue(Delegate<void> call);
    photon::vcpu_base* get_vcpu_in_pool(size_t index);
};

}  // namespace photon
