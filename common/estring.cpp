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

#include "estring.h"
#include <algorithm>

size_t estring_view::find_first_of(const charset& set) const
{
    auto it = begin();
    for (;it != end(); ++it)
        if (set.test(*it))
            return it - begin();
    return npos;
}

size_t estring_view::find_first_not_of(const charset& set) const
{
    auto it = begin();
    for (;it != end(); ++it)
        if (!set.test(*it))
            return it - begin();
    return npos;
}

size_t estring_view::find_last_of(const charset& set) const
{
    auto it = rbegin();
    for (;it != rend(); ++it)
        if (set.test(*it))
            return &*it - &*begin();
    return npos;
}

size_t estring_view::find_last_not_of(const charset& set) const
{
    auto it = rbegin();
    for (;it != rend(); ++it)
        if (!set.test(*it))
            return &*it - &*begin();
    return npos;
}

bool estring_view::to_uint64_check(uint64_t* v) const
{
    v ? (*v = 0) : 0;
    for (unsigned char c : *this) {
        if (c > '9' || c < '0')
            return false;
        v ? (*v = *v * 10 + (c - '0')) : 0;
    }
    return true;
}

uint64_t estring_view::hex_to_uint64() const
{
    uint64_t ret = 0;
    for (unsigned char c : *this) {
        if (c >= '0' && c <= '9') {
            ret = ret * 16 + (c - '0');
        } else if (c >= 'A' && c <= 'F') {
            ret = ret * 16 + (c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            ret = ret * 16 + (c - 'a' + 10);
        } else {
            return ret;
        }
    }
    return ret;
}

estring& estring::append(uint64_t x)
{
    auto begin = size();
    do
    {
        *this += '0' + x % 10;
        x /= 10;
    } while(x);
    auto end = size();
    auto ptr = &(*this)[0];
    std::reverse(ptr + begin, ptr + end);
    return *this;
}

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>().append(0ULL))>::value,
    "estring append uint64 should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>().append('0'))>::value,
    "estring append char should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>().append("Hello"))>::value,
    "estring append char* should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>().append(std::declval<char*>(), 5))>::value,
    "estring append char* and size should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>().append(std::declval<std::string>()))>::value,
    "estring append std::string should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>().append(std::declval<std::string_view>()))>::value,
    "estring append std::string_view should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>() += 0ULL)>::value,
    "estring += uint64 should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>() += '0')>::value,
    "estring += char should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>() += "Hello")>::value,
    "estring += char* should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>() += std::declval<std::string>())>::value,
    "estring += std::string should return estring"
);

static_assert(
    std::is_same<estring&, decltype(std::declval<estring>() += std::declval<std::string_view>())>::value,
    "estring += std::string_view should return estring"
);