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

namespace tuple_assistance {

template <class F>
struct callable;

// function pointer
template <class R, class... Args>
struct callable<R (*)(Args...)> : public callable<R(Args...)> {};

template <class R, class... Args>
struct callable<R(Args...)> {
    using return_type = R;

    using arguments = std::tuple<Args...>;
};

// member function pointer
template <class C, class R, class... Args>
struct callable<R (C::*)(Args...)> : public callable<R(C&, Args...)> {};

// const member function pointer
template <class C, class R, class... Args>
struct callable<R (C::*)(Args...) const> : public callable<R(C&, Args...)> {};

// member object pointer
template <class C, class R>
struct callable<R(C::*)> : public callable<R(C&)> {};

template <typename T>
struct __remove_first_type_in_tuple {};

template <typename T, typename... Ts>
struct __remove_first_type_in_tuple<std::tuple<T, Ts...>> {
    typedef std::tuple<Ts...> type;
};

// functor
template <class F>
struct callable {
    using call_type = callable<decltype(&F::operator())>;

    using return_type = typename call_type::return_type;

    using arguments = typename __remove_first_type_in_tuple<
        typename call_type::arguments>::type;
};

template <class F>
struct callable<F&> : public callable<F> {};

template <class F>
struct callable<F&&> : public callable<F> {};

// #if __cplusplus < 201700
template <typename F, typename Tuple, std::size_t... I>
constexpr inline decltype(auto) apply_impl(F&& f, Tuple&& t,
                                           std::index_sequence<I...>) {
    using Args = typename callable<F>::arguments;
    return f(std::forward<typename std::tuple_element<I, Args>::type>(
             std::get<I>(t))...);
}

// Implementation of a simplified std::apply from C++17
template <typename F, typename Tuple>
constexpr inline decltype(auto) apply(F&& f, Tuple&& t) {
    return apply_impl(
        std::forward<F>(f), std::forward<Tuple>(t),
        std::make_index_sequence<
            std::tuple_size<std::remove_reference_t<Tuple>>::value>{});
}

// #else
// using std::apply;
// #endif

template <typename P, size_t I, typename... Ts>
struct do_enumerate;

template <typename P, size_t I, typename... Ts>
struct do_enumerate<P, I, std::tuple<Ts...>> {
    static_assert(0 < I && I < sizeof...(Ts), "");
    static void proc(const P& p, std::tuple<Ts...>& t) {
        do_enumerate<P, I - 1, std::tuple<Ts...>>::proc(p, t);
        p.proc(std::get<I>(t));
    }
};

template <typename P, typename... Ts>
struct do_enumerate<P, 0, std::tuple<Ts...>> {
    static void proc(const P& p, std::tuple<Ts...>& t) {
        p.proc(std::get<0>(t));
    }
};

template <typename P, typename... Ts>
static void enumerate(const P& p, std::tuple<Ts...>& t) {
    do_enumerate<P, sizeof...(Ts) - 1, std::tuple<Ts...>>::proc(p, t);
}
}  // namespace tuple_assistance
