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

#include <cstdint>
#include <type_traits>

#include "string_view.h"

namespace ConstString {

template <typename T, T... xs>
struct TList {};
template <size_t... indices>
struct index_sequence {
    template <size_t Z>
    using append = index_sequence<indices..., sizeof...(indices) + Z>;
};

template <size_t N, size_t Z>
struct make_index_sequence {
    using result =
        typename make_index_sequence<N - 1, Z>::result::template append<Z>;
};

template <size_t Z>
struct make_index_sequence<0, Z> {
    using result = index_sequence<>;
};

struct tstring_base {};

template <typename... Tss>
struct TStrArray;

template <char SP, char... chs>
struct TSCut;

template <char SP, char... chs>
struct TStrip;

template <char SP, char IGN, typename TStr>
struct TSpliter;

template <char... str>
struct TString : tstring_base {
    static constexpr const char chars[sizeof...(str) + 1] = {str..., '\0'};
    static constexpr const size_t len = sizeof...(str);

    using type = TString<str...>;

    template <char ch>
    using Prepend = TString<ch, str...>;
    template <char ch>
    using Append = TString<str..., ch>;

    template <char ch>
    static constexpr decltype(auto) prepend() {
        return Prepend<ch>{};
    }
    template <char ch>
    static constexpr decltype(auto) append() {
        return Append<ch>{};
    }
    template <char... rhs>
    static constexpr decltype(auto) concat(TString<rhs...>) {
        return TString<str..., rhs...>{};
    }
    template <typename T, typename = typename std::enable_if<
                              std::is_base_of<tstring_base, T>::value>::type>
    decltype(auto) operator+(T rhs) {
        return concat(*this, rhs);
    }
    template <char SP, char IGN>
    static constexpr decltype(auto) split() {
        return TSpliter<SP, IGN, TString>::array();
    }
    static constexpr decltype(auto) tsreverse(TString<>) { return TString<>(); }
    template <char ch, char... chs>
    static constexpr decltype(auto) tsreverse(TString<ch, chs...>) {
        return typename decltype(tsreverse(
            TString<chs...>()))::template Append<ch>();
    }
    static constexpr decltype(auto) reverse() { return tsreverse(TString()); }
    template <char SP>
    static constexpr decltype(auto) cut() {
        return TSCut<SP, str...>();
    }
    template <char SP>
    static constexpr decltype(auto) lstrip() {
        return typename TStrip<SP, str...>::type();
    }
    template <char SP>
    static constexpr decltype(auto) rstrip() {
        return reverse().template lstrip<SP>().reverse();
    }
    template <char SP>
    static constexpr decltype(auto) strip() {
        return lstrip<SP>().template rstrip<SP>();
    }
    template <size_t... indices>
    static constexpr decltype(auto) _do_substr(index_sequence<indices...>) {
        return TString<chars[indices]...>();
    }
    template <size_t offset, size_t len>
    static constexpr decltype(auto) substr() {
        return _do_substr<decltype(make_index_sequence<len, offset>())>();
    }

    static constexpr std::string_view view() { return {chars, len}; }
};

template <char... str>
constexpr const char TString<str...>::chars[sizeof...(str) + 1];

template <char... str>
constexpr const size_t TString<str...>::len;

template <typename str, size_t... indices>
constexpr decltype(auto) build_tstring(index_sequence<indices...>) {
    return TString<str().chars[indices]...>();
}

#define TSTRING(x)                                                   \
    [] {                                                             \
        struct const_str {                                           \
            const char* chars = x;                                   \
        };                                                           \
        return ConstString::build_tstring<const_str>(                \
            typename ConstString::make_index_sequence<sizeof(x) - 1, \
                                                      0>::result{}); \
    }()

template <typename T, T... xs>
struct Accumulate {
    template <T x>
    using Prepend = Accumulate<T, 0, (xs + x)...>;

    constexpr static T arr[sizeof...(xs)] = {xs...};
};

template <typename T, T... xs>
constexpr T Accumulate<T, xs...>::arr[];

template <typename T, T x, T... xs>
constexpr decltype(auto) accumulate_helper(TList<T, x, xs...>) {
    return typename decltype(accumulate_helper(
        TList<T, xs...>{}))::template Prepend<x>{};
}

template <typename T>
constexpr Accumulate<T, 0> accumulate_helper(TList<T>) {
    return {};
}

template <typename... Tss>
struct TStrArray {
    template <typename T>
    using Prepend = TStrArray<T, Tss...>;

    template <typename T>
    using Append = TStrArray<Tss..., T>;

    template <typename T>
    constexpr static decltype(auto) prepend() {
        return Prepend<T>();
    }
    template <typename T>
    constexpr static decltype(auto) append() {
        return Append<T>();
    }
    constexpr static decltype(auto) whole() { return join<'\0'>(); }
    constexpr static size_t size = sizeof...(Tss);
    constexpr static std::string_view views[sizeof...(Tss)] = {Tss::view()...};
    constexpr static auto offset =
        decltype(accumulate_helper(TList<int16_t, (Tss::len + 1)...>{})){};

    template <char sp>
    constexpr static decltype(auto) _join_tstring() {
        return TStrArray<>();
    }
    template <char sp, typename T>
    constexpr static decltype(auto) _join_tstring(T t) {
        return t;
    }
    template <char sp, typename LS, typename RS, typename... Args>
    constexpr static decltype(auto) _join_tstring(LS ls, RS rs, Args... args) {
        using with_tail = typename LS::template Append<sp>;
        return _join_tstring<sp>(with_tail{}.concat(rs), args...);
    }
    template <char SP>
    constexpr static decltype(auto) join() {
        return _join_tstring<SP>(Tss()...);
    }
};

template <typename... Tss>
constexpr std::string_view TStrArray<Tss...>::views[];

template <typename... Tss>
constexpr size_t TStrArray<Tss...>::size;

template<typename... Tss>
constexpr static decltype(auto) make_tstring_array(Tss...) {
    return TStrArray<Tss...>{};
}

template <char SP, char IGN, typename TStr>
struct TSpliter {
    using Cut = decltype(TStr::template cut<SP>());
    using Current = decltype(Cut::Head::template strip<IGN>());
    using Next = TSpliter<SP, IGN, typename Cut::Tail>;
    using Array = typename Next::Array::template Prepend<Current>;
    static constexpr Current current() { return {}; };
    static constexpr Next next() { return {}; };
    static constexpr Array array() { return {}; };
};

template <char SP, char IGN>
struct TSpliter<SP, IGN, TString<>> {
    using Cut = TSCut<SP>;
    using Current = TString<>;
    using Next = TSpliter;
    using Array = TStrArray<>;
    static constexpr Current current() { return {}; };
    static constexpr Next next() { return {}; };
    static constexpr Array array() { return {}; };
};

template <char SP, char ch, char... chs>
struct TSCut<SP, ch, chs...> {
    using Head = typename std::conditional<
        SP == ch, TString<>,
        typename TSCut<SP, chs...>::Head::template Prepend<ch>>::type;
    using Tail =
        typename std::conditional<SP == ch, TString<chs...>,
                                  typename TSCut<SP, chs...>::Tail>::type;
    static constexpr Head head{};
    static constexpr Tail tail{};
};

template <char SP>
struct TSCut<SP> {
    using Head = TString<>;
    using Tail = TString<>;
    static constexpr Head head{};
    static constexpr Tail tail{};
};

template <char SP, char ch, char... chs>
struct TStrip<SP, ch, chs...> {
    using type =
        typename std::conditional<SP == ch, typename TStrip<SP, chs...>::type,
                                  TString<ch, chs...>>::type;
};
template <char SP>
struct TStrip<SP> {
    using type = TString<>;
};

template <typename EnumType, typename Split>
struct EnumStr : public Split {
    using Enum = EnumType;
    std::string_view operator[](EnumType x) const {
        return {base() + off((int)x),
                (size_t)(off((int)x + 1) - off((int)x)) - 1};
    }

    constexpr static decltype(auto) whole() { return Split::Array::whole(); }

    constexpr static decltype(auto) arr() { return typename Split::Array{}; }

    static int16_t off(int i) { return Split::Array::offset.arr[i]; }

    constexpr static const char* base() { return Split::Array::whole().chars; }

    constexpr static size_t size() { return Split::Array::size; }
};

#define DEFINE_ENUM_STR(enum_name, str_name, ...)                         \
    enum class enum_name : char { __VA_ARGS__ };                          \
    static const auto str_name = [] {                                     \
        auto x = TSTRING(#__VA_ARGS__);                                   \
        using sp = typename ConstString::TSpliter<',', ' ', decltype(x)>; \
        return ConstString::EnumStr<enum_name, sp>();                     \
    }()

}  // namespace ConstString
