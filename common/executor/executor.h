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

#include <atomic>
#include <type_traits>

namespace photon {

class ExecutorImpl;

ExecutorImpl *_new_executor();
void _delete_executor(ExecutorImpl *e);
void _issue(ExecutorImpl *e, Delegate<void> cb);

class Executor {
public:
    ExecutorImpl *e = _new_executor();
    ~Executor() { _delete_executor(e); }

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

    template <typename Context = StdContext, typename Func>
    void async_perform(Func &&act) {
        AsyncOp<Context> aop;
        aop.call(e, [&] {
            // copy the work, so when context is dropped
            // still able to call
            auto copy_act = std::forward<Func>(act);
            aop.done();
            // till here, `act` may be destructed
            // call the copied `copy_act`
            copy_act();
        });
    }

protected:
    static constexpr int64_t kCondWaitMaxTime = 1000L * 1000;

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
};

}  // namespace photon
