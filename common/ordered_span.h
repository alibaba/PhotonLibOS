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

// must be initialized with an initializer_list of ORDERED string_views
template<typename T, typename Less = std::less<T>>
class ordered_span : public ::tcb::span<T> {
public:
    using base = ::tcb::span<T>;
    using base::base;
    using base::operator=;

    constexpr ordered_span(std::initializer_list<T> l)
        : base(l) { assert(ordered()); }

    template<size_t N>
    constexpr ordered_span(const T (&arr)[N])
        : base((T*)arr, N) { assert(ordered()); }

    bool ordered() const {
        if (this->empty())
            return true;
        Less less;
        for (uint32_t i = 0; i < this->size() - 1; ++i)
            if (!less((*this)[i], (*this)[i + 1]))
                return false;
        return true;
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
    using ordered_span::ordered_span;
    using ordered_span::operator[];
    using ordered_span::operator=;

    std::string_view operator[](std::string_view s) const {
        auto it = this->find(s);
        return (it == end()) ? std::string_view() : it->second;
    }
};

#define DEFINE_CONST_STATIC_ORDERED_STRINGS(name, ...)    \
    const static std::string_view _CONCAT(__a__,__LINE__)[] = __VA_ARGS__;  \
    const static ordered_strings name(_CONCAT(__a__,__LINE__));

#define DEFINE_CONST_STATIC_ORDERED_STRING_KV(name, ...)  \
    const static SKV _CONCAT(__a__,__LINE__)[] = __VA_ARGS__;   \
    const static ordered_string_kv name(_CONCAT(__a__,__LINE__));

}
