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
#include <photon/common/executor/stdlock.h>
#include <photon/photon.h>

#include <atomic>
#include <type_traits>

namespace photon {

class ExecutorImpl;

class Executor {
public:
    ExecutorImpl *e;
    Executor(int init_ev = photon::INIT_EVENT_DEFAULT,
             int init_io = photon::INIT_IO_DEFAULT);
    ~Executor();

    template <
        typename Context = StdContext, typename Func,
        typename R = typename std::result_of<Func()>::type,
        typename _ = typename std::enable_if<!std::is_void<R>::value, R>::type>
    R perform(Func &&act) {
        R result;
        AsyncOp<Context> aop;
        aop.call(e, [&] {
            result = act();
            aop.done();
        });
        return result;
    }

    template <
        typename Context = StdContext, typename Func,
        typename R = typename std::result_of<Func()>::type,
        typename _ = typename std::enable_if<std::is_void<R>::value, R>::type>
    void perform(Func &&act) {
        AsyncOp<Context> aop;
        aop.call(e, [&] {
            act();
            aop.done();
        });
    }

    // `task` accept on heap lambda or functor pointer
    // Usually could able to call as
    // `e.async_perform(new auto ([]{ ... })`
    // to create a new lambda object on heap without move from stack.
    // The task object will be delete after work done
    template <typename Context = StdContext, typename Func>
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

    static Executor* export_as_executor();

protected:
    static constexpr int64_t kCondWaitMaxTime = 100L * 1000;

    struct create_on_current_vcpu {};

    Executor(create_on_current_vcpu);

    template <typename Context>
    struct AsyncOp {
        int err;
        std::atomic_bool gotit;
        typename Context::Mutex mtx;
        typename Context::Cond cond;
        AsyncOp() : gotit(false), cond(mtx) {}
        void wait_for_completion() {
            typename Context::CondLock lock(mtx);
            while (!gotit.load(std::memory_order_acquire)) {
                cond.wait_for(lock, kCondWaitMaxTime);
            }
            if (err) errno = err;
        }
        void done(int error_number = 0) {
            typename Context::CondLock lock(mtx);
            err = error_number;
            gotit.store(true, std::memory_order_release);
            cond.notify_all();
        }
        template <typename Func>
        void call(ExecutorImpl *e, Func &&act) {
            _issue(e, act);
            wait_for_completion();
        }
    };

    static void _issue(ExecutorImpl *e, Delegate<void> cb);
};

}  // namespace photon
