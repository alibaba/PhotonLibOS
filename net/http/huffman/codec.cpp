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

#include <memory>
#include <type_traits>
#include <cstring>
#include <photon/common/utility.h>
#include "codec.h"

#undef  LOG_ERROR_RETURN
#define LOG_ERROR_RETURN(a, b, ...) return b;

namespace photon {
namespace net {
namespace http {

struct HuffmanDecodeEntry {
    uint8_t next_state = 0;
    uint8_t num_symbols = 0;
    uint8_t symbols[2] = {0, 0};
};

typedef HuffmanDecodeEntry (*HuffmanDecodeTable)[256];

#include "code.i"

inline uint8_t bswap(char x) { return x; }
inline uint32_t bswap(uint32_t x) { return __builtin_bswap32(x); }

size_t huffman_encoded_length(std::string_view src) {
    size_t bits = 0;
    for (unsigned char c : src)
        bits += code_length[c];
    return (bits + 7) / 8;
}

ssize_t huffman_encode(std::string_view src, char* dst, const char* dst_end) {
    uint64_t x = 0, coded = 0;
    auto output = [&](auto*& ptr) {
        const auto nbits = sizeof(*ptr) * 8;
        if (unlikely(coded >= nbits)) {
            coded -= nbits;
            if (likely((char*)ptr < dst_end)) {
                using T = typename std::remove_reference<decltype(*ptr)>::type;
                *ptr++ = bswap((T)(x >> coded));
            }
            x &= (1ULL << coded) - 1;
        }
    };

    dst_end -= 4;
    auto p = (uint32_t*)dst;
    for (unsigned char c : src) {
        assert((huffman_code[c] >> code_length[c]) == 0);
        x = (x << code_length[c]) | huffman_code[c];
        coded += code_length[c];
        output(p);
    }
    dst_end += 4;

    auto pc = (char*)p;
    while (coded >= 8)
        output(pc);
    if (coded) {
        assert(coded < 8);
        auto padding = 8 - coded;
        x = (x << padding) | ((1ULL << padding) - 1);
        coded = 8;
        output(pc);
    }
    return pc - dst;
}

#include "tree.i"

static ssize_t huffman_decode_slow(std::string_view src, char* dst,
            const char* dst_end, uint16_t* start_node = nullptr) {
    auto p = dst;
    auto t = huffman_tree;
    if (start_node) {
        t += *start_node;
        assert(*start_node < LEN(huffman_tree) && !t->isleaf());
    }
    for (size_t i = 0; i < src.size(); ++i) {
        signed char c = src[i];
        for (uint8_t n = 8; n; --n) {
            assert(!t->isleaf());
            auto nx = t->next[c < 0];
            c = (signed char)(((uint8_t)c) << 1);
            if (nx == 0) {
                assert(t == &huffman_tree[EOS_Node]);
                break;
            }
            t += nx;
            if (t >= huffman_tree + LEN(huffman_tree))
                LOG_ERROR_RETURN(EINVAL, -1, "invalid input data");
            if (unlikely(t->isleaf())) {
                if (p >= dst_end)
                    LOG_ERROR_RETURN(ENOBUFS, -1, "dst buffer too small");
                *p++ = t->get_symbol();
                t = huffman_tree;
            }
        }
    }
    if (start_node)
        *start_node = t - huffman_tree;
    return p - dst;
}

struct _HuffmanDecodeTable {
    HuffmanDecodeEntry huffman_decode_table[256][256];
    _HuffmanDecodeTable() {
        uint16_t num_states = 0;
        uint8_t state_map[LEN(huffman_tree)];
        uint16_t reverse_map[LEN(huffman_tree) / 2];
        for (size_t i = 0; i < LEN(huffman_tree); ++i) {
            if (!huffman_tree[i].isleaf()) {
                state_map[i] = num_states;
                assert(num_states < LEN(reverse_map));
                reverse_map[num_states++] = i;
            }
        }
        for (size_t idx = 0; idx < num_states; ++idx) {
            for (int byte = 0; byte < 256; ++byte) {
                auto& entry = huffman_decode_table[idx][byte];
                uint16_t state = reverse_map[idx];
                entry.num_symbols = huffman_decode_slow({(char*)&byte, 1}, 
                    (char*)entry.symbols, (char*)entry.symbols + 2, &state);
                assert(entry.num_symbols < 3);
                entry.next_state = state_map[state];
            }
        }
    }
};

[[maybe_unused]] static const HuffmanDecodeTable get_huffman_decode_table() {
    const static auto t = std::make_unique<_HuffmanDecodeTable>();
    return t->huffman_decode_table;
}

ssize_t huffman_decode(std::string_view src, char* dst, const char* dst_end) {
    auto p = dst;
    uint8_t state = 0;
    auto huffman_decode_table = get_huffman_decode_table();
    for (uint8_t c : src) {
        const auto& entry = huffman_decode_table[state][c];
        if (entry.next_state == 0xFF)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid input data");
        auto n = entry.num_symbols;
        if (p + n > dst_end)
            LOG_ERROR_RETURN(ENOBUFS, -1, "dst buffer too small");
        if (likely(n == 1)) *p++ = entry.symbols[0];
        else if (likely(n == 2)) { memcpy(p, entry.symbols, 2); p += 2; }
        else assert(n == 0);
        state = entry.next_state;
    }
    return p - dst;
}

}
}
}
