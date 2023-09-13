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
#include <tuple>
#include <utility>
#include <functional>
#include <photon/thread/thread.h>
#include <photon/common/PMF.h>
#include <photon/common/utility.h>
#include <photon/common/tuple-assistance.h>
#include <photon/common/callback.h>

namespace photon {
    template<typename Pair>
    static void* __stub11(void*) {
        auto p = thread_reserved_space<Pair>(CURRENT);
        tuple_assistance::apply(std::move(p->first), std::move(p->second));
        return nullptr;
    }

    // The arguments are forwarded to the new thread by value.
    // If an argument does need to be passed by reference,
    // it has to be wrapped by std::ref or std::cref.
    template<typename F, typename SavedArgs, typename...ARGUMENTS> inline
    thread* __thread_create11(uint64_t stack_size, F&& f, ARGUMENTS&&...args) {
        using Pair = std::pair<F, SavedArgs>;
        static_assert(sizeof(Pair) < UINT16_MAX, "...");
        auto th = thread_create(&__stub11<Pair>, nullptr, stack_size, sizeof(Pair));
        auto p  = thread_reserved_space<Pair>(th);
        new (p) Pair{std::forward<F>(f), SavedArgs{std::forward<ARGUMENTS>(args)...}};
        return th;
    }

    template<typename FUNCTOR, typename...ARGUMENTS>
    struct FunctorWrapper {
        typename std::decay<FUNCTOR>::type _obj;
        // __attribute__((always_inline))
        int operator()(ARGUMENTS&...args) {
            _obj(std::move(args)...);
            return 0;
        }
    };

    #define _ENABLE_IF(cond) typename std::enable_if<cond, thread*>::type

    template<typename F, typename...ARGUMENTS>
    inline _ENABLE_IF(is_function_pointer<F>::value)
    thread_create11(uint64_t stack_size, F f, ARGUMENTS&&...args) {
        using SavedArgs = typename tuple_assistance::callable<F>::arguments;
        return __thread_create11<F, SavedArgs, ARGUMENTS...>(stack_size,
            std::forward<F>(f), std::forward<ARGUMENTS>(args)...);
    }

    template<typename F, typename...ARGUMENTS>
    inline _ENABLE_IF(is_function_pointer<F>::value)
    thread_create11(F f, ARGUMENTS&&...args) {
        return thread_create11<F, ARGUMENTS...>(DEFAULT_STACK_SIZE,
            std::forward<F>(f), std::forward<ARGUMENTS>(args)...);
    }

    template<typename CLASS, typename F, typename...ARGUMENTS>
    inline _ENABLE_IF(std::is_member_function_pointer<F>::value)
    thread_create11(uint64_t stack_size, F f, CLASS* obj, ARGUMENTS&&...args) {
        auto pmf = ::get_member_function_address(obj, f);
        using FF = decltype(pmf.f);
        using Obj = decltype(pmf.obj);
        return thread_create11<FF, Obj, ARGUMENTS...>(stack_size,
            std::forward<FF>(pmf.f), std::forward<Obj>(pmf.obj),
            std::forward<ARGUMENTS>(args)...);
    }

    template<typename CLASS, typename F, typename...ARGUMENTS>
    inline _ENABLE_IF(std::is_member_function_pointer<F>::value)
    thread_create11(F f, CLASS* obj, ARGUMENTS&&...args) {
        return thread_create11<CLASS, F, ARGUMENTS...>(
            DEFAULT_STACK_SIZE, f, obj, std::forward<ARGUMENTS>(args)...);
    }

    template<typename T, typename...ARGUMENTS>
    using is_callable = std::is_constructible< std::function<void(ARGUMENTS...)>,
        typename std::reference_wrapper<typename std::remove_reference<T>::type>::type>;

    template<typename T, typename...ARGUMENTS>
    struct is_functor : std::conditional<
        std::is_class<typename std::decay<T>::type>::value &&
        is_callable<T, ARGUMENTS...>::value,
            std::true_type, std::false_type>::type { };

    template<typename FUNCTOR, typename...ARGUMENTS>
    inline _ENABLE_IF((is_functor<FUNCTOR, ARGUMENTS...>::value))
    thread_create11(uint64_t stack_size, FUNCTOR&& f, ARGUMENTS&&...args) {
        using Wrapper = FunctorWrapper<FUNCTOR, ARGUMENTS...>;
        using SavedArgs = std::tuple<typename std::decay<ARGUMENTS>::type ...>;
        return __thread_create11<Wrapper, SavedArgs, ARGUMENTS...>(
            stack_size, Wrapper{std::forward<FUNCTOR>(f)},
            std::forward<ARGUMENTS>(args)...);
    }

    template<typename FUNCTOR, typename...ARGUMENTS>
    inline _ENABLE_IF((is_functor<FUNCTOR, ARGUMENTS...>::value))
    thread_create11(FUNCTOR&& f, ARGUMENTS&&...args) {
        return thread_create11<FUNCTOR, ARGUMENTS...>(DEFAULT_STACK_SIZE,
            std::forward<FUNCTOR>(f), std::forward<ARGUMENTS>(args)...);
    }

    template<typename Callable> inline
    int thread_usleep_defer(uint64_t timeout, Callable&& callback) {
        Delegate<void> delegate(callback);
        return thread_usleep_defer(timeout, delegate._func, delegate._obj);
    }

    #undef _ENABLE_IF

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

            thread_create11([&](int, double) { return -1; }, 10, 3.1415926);

            thread_yield();
        }
    };
}
