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
#include <utility>
#include "photon/common/utility.h"
#include "string_view.h"

namespace photon {

// a set of static strings stored continuously in a buffer at most 64KB,
// can be individually addressed via operator []
template<uint16_t N, uint16_t Size>
struct static_strings {
    uint16_t offsets[N+1];
    char buf[Size];
    std::string_view operator[](size_t i) const {
        assert(i < N);
        return {buf + offsets[i], size_t(offsets[i + 1] - offsets[i])};
    }
};

template<size_t N, const std::string_view (&strs)[N], std::size_t...I>
inline constexpr size_t sss_total_capacity(std::index_sequence<I...>) {
    return (0 + ... + strs[I].size()) + N;
}

template<size_t N, const std::string_view (&strs)[N]>
inline constexpr size_t sss_total_capacity() {
    return sss_total_capacity<N, strs>(std::make_index_sequence<N>{});
}

template<size_t N, size_t Size> inline constexpr
auto sss_construct(const std::string_view (&strs)[N]) {
    static_strings<N, Size> sss;
    size_t j = 0;
    for (auto& s: strs) {
        for (auto& c: s) {
            sss.buf[j++] = c;
        }
        sss.buf[j++] = '\0';
    }
    assert(j == Size);

    sss.offsets[0] = 0;
    for (size_t i = 0; i < N; ++i) {
        sss.offsets[i+1] = sss.offsets[i] + strs[i].size() + 1;
    }
    return sss;
}

#define SSS_CONSTRUCT(strs) ({                                  \
    constexpr size_t N = LEN(strs);                             \
    constexpr size_t Size = sss_total_capacity<N, strs>();      \
    sss_construct<N, Size>(strs);                               \
})

}
