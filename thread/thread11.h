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

namespace photon
{
    template<typename F>
    struct ThreadContext11__ : public tuple_assistance::callable<F>
    {
        using base = tuple_assistance::callable<F>;
        using typename base::return_type;
        using typename base::arguments;

        F start;
        uint64_t stack_size;
        arguments args;
        bool got_it = false;
        thread* parent;

        template<typename...ARGUMENTS>
        ThreadContext11__(uint64_t stack_size_, F f, ARGUMENTS&&...args_) :
            start(f), stack_size(stack_size_), args{std::forward<ARGUMENTS>(args_)...}
        {
            parent = CURRENT;
//            LOG_DEBUG("arguments stored in tuple");
        }

        static return_type stub11(void* args)
        {
            typedef ThreadContext11__ Context;
            auto ctx_ = (Context*)args;
            Context ctx = std::move(*ctx_);
//            LOG_DEBUG("arguments tuple moved");
            ctx_->got_it = true;
            thread_yield_to(ctx.parent);
            return tuple_assistance::apply(ctx.start, ctx.args);
        }
        thread* thread_create(thread_entry start_)
        {
            auto th = ::photon::thread_create(start_, this, stack_size);
            thread_yield_to(th);
            return th;
        }
    };

    template<typename F>
    struct ThreadContext11 : public ThreadContext11__<F>
    {
        typedef ThreadContext11__<F> base;
        using base::base;
        using base::stub11;
        static void* stub(void* args)
        {
            stub11(args);
            return nullptr;
        }
        thread* thread_create()
        {
            return base::thread_create(&stub);
        }
    };

    template<typename...PARAMETERS>
    struct ThreadContext11<void* (*)(PARAMETERS...)> :
        public ThreadContext11__<void* (*)(PARAMETERS...)>
    {
        typedef void* (*F)(PARAMETERS...);
        typedef ThreadContext11__<F> base;
        using base::base;
        using base::stub11;
        static void* stub(void* args)
        {
            return stub11(args);
        }
        thread* thread_create()
        {
            return base::thread_create(&stub);
        }
    };

    template <typename F, typename... ARGUMENTS>
    inline std::enable_if_t<is_function_pointer<F>::value, thread*>
    thread_create11(uint64_t stack_size, F f, ARGUMENTS&&... args) {
        return ThreadContext11<F>(stack_size, f,
                                  std::forward<ARGUMENTS>(args)...)
            .thread_create();
    }

    template <typename F, typename... ARGUMENTS>
    inline std::enable_if_t<is_function_pointer<F>::value, thread*>
    thread_create11(F f, ARGUMENTS&&... args) {
        return thread_create11<F, ARGUMENTS...>(
            DEFAULT_STACK_SIZE, f, std::forward<ARGUMENTS>(args)...);
    }

    template <typename CLASS, typename F, typename... ARGUMENTS>
    inline std::enable_if_t<std::is_member_function_pointer<F>::value, thread*>
    thread_create11(uint64_t stack_size, F f, CLASS* obj, ARGUMENTS&&... args) {
        auto pmf = ::get_member_function_address(obj, f);
        return thread_create11(stack_size, pmf.f, pmf.obj,
                               std::forward<ARGUMENTS>(args)...);
    }

    template <typename CLASS, typename F, typename... ARGUMENTS>
    inline std::enable_if_t<std::is_member_function_pointer<F>::value, thread*>
    thread_create11(F f, CLASS* obj, ARGUMENTS&&... args) {
        return thread_create11<CLASS, F, ARGUMENTS...>(
            DEFAULT_STACK_SIZE, f, obj, std::forward<ARGUMENTS>(args)...);
    }

    template <typename FUNCTOR, typename... ARGUMENTS>
    inline void __functor_call_helper(
        typename std::decay<FUNCTOR>::type f,
        typename std::decay<ARGUMENTS>::type... args) {
        f(std::forward<ARGUMENTS>(args)...);
    }

    // NOTICE: The arguments to the thread function are moved or copied by
    // value. If a reference argument needs to be passed to the thread function,
    // it has to be wrapped (e.g., with std::ref or std::cref).
    template <typename T, typename... Args>
    struct is_functor
        : std::conditional<
              std::is_class<typename std::decay<T>::type>::value &&
                  std::is_constructible<typename std::function<void(Args...)>,
                                        typename std::reference_wrapper<
                                            typename std::remove_reference<
                                                T>::type>::type>::value,
              std::true_type, std::false_type>::type {};

    template <typename FUNCTOR, typename... ARGUMENTS>
    inline typename std::enable_if<is_functor<FUNCTOR, ARGUMENTS...>::value,
                                   thread*>::type
    thread_create11(uint64_t stack_size, FUNCTOR&& f, ARGUMENTS&&... args) {
        // takes `f` as parameter to helper function
        // thread_create11 will make sure parameters copy is completed
        return thread_create11(
            stack_size, &__functor_call_helper<FUNCTOR, ARGUMENTS...>,
            std::forward<FUNCTOR>(f), std::forward<ARGUMENTS>(args)...);
    }

    // NOTICE: The arguments to the thread function are moved or copied by
    // value. If a reference argument needs to be passed to the thread function,
    // it has to be wrapped (e.g., with std::ref or std::cref).
    template <typename FUNCTOR, typename... ARGUMENTS>
    inline typename std::enable_if<is_functor<FUNCTOR, ARGUMENTS...>::value,
                                   thread*>::type
    thread_create11(FUNCTOR&& f, ARGUMENTS&&... args) {
        return thread_create11<FUNCTOR, ARGUMENTS...>(
            DEFAULT_STACK_SIZE, std::forward<FUNCTOR>(f),
            std::forward<ARGUMENTS>(args)...);
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
