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
#include <memory>
#include <tuple>
#include <utility>
#include <functional>
#include <photon/thread/thread.h>
#include <photon/common/PMF.h>
#include <photon/common/utility.h>
#include <photon/common/tuple-assistance.h>
#include <photon/common/callback.h>

namespace photon {

template<class F, class... Args, size_t I0, size_t... I>
void __thread_execute(std::tuple<F, Args...>& t, std::index_sequence<I0, I...>) {
    std::__invoke(std::move(std::get<I0>(t)), std::move(std::get<I>(t))...);
}

template <class Tuple>
void* __thread_proxy(void* vp) {
    // Tuple = std::tuple<Func, Args...>
    std::unique_ptr<Tuple> p(static_cast<Tuple*>(vp));

    __thread_execute(*p, std::make_index_sequence<std::tuple_size<Tuple>::value>{});
    return nullptr;
}

// NOTICE: The arguments to the thread function are moved or copied by
// value. If a reference argument needs to be passed to the thread function,
// it has to be wrapped (e.g., with std::ref or std::cref).
template <class F, class... Args, typename = std::enable_if_t<std::__is_invocable<F&&, Args&&...>::value>>
thread* thread_create11(uint64_t stack_size, F&& f, Args&&... args) {
    using Gp = std::tuple<typename std::decay<F>::type, typename std::decay<Args>::type...>;
    std::unique_ptr<Gp> p(new Gp(std::forward<F>(f), std::forward<Args>(args)...));
    auto thread = thread_create(&__thread_proxy<Gp>, p.get(), stack_size);
    if (thread) {
        _unused(p.release());
    }
    return thread;
}

// NOTICE: The arguments to the thread function are moved or copied by
// value. If a reference argument needs to be passed to the thread function,
// it has to be wrapped (e.g., with std::ref or std::cref).
template <class F, class... Args, typename = std::enable_if_t<std::__is_invocable<F&&, Args&&...>::value>>
thread* thread_create11(F&& f, Args&&... args) {
    return thread_create11(DEFAULT_STACK_SIZE, std::forward<F>(f), std::forward<Args>(args)...);
}

template<typename Callable>
inline int thread_usleep_defer(uint64_t timeout, Callable&& callback) {
    Delegate<void> delegate(callback);
    return thread_usleep_defer(timeout, delegate._func, delegate._obj);
}

    // =============================================================================
    class __Example_of_Thread11__
    {
    public:
        void* member_function(int, char) { return nullptr; }
        int const_member_function(int, char, double) const { return -1; }
        static long any_return_type(int, char) { return 0; }

        void asdf()
        {
            int a; char b;
            auto func = &__Example_of_Thread11__::member_function;
            auto cfunc = &__Example_of_Thread11__::const_member_function;

            // thread { return this->func(a, b); }
            thread_create11(func, this, a, b);

            // thread { return this->cfunc(a, b, 3.14); }
            thread_create11(cfunc, this, a, b, 3.14);

            // thread(stack_size = 64KB) { return this->func(1, 'a'); }
            thread_create11(/* stack size */ 64*1024, func, this, 1, 'a');

            // thread { any_return_type(a, b); return nullptr; }
            thread_create11(&any_return_type, a, b);

            // thread(stack_size = 64KB) { any_return_type(1, 'a'); return nullptr; }
            thread_create11(/* stack size */ 64*1024, &any_return_type, 1, 'a');

            thread_yield();
        }
    };
}
