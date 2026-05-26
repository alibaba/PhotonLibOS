/*
Copyright 2025 The Photon Authors

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

#include "xxhash.h"

uint32_t xxhash32_sw(const uint8_t* data, size_t len, uint32_t seed) {
    constexpr uint32_t PRIME32_1 = 0x9E3779B1;
    constexpr uint32_t PRIME32_2 = 0x85EBCA77;
    constexpr uint32_t PRIME32_3 = 0xC2B2AE3D;
    constexpr uint32_t PRIME32_4 = 0x27D4EB2F;
    constexpr uint32_t PRIME32_5 = 0x165667B1;

    auto rotl = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    auto round = [&](uint32_t acc, uint32_t val) {
        acc += val * PRIME32_2;
        acc = rotl(acc, 13);
        acc *= PRIME32_1;
        return acc;
    };
    auto read32 = [](const uint8_t* p) -> uint32_t {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    };

    auto* ptr = data;
    uint32_t h32;

    if (len >= 16) {
        uint32_t v1 = seed + PRIME32_1 + PRIME32_2;
        uint32_t v2 = seed + PRIME32_2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - PRIME32_1;
        size_t remaining = len;
        while (remaining >= 16) {
            v1 = round(v1, read32(ptr));    ptr += 4;
            v2 = round(v2, read32(ptr));    ptr += 4;
            v3 = round(v3, read32(ptr));    ptr += 4;
            v4 = round(v4, read32(ptr));    ptr += 4;
            remaining -= 16;
        }
        h32 = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    } else {
        h32 = seed + PRIME32_5;
    }

    h32 += (uint32_t)len;

    size_t remaining = len & 15;
    while (remaining >= 4) {
        h32 += read32(ptr) * PRIME32_3;
        h32 = rotl(h32, 17) * PRIME32_4;
        ptr += 4;
        remaining -= 4;
    }
    while (remaining >= 1) {
        h32 += (*ptr) * PRIME32_5;
        h32 = rotl(h32, 11) * PRIME32_1;
        ptr += 1;
        remaining -= 1;
    }

    h32 ^= h32 >> 15;
    h32 *= PRIME32_2;
    h32 ^= h32 >> 13;
    h32 *= PRIME32_3;
    h32 ^= h32 >> 16;
    return h32;
}
