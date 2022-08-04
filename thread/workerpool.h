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
#include <photon/thread/thread.h>

#include <memory>
#include <utility>

namespace photon {

class WorkPool {
public:
    /**
     * @brief Construct a new Work Pool object
     *
     * @param vcpu_num how many VCPU (std threads) create for this workpool
     * @param ev_engine how to initial event engine as preset VCPUs
     * @param io_engine how to initial io engine as preset VCPUs
     * @param thread_mod threads work in which mode, -1 for non-thread mode, set
     * to 0 will create photon thread for every task, and >0 to create photon
     * thread in photon thread pool with this size.
     */
    explicit WorkPool(size_t vcpu_num, int ev_engine = 0, int io_engine = 0,
                      int thread_mod = -1);

    WorkPool(const WorkPool& other) = delete;
    WorkPool& operator=(const WorkPool& rhs) = delete;

    ~WorkPool();

    /**
     * @brief `hold_as_worker` makes blocks current std thread join workpool, as
     * a woker member. Noticed that worker should initial environment by self,
     * workpool will not do photon environment for it. When workpool destructed,
     * the function call will be finished, and return 0.
     * @return int 0 for success, and -1 for failure
     */
    int join_current_vcpu_into_workpool();

    /**
     * @brief Get the vcpu num
     * 
     * @return int 
     */
    int get_vcpu_num();

    /**
     * @brief `call` method deploy a callable object (usually lambda function)
     * into one of workpool vcpu, and wait till this function finished.
     *
     * @param f Callable object as a work task. Noticed that the return value of
     * f will not being collected.
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
     * @param task Pointer to async task callable object. Call by lamda could
     * using `workpool.async_call(new auto ([&](){ // some lambda; }));` The
     * ownership of callable object is moved to workpool, object will be delete
     * after task done.
     */
    template <typename Task>
    void async_call(Task* task) {
        enqueue({&WorkPool::__async_call_helper<Task>, task});
    }

    /**
     * @brief `thread_migrate` takes a thread to migrate to one of work-pool
     * managed vcpu.
     *
     * @param th Photon thread that goint to migrate
     * @param index Which vcpu in pool to migrate to. if index is not in range
     * [0, vcpu_num), it will choose random one in pool.
     * @return int 0 for success, and <0 means failed to migrate.
     */
    int thread_migrate(photon::thread* th = CURRENT, size_t index = -1UL) {
        return photon::thread_migrate(th, get_vcpu_in_pool(index));
    }

protected:
    class impl;  // does not depend on T
    std::unique_ptr<impl> pImpl;
    // send delegate to run at a workerthread,
    // Caller should keep callable object and resources alive
    void do_call(Delegate<void> call);
    void enqueue(Delegate<void> call);
    photon::vcpu_base* get_vcpu_in_pool(size_t index);

    template<typename Task>
    static void __async_call_helper(void* task) {
        auto t = (Task*)task;
        (*t)();
        delete t;
    }
};

}  // namespace photon
