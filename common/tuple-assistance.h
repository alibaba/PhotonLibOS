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

#include <functional>
#include <tuple>
#include <utility>

namespace tuple_assistance {

#if __cplusplus < 201703L

namespace detail {

// See https://en.cppreference.com/w/cpp/utility/functional/invoke
// See https://en.cppreference.com/w/cpp/types/result_of

template <typename...> using void_t = void;

template <class>
struct is_reference_wrapper : std::false_type {};

template <class U>
struct is_reference_wrapper<std::reference_wrapper<U>> : std::true_type {};

template <class T>
struct invoke_impl {
    template <class F, class... Args>
    static auto call(F&& f, Args&&... args)
        -> decltype(std::forward<F>(f)(std::forward<Args>(args)...));
};

template <class B, class MT>
struct invoke_impl<MT B::*> {
    template <
        class T, class Td = typename std::decay<T>::type,
        class = typename std::enable_if<std::is_base_of<B, Td>::value>::type>
    static auto get(T&& t) -> T&&;

    template <
        class T, class Td = typename std::decay<T>::type,
        class = typename std::enable_if<is_reference_wrapper<Td>::value>::type>
    static auto get(T&& t) -> decltype(t.get());

    template <
        class T, class Td = typename std::decay<T>::type,
        class = typename std::enable_if<!std::is_base_of<B, Td>::value>::type,
        class = typename std::enable_if<!is_reference_wrapper<Td>::value>::type>
    static auto get(T&& t) -> decltype(*std::forward<T>(t));

    template <
        class T, class... Args, class MT1,
        class = typename std::enable_if<std::is_function<MT1>::value>::type>
    static auto call(MT1 B::*pmf, T&& t, Args&&... args)
        -> decltype((invoke_impl::get(std::forward<T>(t)).*
                     pmf)(std::forward<Args>(args)...));

    template <class T>
    static auto call(MT B::*pmd, T&& t)
        -> decltype(invoke_impl::get(std::forward<T>(t)).*pmd);
};

template <class F, class... Args, class Fd = typename std::decay<F>::type>
auto INVOKE(F&& f, Args&&... args)
    -> decltype(invoke_impl<Fd>::call(std::forward<F>(f),
                                      std::forward<Args>(args)...));

template <typename AlwaysVoid, typename, typename...>
struct invoke_result {};

template <typename F, typename... Args>
struct invoke_result<decltype(void(
                         INVOKE(std::declval<F>(), std::declval<Args>()...))),
                     F, Args...> {
    using type = decltype(INVOKE(std::declval<F>(), std::declval<Args>()...));
};

template <typename Result, typename Ret, bool = std::is_void<Ret>::value,
          typename = void>
struct is_invocable_impl : std::false_type {};

// Used for valid INVOKE and INVOKE<void> expressions.
template <typename Result, typename Ret>
struct is_invocable_impl<Result, Ret,
                         /* is_void<_Ret> = */ true,
                         void_t<typename Result::type>> : std::true_type {
};

template <class C, class Pointed, class T1, class... Args,
          std::enable_if_t<std::is_function<Pointed>::value &&
                               std::is_base_of<C, std::decay_t<T1>>::value,
                           void*> = nullptr>
constexpr decltype(auto) invoke_memptr(Pointed C::*f, T1&& t1, Args&&... args) {
    return (std::forward<T1>(t1).*f)(std::forward<Args>(args)...);
}

template <class C, class Pointed, class T1, class... Args,
          std::enable_if_t<std::is_function<Pointed>::value &&
                               is_reference_wrapper<std::decay_t<T1>>::value,
                           void*> = nullptr>
constexpr decltype(auto) invoke_memptr(Pointed C::*f, T1&& t1, Args&&... args) {
    return (t1.get().*f)(std::forward<Args>(args)...);
}

template <class C, class Pointed, class T1, class... Args,
          std::enable_if_t<std::is_function<Pointed>::value &&
                               !std::is_base_of<C, std::decay_t<T1>>::value &&
                               !is_reference_wrapper<std::decay_t<T1>>::value,
                           void*> = nullptr>
constexpr decltype(auto) invoke_memptr(Pointed C::*f, T1&& t1, Args&&... args) {
    return ((*std::forward<T1>(t1)).*f)(std::forward<Args>(args)...);
}

}  // namespace detail

// std::invoke_result for C++14
template <class F, class... ArgTypes>
struct invoke_result : detail::invoke_result<void, F, ArgTypes...> {};

template <class F, class... Args>
using invoke_result_t = typename invoke_result<F, Args...>::type;

// std::is_invocable for C++14
template <typename F, typename... Args>
struct is_invocable
    : detail::is_invocable_impl<invoke_result<F, Args...>, void>::type {};


// std::invoke for C++14
template <class F, class... Args,
          std::enable_if_t<std::is_member_pointer<std::decay_t<F>>::value,
                           void*> = nullptr>
constexpr invoke_result_t<F, Args...> invoke(F&& f, Args&&... args) {
    return detail::invoke_memptr(f, std::forward<Args>(args)...);
}

template <class F, class... Args,
          std::enable_if_t<!std::is_member_pointer<std::decay_t<F>>::value,
                           void*> = nullptr>
constexpr invoke_result_t<F, Args...> invoke(F&& f, Args&&... args) {
    return std::forward<F>(f)(std::forward<Args>(args)...);
}


template <typename F, typename Tuple, std::size_t... I>
constexpr decltype(auto) apply_impl(F&& f, Tuple&& t,
                                           std::index_sequence<I...>) {
    return invoke(std::forward<F>(f), 
                        std::get<I>(std::forward<Tuple>(t))...);
}

// Implementation of a simplified std::apply from C++17
template <typename F, typename Tuple>
constexpr decltype(auto) apply(F&& f, Tuple&& t) {
    return apply_impl(
        std::forward<F>(f), std::forward<Tuple>(t),
        std::make_index_sequence<
            std::tuple_size<std::remove_reference_t<Tuple>>::value>{});
}

#else

using std::invoke;
using std::apply;

template <typename F, typename... Args>
using invoke_result = std::invoke_result<F, Args...>;

template <typename F, typename... Args>
using invoke_result_t = std::invoke_result_t<F, Args...>;

template <typename F, typename... Args>
using is_invocable = std::is_invocable<F, Args...>;

#endif

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
