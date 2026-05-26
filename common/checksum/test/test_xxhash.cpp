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

#include <photon/common/checksum/xxhash.h>
#include <cstring>
#include <vector>
#include "../../../test/gtest.h"

// Reference vectors verified against official xxHash implementation.
struct TestCase {
    uint32_t expected;
    uint32_t seed;
    const char* input;
};

static const TestCase cases[] = {
    {0x02CC5D05u, 0,  ""},
    {0x0B2CB792u, 1,  ""},
    {0x550D7456u, 0,  "a"},
    {0x32D153FFu, 0,  "abc"},
    {0x63A14D5Fu, 0,  "abcdefghijklmnopqrstuvwxyz"},
    {0xD5CCFE8Eu, 42, "abcdefghijklmnopqrstuvwxyz"},
    {0xC2C45B69u, 0,  "0123456789abcdef"},
    {0xCC79B217u, 0,  "0123456789abcdefg"},
    {0xEB888D30u, 0,  "0123456789abcdef0123456789abcdef"},
};

TEST(XxHash32, ReferenceVectors) {
    for (auto& c : cases) {
        auto h = xxhash32_sw((const uint8_t*)c.input, strlen(c.input), c.seed);
        EXPECT_EQ(h, c.expected)
            << "input=\"" << c.input << "\" seed=" << c.seed;
    }
}

TEST(XxHash32, Deterministic) {
    const char* s = "The quick brown fox jumps over the lazy dog";
    auto h1 = xxhash32_sw((const uint8_t*)s, 43, 0);
    auto h2 = xxhash32_sw((const uint8_t*)s, 43, 0);
    EXPECT_EQ(h1, h2);
}

TEST(XxHash32, SeedChangesResult) {
    const char* s = "hello world";
    auto h0 = xxhash32_sw((const uint8_t*)s, 11, 0);
    auto h1 = xxhash32_sw((const uint8_t*)s, 11, 1);
    EXPECT_NE(h0, h1);
}

TEST(XxHash32, LargeBuffer) {
    std::vector<uint8_t> buf(1024 * 1024);
    for (size_t i = 0; i < buf.size(); i++)
        buf[i] = (uint8_t)(i & 0xFF);
    auto h = xxhash32_sw(buf.data(), buf.size(), 12345);
    EXPECT_NE(h, 0u);
    EXPECT_EQ(xxhash32_sw(buf.data(), buf.size(), 12345), h);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
