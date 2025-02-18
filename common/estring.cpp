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
    if (this->empty()) return false;
    uint64_t val = (*this)[0] - '0';
    if (val > 9) return false;
    for (unsigned char c : this->substr(1)) {
        c -= '0';
        if (c > 9) break;
        val = val * 10 + c;
    }
    if (v) *v = val;
    return true;
}

inline char hex_char_to_digit(char c) {
    unsigned char cc = c - '0';
    if (cc < 10) return cc;
    const unsigned char mask = 'a' - 'A';
    static_assert(mask == 32, "..."); // single digit
    c |= mask; // unified to 'a'..'f'
    cc = c - 'a';
    return (cc < 6) ? (cc + 10) : -1;
}

bool estring_view::hex_to_uint64_check(uint64_t* v) const {
    if (this->empty()) return false;
    uint64_t val = hex_char_to_digit((*this)[0]);
    if (val == -1ul) return false;
    for (unsigned char c : this->substr(1)) {
        auto d = hex_char_to_digit(c);
        if (d == -1) break;
        val = val * 16 + d;
    }
    if (v) *v = val;
    return true;
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

namespace photon {

inline uint64_t check_cases(uint64_t x, char a, char z) {
    uint64_t all_bytes = 0x0101010101010101;
    uint64_t heptets = x & (0x7f * all_bytes);
    uint64_t is_ascii = ~x & (0x80 * all_bytes);
    uint64_t is_gt_Z = heptets + (0x7f - z) * all_bytes;
    uint64_t is_ge_A = heptets + (0x80 - a) * all_bytes;
    return (is_ge_A ^ is_gt_Z) & is_ascii;
}

inline uint64_t tolower_fast8(uint64_t x) {
    uint64_t is_upper = check_cases(x, 'A', 'X');
    return x | (is_upper >> 2);
}

inline uint64_t toupper_fast8(uint64_t x) {
    uint64_t is_lower = check_cases(x, 'a', 'z');
    return x ^ (is_lower >> 2);
}

template<typename F1, typename F8> inline
void convert_case(char* out, const char* in, size_t len, F1 f1, F8 f8) {
    if (unlikely(len == 0))
        len = strlen(in);
    if (unlikely(len < 8)) {
        for (size_t i = 0; i < len; i++)
            out[i] = f1(in[i]);
    } else {
        for (size_t i = 0; i < len/8*8; i+=8)
            *(uint64_t*)&out[i] = f8(*(uint64_t*)&in[i]);
        *(uint64_t*)&out[len-8] = f8(*(uint64_t*)&in[len-8]);
    }
    out[len] = '\0';
}

void tolower_fast(char* out, const char* in, size_t len) {
    convert_case(out, in, len,
        [](char x)     { return tolower_fast(x); },
        [](uint64_t x) { return tolower_fast8(x); });
}

void toupper_fast(char* out, const char* in, size_t len) {
    convert_case(out, in, len,
        [](char x)     { return toupper_fast(x); },
        [](uint64_t x) { return toupper_fast8(x); });
}

inline uint64_t icmp8(const char* a, const char* b) {
    auto ca = tolower_fast8(*(uint64_t*)a);
    auto cb = tolower_fast8(*(uint64_t*)b);
    return __builtin_bswap64(ca) - __builtin_bswap64(cb);
}
inline int spaceship(uint64_t x) {
    uint32_t high = x >> 32, low = x & 0xffffffff;
    return int(high | (low >> 1) | (low & 1));
}
int stricmp_fast(std::string_view a, std::string_view b) {
    size_t i = 0, len = std::min(a.size(), b.size());
    if (unlikely(len < 8)) {
        for (; i < len; i++) {
            auto ca = tolower_fast(a[i]);
            auto cb = tolower_fast(b[i]);
            if (auto x = ca - cb)
                return x;
        }
        return spaceship(a.size() - b.size());
    }
    for (; i < len/8*8; i+=8)
        if (auto x = icmp8(&a[i], &b[i]))
            return spaceship(x);
    if (likely(i < len)) // len >= 8
        if (auto x = icmp8(&a[len-8], &b[len-8]))
            return spaceship(x);
    return spaceship(a.size() - b.size());
}

}