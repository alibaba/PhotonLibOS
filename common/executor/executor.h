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
#include <photon/photon.h>
#include <photon/thread/awaiter.h>
#include <photon/thread/thread.h>

namespace photon {
class Executor {
public:
    class ExecutorImpl;

    ExecutorImpl *e;
    Executor(int init_ev = photon::INIT_EVENT_DEFAULT,
             int init_io = photon::INIT_IO_DEFAULT);
    ~Executor();

    template <
        typename Context = AutoContext, typename Func,
        typename R = typename std::result_of<Func()>::type,
        typename _ = typename std::enable_if<!std::is_void<R>::value, R>::type>
    R perform(Func &&act) {
        R result;
        int err;
        Awaiter<Context> aop;
        auto task = [&] {
            result = act();
            err = errno;
            aop.resume();
        };
        _issue(e, task);
        aop.suspend();
        errno = err;
        return result;
    }

    template <
        typename Context = AutoContext, typename Func,
        typename R = typename std::result_of<Func()>::type,
        typename _ = typename std::enable_if<std::is_void<R>::value, R>::type>
    void perform(Func &&act) {
        Awaiter<Context> aop;
        int err;
        auto task = [&] {
            act();
            err = errno;
            aop.resume();
        };
        _issue(e, task);
        aop.suspend();
        errno = err;
    }

    // `task` accept on heap lambda or functor pointer
    // Usually could able to call as
    // `e.async_perform(new auto ([]{ ... })`
    // to create a new lambda object on heap without move from stack.
    // The task object will be delete after work done
    template <typename Context = AutoContext, typename Func>
    void async_perform(Func *task) {
        void (*func)(void *);
        func = [](void *task_) {
            using Task = decltype(task);
            auto t = (Task)task_;
            (*t)();
            delete t;
        };
        _issue(e, {func, task});
    }

    static Executor *export_as_executor();

protected:
    static constexpr int64_t kCondWaitMaxTime = 100L * 1000;

    struct create_on_current_vcpu {};

    Executor(create_on_current_vcpu);

    static void _issue(ExecutorImpl *e, Delegate<void> cb);
};

}  // namespace photon
