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
#include <inttypes.h>
#include <assert.h>
#include <algorithm>
#include <initializer_list>
#include <photon/common/string_view.h>
#include <photon/common/span.h>
#include <photon/common/utility.h>

namespace photon {

// must be initialized with an T[] what is ORDERED
template<typename T, typename Less = std::less<T>>
class ordered_span : public ::tcb::span<T> {
    size_t m_free = 0;  // number of free slots at tail
public:
    using base = ::tcb::span<T>;
    using base::operator=;

    ordered_span() = default;

    template<size_t N>
    constexpr ordered_span(const T (&arr)[N], size_t free = 0)
        : ordered_span(arr, N, free) { }

    constexpr ordered_span(const T* arr, size_t n, size_t free = 0)
        : base((T*)arr, n - free), m_free(free) { assert(ordered(arr, n - free)); }

    constexpr ordered_span(const T* begin, const T* end, size_t free = 0)
        : ordered_span(begin, end - begin, free) { }

    constexpr bool ordered(const T* p, size_t n) const {
        return (n <= 1) || (!Less()(p[1], p[0]) && ordered(p + 1, n - 1));
    }

    int append(const T& val) {
        assert(m_free);
        if (!m_free) return -1; else m_free--;
        if (!this->empty()) assert(!Less()(val, this->back()));
        (base&)*this = base(this->data(), this->size() + 1);
        this->back() = val;
        return 0;
    }

    int insert(const T& val) {
        assert(m_free);
        if (!m_free) return -1; else m_free--;
        (base&)*this = base(this->data(), this->size() + 1);
        auto& arr = *this;
        size_t i = this->size() - 1;
        for (; i; --i) {
            if (!Less()(val, arr[i-1])) break;
            else arr[i] = std::move(arr[i - 1]);
        }
        arr[i] = val;
        return 0;
    }

    using typename base::iterator;
    template<typename P>
    const iterator find (const P& s) const {
        Less less;
        auto it = std::lower_bound(this->begin(), this->end(), s, less);
        return (it != this->end() && !less(s, *it)) ? it : this->end();
    }

    template<typename P>
    size_t count(const P& s) const {
        return find(s) != this->end();
    }
};

using ordered_strings = ordered_span<std::string_view>;

using SKV = std::pair<std::string_view, std::string_view>;

struct SKVLess {
    bool operator()(const SKV& a, const SKV& b) const {
        return a.first < b.first;
    }
    bool operator()(const SKV& a, std::string_view b) const {
        return a.first < b;
    }
    bool operator()(std::string_view a, const SKV& b) const {
        return a < b.first;
    }
};

class ordered_string_kv : public ordered_span<SKV, SKVLess> {
public:
    using base = ordered_span<SKV, SKVLess>;
    using base::base;
    using base::operator[];
    using base::operator=;

    // gcc seems to have a bug regard inherited constructor, so we have to
    template<size_t N>  // repeat the constructor definition here
    constexpr ordered_string_kv(const SKV (&arr)[N], size_t free = 0)
        : base(arr, N, free) { }

    std::string_view operator[](std::string_view s) const {
        auto it = this->find(s);
        return (it == end()) ? std::string_view() : it->second;
    }
};

#define DEFINE_ORDERED_STRINGS(name, ...)    \
    std::string_view _CONCAT(__a__,__LINE__)[] = __VA_ARGS__;  \
    ordered_strings name(_CONCAT(__a__,__LINE__));

#define DEFINE_APPENDABLE_ORDERED_STRINGS(name, free, ...)    \
    std::string_view _CONCAT(_a_,__LINE__)[] = __VA_ARGS__;  \
    std::string_view _CONCAT(__a__,__LINE__)[LEN(_CONCAT(_a_,__LINE__)) + free] = __VA_ARGS__;  \
    ordered_strings name(_CONCAT(__a__,__LINE__), free);

#define DEFINE_CONST_STATIC_ORDERED_STRINGS(name, ...)    \
    const static std::string_view _CONCAT(__a__,__LINE__)[] = __VA_ARGS__;  \
    const static ordered_strings name(_CONCAT(__a__,__LINE__));

#define DEFINE_ORDERED_STRING_KV(name, ...)  \
    SKV _CONCAT(__a__,__LINE__)[] = __VA_ARGS__;   \
    ordered_string_kv name(_CONCAT(__a__,__LINE__));

#define DEFINE_APPENDABLE_ORDERED_STRING_KV(name, free, ...)    \
    SKV _CONCAT(_a_,__LINE__)[] = __VA_ARGS__;  \
    SKV _CONCAT(__a__,__LINE__)[LEN(_CONCAT(_a_,__LINE__)) + free] = __VA_ARGS__;  \
    ordered_string_kv name(_CONCAT(__a__,__LINE__), free);

#define DEFINE_CONST_STATIC_ORDERED_STRING_KV(name, ...)  \
    const static SKV _CONCAT(__a__,__LINE__)[] = __VA_ARGS__;   \
    const static ordered_string_kv name(_CONCAT(__a__,__LINE__));

}
