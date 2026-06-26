#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <vector>
#include <algorithm>
#include "code.i"

struct Code {
    uint32_t code;
    uint8_t len;
    uint8_t symbol;
};

Code hc[num_huffman_codes];

struct HuffmanTreeNode {
    uint16_t next[2] = {0};
    uint32_t symbol = -1;
    bool isleaf() { return next[0] == 0; }
};

std::vector<HuffmanTreeNode> hfmtree;
uint32_t max = 0;

void huffman_tree_gen() {
    for (int i = 0; i < num_huffman_codes; ++i) {
        hc[i] = { huffman_code[i], code_length[i], (uint8_t)i};
        uint32_t m = hc[i].code << (32u - hc[i].len);
        assert((m >> (32u - hc[i].len)) == hc[i].code);
        hc[i].code = m;
    }
    std::sort(hc, hc + num_huffman_codes, [](auto a, auto b) {
        assert(a.code != b.code);
        return a.code < b.code;
    });
    hfmtree.emplace_back();
    for (int i = 0; i < num_huffman_codes; ++i) {
        int p = 0;
        for (int j = 0; j < hc[i].len; ++j) {
            int code = hc[i].code << j;
            auto nx = &hfmtree[p].next[code < 0];
            if (*nx) {
                p += *nx;
            } else {
                auto diff = hfmtree.size() - p;
                assert(diff > 0);
                if (diff > max) max = diff;
                assert(max < num_huffman_codes);
                *nx = diff;
                p = hfmtree.size();
                hfmtree.emplace_back();
            }
            assert(hfmtree[p].symbol == -1);
        }
        assert(p == hfmtree.size() - 1);
        hfmtree[p].symbol = hc[i].symbol;
    }
}

void print_huffman_tree() {
    puts("const static HuffmanTreeNode huffman_tree[512] = {");
    int eos_node = -1;
    auto N = (num_huffman_codes - 1) * 2;
    for (int i = 0; i < hfmtree.size() - 8; ) {
        printf("    ");
        for (int j = 0; j < 8; ++j, ++i) {
            assert(hfmtree[i].next[0] || hfmtree[i].symbol != -1);
            uint8_t nx0 = hfmtree[i].next[0];
            uint8_t nx1 = nx0 ? hfmtree[i].next[1] : hfmtree[i].symbol;
            if (nx0 && (i + nx0 >= N || i + nx1 >= N)) {
                assert(eos_node == -1 && nx1 > 0);
                eos_node = i;
                nx1 = 0;
            }
            printf("{0x%02x, 0x%02x}, ", nx0, nx1);
        }
        printf("\n");
    }
    puts("};");
    printf("\nconst uint32_t EOS_Node = %u;\n", eos_node);
}

int main() {
    huffman_tree_gen();
    print_huffman_tree();
    return 0;
}
