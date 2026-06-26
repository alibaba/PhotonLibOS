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

#include <photon/photon.h>
#include "../huffman/codec.h"
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace photon::net::http;

// ---- Huffman coding (RFC 7541) ----

static struct RfcExample {
    char section[8];
    char header[24];
    char input[64];
    uint8_t expected[32];
} rfc_examples[] = {
    {"C.4.1", ":authority",       "www.example.com",                  {0x0c,0xf1,0xe3,0xc2,0xe5,0xf2,0x3a,0x6b,0xa0,0xab,0x90,0xf4,0xff}},
    {"C.4.2", "cache-control",    "no-cache",                         {0x06,0xa8,0xeb,0x10,0x64,0x9c,0xbf}},
    {"C.4.3", "custom-key",       "custom-key",                       {0x08,0x25,0xa8,0x49,0xe9,0x5b,0xa9,0x7d,0x7f}},
    {"C.4.3", "custom-value",     "custom-value",                     {0x09,0x25,0xa8,0x49,0xe9,0x5b,0xb8,0xe8,0xb4,0xbf}},
    {"C.5.1", ":status",          "302",                              {0x02,0x64,0x02}},
    {"C.5.1", "cache-control",    "private",                          {0x05,0xae,0xc3,0x77,0x1a,0x4b}},
    {"C.5.1", "date",             "Mon, 21 Oct 2013 20:13:21 GMT",    {0x16,0xd0,0x7a,0xbe,0x94,0x10,0x54,0xd4,0x44,0xa8,0x20,0x05,0x95,0x04,0x0b,0x81,0x66,0xe0,0x82,0xa6,0x2d,0x1b,0xff}},
    {"C.5.1", "location",         "https://www.example.com",          {0x11,0x9d,0x29,0xad,0x17,0x18,0x63,0xc7,0x8f,0x0b,0x97,0xc8,0xe9,0xae,0x82,0xae,0x43,0xd3}},
    {"C.5.2", ":status",          "307",                              {0x03,0x64,0x0e,0xff}},
    {"C.5.2", "cache-control",    "private",                          {0x05,0xae,0xc3,0x77,0x1a,0x4b}},
    {"C.5.2", "date",             "Mon, 21 Oct 2013 20:13:21 GMT",    {0x16,0xd0,0x7a,0xbe,0x94,0x10,0x54,0xd4,0x44,0xa8,0x20,0x05,0x95,0x04,0x0b,0x81,0x66,0xe0,0x82,0xa6,0x2d,0x1b,0xff}},
    {"C.5.2", "location",         "https://www.example.com",          {0x11,0x9d,0x29,0xad,0x17,0x18,0x63,0xc7,0x8f,0x0b,0x97,0xc8,0xe9,0xae,0x82,0xae,0x43,0xd3}},
    {"C.5.3", ":status",          "200",                              {0x02,0x10,0x01}},
    {"C.5.3", "cache-control",    "private",                          {0x05,0xae,0xc3,0x77,0x1a,0x4b}},
    {"C.5.3", "date",             "Mon, 21 Oct 2013 20:13:21 GMT",    {0x16,0xd0,0x7a,0xbe,0x94,0x10,0x54,0xd4,0x44,0xa8,0x20,0x05,0x95,0x04,0x0b,0x81,0x66,0xe0,0x82,0xa6,0x2d,0x1b,0xff}},
    {"C.5.3", "location",         "https://www.example.com",          {0x11,0x9d,0x29,0xad,0x17,0x18,0x63,0xc7,0x8f,0x0b,0x97,0xc8,0xe9,0xae,0x82,0xae,0x43,0xd3}},
    {"C.5.3", "content-encoding", "gzip",                             {0x03,0x9b,0xd9,0xab}},
};

TEST(h2, huffman_rfc_examples) {
    for (auto& ex : rfc_examples) {
        size_t exp_len = ex.expected[0];
        const uint8_t* exp_data = ex.expected + 1;

        auto elen = huffman_encoded_length(ex.input);
        EXPECT_EQ(elen, exp_len)
            << "section=" << ex.section << " header=" << ex.header;

        char encoded[256];
        auto ret = huffman_encode(ex.input, encoded, encoded + sizeof(encoded));
        EXPECT_EQ(ret, (ssize_t)elen)
            << "section=" << ex.section << " header=" << ex.header;
        EXPECT_EQ(memcmp(encoded, exp_data, elen), 0)
            << "section=" << ex.section << " header=" << ex.header
            << " input=\"" << ex.input << "\"";

        char decoded[256];
        auto dret = huffman_decode({encoded, (size_t)ret},
                                   decoded, decoded + sizeof(decoded));
        EXPECT_EQ(dret, (ssize_t)strlen(ex.input))
            << "section=" << ex.section << " header=" << ex.header;
        EXPECT_EQ(std::string_view(decoded, dret), ex.input)
            << "section=" << ex.section << " header=" << ex.header;
    }
}

TEST(h2, huffman_empty) {
    auto elen = huffman_encoded_length("");
    EXPECT_EQ(elen, 0u);

    char encoded[16], decoded[16];
    auto ret = huffman_encode("", encoded, encoded + sizeof(encoded));
    EXPECT_EQ(ret, 0);

    auto dret = huffman_decode({encoded, 0}, decoded, decoded + sizeof(decoded));
    EXPECT_EQ(dret, 0);
}

TEST(h2, huffman_single_char) {
    char encoded[16], decoded[16];

    auto elen = huffman_encoded_length("a");
    EXPECT_GT(elen, 0u);

    auto ret = huffman_encode("a", encoded, encoded + sizeof(encoded));
    EXPECT_EQ(ret, (ssize_t)elen);

    auto dret = huffman_decode({encoded, (size_t)ret}, decoded, decoded + sizeof(decoded));
    EXPECT_EQ(dret, 1);
    EXPECT_EQ(decoded[0], 'a');
}

TEST(h2, huffman_long_repetitive) {
    std::string test_str(1000, 'a');
    auto elen = huffman_encoded_length(test_str);
    EXPECT_LT(elen, test_str.size());

    std::vector<char> encoded(elen + 16);
    std::vector<char> decoded(test_str.size() + 16);

    auto ret = huffman_encode(test_str, encoded.data(), encoded.data() + encoded.size());
    EXPECT_EQ(ret, (ssize_t)elen);

    auto dret = huffman_decode({encoded.data(), (size_t)ret}, decoded.data(), decoded.data() + decoded.size());
    EXPECT_EQ(dret, (ssize_t)test_str.size());
    EXPECT_EQ(std::string_view(decoded.data(), dret), test_str);
}

TEST(h2, huffman_dst_size_overload) {
    const char* input = "hello world";
    auto elen = huffman_encoded_length(input);

    std::vector<char> encoded(elen + 16);
    auto ret = huffman_encode(input, encoded.data(), encoded.size());
    EXPECT_EQ(ret, (ssize_t)elen);

    std::vector<char> decoded(64);
    auto dret = huffman_decode({encoded.data(), (size_t)ret}, decoded.data(), decoded.size());
    EXPECT_EQ(dret, (ssize_t)strlen(input));
    EXPECT_EQ(std::string_view(decoded.data(), dret), input);
}

TEST(h2, huffman_all_bytes) {
    std::string input;
    for (int i = 0; i < 256; i++)
        input.push_back((char)i);

    auto elen = huffman_encoded_length(input);
    std::vector<char> encoded(elen + 16);
    auto ret = huffman_encode(input, encoded.data(), encoded.data() + encoded.size());
    ASSERT_EQ(ret, (ssize_t)elen);

    std::vector<char> decoded(input.size() + 16);
    auto dret = huffman_decode({encoded.data(), (size_t)ret}, decoded.data(), decoded.data() + decoded.size());
    ASSERT_EQ(dret, (ssize_t)input.size());
    EXPECT_EQ(std::string_view(decoded.data(), dret), input);
}

TEST(h2, huffman_decode_buffer_too_small) {
    const char* input = "hello world";
    auto elen = huffman_encoded_length(input);
    std::vector<char> encoded(elen + 16);
    huffman_encode(input, encoded.data(), encoded.data() + encoded.size());

    char decoded[1];
    auto dret = huffman_decode({encoded.data(), (size_t)elen}, decoded, decoded + 1);
    EXPECT_EQ(dret, -1);
}

TEST(h2, huffman_encode_buffer_too_small) {
    const char* input = "hello world";
    auto elen = huffman_encoded_length(input);

    char encoded[1];
    auto ret = huffman_encode(input, encoded, encoded + 1);
    EXPECT_GE(ret, 0);
    EXPECT_LT(ret, (ssize_t)elen);
}

TEST(h2, huffman_decode_corrupt_data) {
    const char* input = "hello world";
    auto elen = huffman_encoded_length(input);
    std::vector<char> encoded(elen + 16);
    auto ret = huffman_encode(input, encoded.data(), encoded.data() + encoded.size());
    ASSERT_GT(ret, 0);

    encoded[0] ^= 0x80;

    char decoded[64];
    auto dret = huffman_decode({encoded.data(), (size_t)ret}, decoded, decoded + sizeof(decoded));
    if (dret >= 0) {
        EXPECT_NE(std::string_view(decoded, dret), input);
    }
}

TEST(h2, huffman_various_strings) {
    const char* test_strings[] = {
        "Hello, World!",
        "The quick brown fox jumps over the lazy dog",
        "1234567890!@#$%^&*()",
        "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09",
        "MixedCase123WithSymbols!@#",
        "abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    };

    for (auto* s : test_strings) {
        std::string input(s);
        auto elen = huffman_encoded_length(input);
        std::vector<char> encoded(elen + 16);
        auto ret = huffman_encode(input, encoded.data(), encoded.data() + encoded.size());
        EXPECT_EQ(ret, (ssize_t)elen);

        std::vector<char> decoded(input.size() + 16);
        auto dret = huffman_decode({encoded.data(), (size_t)ret}, decoded.data(), decoded.data() + decoded.size());
        EXPECT_EQ(dret, (ssize_t)input.size()) << "input: " << input;
        EXPECT_EQ(std::string_view(decoded.data(), dret), input) << "input: " << input;
    }
}

int main(int argc, char** argv) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
