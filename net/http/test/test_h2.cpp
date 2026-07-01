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
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/net/socket.h>
#include <photon/net/http/headers.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include "../streams.h"
#include "../huffman/codec.h"
#include <gtest/gtest.h>
#include <cstring>
#include <string_view>
#include <vector>
#include <memory>
#include "../../../common/memory-stream/memory-stream.h"

using namespace photon::net::http;
using namespace photon::net;

// ---- FrameHeader encode/decode ----

TEST(h2, frame_header_encode_decode) {
    char buf[20] = {0};
    auto& h = *(FrameHeader*)buf;
    h.type = FrameHeader::SETTINGS;
    h.flags = 0x01;  // ACK
    h.length = 0x12345;
    h.stream_id = 0x7654321;

    EXPECT_EQ(h.type, FrameHeader::SETTINGS);
    EXPECT_EQ(h.flags, 0x01);
    EXPECT_EQ(h.length, 0x12345u);
    EXPECT_EQ(h.stream_id, 0x7654321u);

    h.byte_order_encode();

    auto* raw = (uint8_t*)buf;
    EXPECT_EQ(raw[0], 0x01);
    EXPECT_EQ(raw[1], 0x23);
    EXPECT_EQ(raw[2], 0x45);
    EXPECT_EQ(raw[3], 0x04);
    EXPECT_EQ(raw[4], 0x01);
    EXPECT_EQ(raw[5], 0x07);
    EXPECT_EQ(raw[6], 0x65);
    EXPECT_EQ(raw[7], 0x43);
    EXPECT_EQ(raw[8], 0x21);

    h.byte_order_decode();

    EXPECT_EQ(h.type, FrameHeader::SETTINGS);
    EXPECT_EQ(h.flags, 0x01);
    EXPECT_EQ(h.length, 0x12345u);
    EXPECT_EQ(h.stream_id, 0x7654321u);
}

TEST(h2, frame_header_stream_id_mask) {
    char buf[20] = {0};
    auto& h = *(FrameHeader*)buf;
    h.type = FrameHeader::DATA;
    h.stream_id = 0xFFFFFFFF;
    h.byte_order_encode();
    h.byte_order_decode();
    EXPECT_EQ(h.stream_id, 0x7FFFFFFFu);
}

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

// ---- HPACK static table lookup (include streams.cpp for internal symbols) ----
#include "../streams.cpp"

// ---- SETTINGS frame encode/decode ----

TEST(h2, settings_encode_decode) {
    char buf[20] = {0};
    auto& h = *(SettingsFrameHeader*)buf;
    h.type = FrameHeader::SETTINGS;
    h.stream_id = 0;
    h.flags = 0;
    h.length = sizeof(Setting);
    auto& s = h.ext<Setting>(0);
    s.id = Setting::HEADER_TABLE_SIZE;
    s.value = 4096;

    auto* raw = (uint8_t*)buf;

    h.byte_order_encode();

    EXPECT_EQ(raw[9], 0x00);
    EXPECT_EQ(raw[10], 0x01);
    EXPECT_EQ(raw[11], 0x00);
    EXPECT_EQ(raw[12], 0x00);
    EXPECT_EQ(raw[13], 0x10);
    EXPECT_EQ(raw[14], 0x00);

    h.byte_order_decode();

    EXPECT_EQ(h.type, FrameHeader::SETTINGS);
    EXPECT_EQ(h.length, sizeof(Setting));
    EXPECT_EQ(s.id, Setting::HEADER_TABLE_SIZE);
    EXPECT_EQ(s.value, 4096u);
}

// ---- PING frame encode/decode ----

TEST(h2, ping_encode_decode) {
    char buf[20] = {0};
    auto& h = *(PingFrameHeader*)buf;
    h.type = FrameHeader::PING;
    h.stream_id = 0;
    h.flags = 0;
    h.length = 8;
    h.set_ack();
    uint8_t opaque[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memcpy(h.data, opaque, 8);

    h.byte_order_encode();
    h.byte_order_decode();

    EXPECT_EQ(h.type, FrameHeader::PING);
    EXPECT_TRUE(h.ack());
    EXPECT_EQ(memcmp(h.data, opaque, 8), 0);
}

// ---- WINDOW_UPDATE frame ----

TEST(h2, window_update_encode_decode) {
    char buf[20] = {0};
    auto& h = *(WindowUpdateFrameHeader*)buf;
    h.type = FrameHeader::WINDOW_UPDATE;
    h.stream_id = 1;
    h.flags = 0;
    h.window_size_increment = 0x7FFFFFFF;
    h.length = h.size() - sizeof(FrameHeader);

    h.byte_order_encode();
    h.byte_order_decode();

    EXPECT_EQ(h.type, FrameHeader::WINDOW_UPDATE);
    EXPECT_EQ(h.stream_id, 1u);
    EXPECT_EQ(h.window_size_increment, 0x7FFFFFFFu);
}

// ---- RST_STREAM frame ----

TEST(h2, rst_stream_encode_decode) {
    char buf[20] = {0};
    auto& h = *(ResetStreamFrameHeader*)buf;
    h.type = FrameHeader::RST_STREAM;
    h.stream_id = 3;
    h.flags = 0;
    h.error_code = FrameHeader::CANCEL;
    h.length = h.size() - sizeof(FrameHeader);

    h.byte_order_encode();
    h.byte_order_decode();

    EXPECT_EQ(h.type, FrameHeader::RST_STREAM);
    EXPECT_EQ(h.stream_id, 3u);
    EXPECT_EQ(h.error_code, (uint32_t)FrameHeader::CANCEL);
}

// ---- GOAWAY frame ----

TEST(h2, goaway_encode_decode) {
    char buf[20] = {0};
    auto& h = *(GoawayFrameHeader*)buf;
    h.type = FrameHeader::GOAWAY;
    h.stream_id = 0;
    h.flags = 0;
    h.last_stream_id = 0x7FFFFFFF;
    h.error_code = FrameHeader::NO_ERROR;
    h.length = h.size() - sizeof(FrameHeader);

    h.byte_order_encode();
    h.byte_order_decode();

    EXPECT_EQ(h.type, FrameHeader::GOAWAY);
    EXPECT_EQ(h.stream_id, 0u);
    EXPECT_EQ(h.last_stream_id, 0x7FFFFFFFu);
    EXPECT_EQ(h.error_code, (uint32_t)FrameHeader::NO_ERROR);
}

TEST(h2, hpack_static_table_lookup) {
    EXPECT_EQ(hpack_find_static_entry(":method", "GET"), 2);
    EXPECT_EQ(hpack_find_static_entry("x-no-such-header", ""), 0);
    EXPECT_EQ(hpack_find_static_entry(":scheme", "ftp"), -(int)6);

    LOG_DEBUG(VALUE(static_header_names.size()),
              VALUE(static_header_values.size()));
    auto sz = static_header_names.size();
    for (size_t i = 1; i < sz; i++) {
        auto name = static_header_names[i];
        auto value = (i < static_header_values.size())
            ? static_header_values[i] : std::string_view{};
        int result = hpack_find_static_entry(name, value);
        EXPECT_NE(result, 0) << "index " << i << " name=\"" << name << "\" not found";
    }
}

// ---- HPACK integer encoding/decoding (RFC 7541 Appendix C.1) ----

TEST(h2, hpack_integer_encode_decode) {
    char buf[16], *p = buf;

    // C.1.1: 10 encoded with 5-bit prefix
    hpack_encode_integer(p, 0, 5, 10);
    EXPECT_EQ(p - buf, 1);
    EXPECT_EQ(buf[0] & 0x1f, 0x0a);

    // C.1.2: 1337 encoded with 5-bit prefix
    p = buf;
    hpack_encode_integer(p, 0, 5, 1337);
    EXPECT_EQ(p - buf, 3);
    EXPECT_EQ(buf[0] & 0x1f, 31);
    EXPECT_EQ((unsigned char)buf[1], 0x9au);
    EXPECT_EQ(buf[2], 0x0a);

    // C.1.3: 42 encoded with 8-bit prefix
    p = buf;
    hpack_encode_integer(p, 0, 8, 42);
    EXPECT_EQ(p - buf, 1);
    EXPECT_EQ(buf[0], 0x2a);
}

// ---- HPACK integer decoding ----

TEST(h2, hpack_integer_decode) {
    // 10 with 5-bit prefix
    {
        uint8_t data[] = {0x0a};
        const char* r = (char*)data;
        EXPECT_EQ(hpack_decode_integer(r, 5).value, 10u);
    }
    // 42 with 8-bit prefix
    {
        uint8_t data[] = {0x2a};
        const char* r = (char*)data;
        EXPECT_EQ(hpack_decode_integer(r, 8).value, 42u);
    }
}

// ---- HPACK string encoding (RFC 7541 Appendix C) ----

TEST(h2, hpack_string_encode_decode) {
    // Huffman encoded string
    char buf[256], *p = buf;
    auto ret = hpack_encode_string(p, "www.example.com");
    EXPECT_GT(ret, 0);
    const char* r = buf;
    char out[256];
    auto dret = hpack_decode_string(r, r + ret, out, sizeof(out));
    EXPECT_GT(dret, 0);
    EXPECT_EQ(std::string_view(out, dret), "www.example.com");

    // Plain string
    p = buf;
    ret = hpack_encode_string(p, "hello", false);
    EXPECT_GT(ret, 0);
    r = buf;
    dret = hpack_decode_string(r, r + ret, out, sizeof(out));
    EXPECT_GT(dret, 0);
    EXPECT_EQ(std::string_view(out, dret), "hello");
}

// ---- HPACK header encoding (RFC 7541 Appendix C) ----

TEST(h2, hpack_header_encode_decode) {
    char buf[4096], *p = buf;

    // C.2.1: :method GET → indexed (static table index 2)
    auto ret = hpack_encode_field(p, ":method", "GET");
    EXPECT_GT(ret, 0);
    EXPECT_EQ(buf[0] & 0x80, 0x80);  // indexed header field

    // C.2.3: custom-key → literal with new name
    p = buf;
    ret = hpack_encode_field(p, "custom-key", "custom-value");
    EXPECT_GT(ret, 0);
    EXPECT_EQ(buf[0] & 0xc0, 0x40);  // literal with incremental indexing

    // Decode round-trip
    CommonHeaders<4096> decoded;
    EXPECT_EQ(hpack_decode_headers(buf, ret, decoded), 0);
    EXPECT_EQ(decoded["custom-key"], "custom-value");
}

// ---- HPACK full header block round-trip (RFC 7541 Appendix C.3) ----

TEST(h2, hpack_full_roundtrip) {
    CommonHeaders<4096> headers_out;
    headers_out.insert(":method", "GET");
    headers_out.insert(":scheme", "http");
    headers_out.insert(":path", "/");
    headers_out.insert(":authority", "www.example.com");

    char buf[4096], *p = buf;
    for (auto kv : headers_out) {
        auto ret = hpack_encode_field(p, kv.first, kv.second);
        EXPECT_GT(ret, 0);
    }
    size_t encoded_len = p - buf;
    EXPECT_GT(encoded_len, 0u);

    CommonHeaders<4096> headers_in;
    EXPECT_EQ(hpack_decode_headers(buf, encoded_len, headers_in), 0);

    EXPECT_EQ(headers_in[":method"], "GET");
    EXPECT_EQ(headers_in[":scheme"], "http");
    EXPECT_EQ(headers_in[":path"], "/");
    EXPECT_EQ(headers_in[":authority"], "www.example.com");
}

// ---- HPACK indexing modes (RFC 7541 Section 6.2) ----

TEST(h2, hpack_encode_no_index) {
    char buf[4096], *p = buf;

    // NoIndex with indexed name: top 4 bits = 0000
    auto ret = hpack_encode_field(p, "cache-control", "no-cache",
                                  HpackIndexing::NoIndex);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(buf[0] >> 4, 0x00);

    // NoIndex with new name: top 4 bits = 0000
    p = buf;
    ret = hpack_encode_field(p, "custom-key", "custom-value",
                             HpackIndexing::NoIndex);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(buf[0] >> 4, 0x00);

    // Decode round-trip
    CommonHeaders<4096> decoded;
    EXPECT_EQ(hpack_decode_headers(buf, ret, decoded), 0);
    EXPECT_EQ(decoded["custom-key"], "custom-value");
}

TEST(h2, hpack_encode_never_index) {
    char buf[4096], *p = buf;

    // NeverIndex with indexed name: top 4 bits = 0001
    auto ret = hpack_encode_field(p, "cache-control", "no-cache",
                                  HpackIndexing::NeverIndex);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(buf[0] >> 4, 0x01);

    // NeverIndex with new name: top 4 bits = 0001
    p = buf;
    ret = hpack_encode_field(p, "custom-key", "custom-value",
                             HpackIndexing::NeverIndex);
    EXPECT_GT(ret, 0);
    EXPECT_EQ(buf[0] >> 4, 0x01);

    // Decode round-trip
    CommonHeaders<4096> decoded;
    EXPECT_EQ(hpack_decode_headers(buf, ret, decoded), 0);
    EXPECT_EQ(decoded["custom-key"], "custom-value");
}

TEST(h2, hpack_roundtrip_all_modes) {
    struct TestCase {
        const char* name;
        const char* value;
        HpackIndexing indexing;
    };
    TestCase cases[] = {
        {":method", "GET",        HpackIndexing::Incremental},
        {":path", "/",           HpackIndexing::Incremental},
        {"custom-key", "val1",   HpackIndexing::NoIndex},
        {"x-secret", "secret",   HpackIndexing::NeverIndex},
        {"cache-control", "no-cache", HpackIndexing::NoIndex},
        {"authorization", "token", HpackIndexing::NeverIndex},
    };

    char buf[4096], *p = buf;
    for (auto& tc : cases) {
        auto ret = hpack_encode_field(p, tc.name, tc.value, tc.indexing);
        EXPECT_GT(ret, 0) << "failed to encode " << tc.name;
    }
    size_t encoded_len = p - buf;

    CommonHeaders<4096> decoded;
    EXPECT_EQ(hpack_decode_headers(buf, encoded_len, decoded), 0);

    for (auto& tc : cases) {
        EXPECT_EQ(decoded[tc.name], tc.value) << "mismatch for " << tc.name;
    }
}

// ---- H2Connection/H2Stream integration test ----

TEST(h2, connection_send_headers) {
    auto dms = std::unique_ptr<DuplexMemoryStream>(new_duplex_memory_stream(4096));
    EXPECT_NE(dms, nullptr);

    auto client = std::unique_ptr<H2Connection>(new H2Connection(dms->endpoint_a, false));
    EXPECT_NE(client, nullptr);

    auto stream = client->create_stream();
    EXPECT_EQ(stream.id(), 1u);
    CommonHeaders<4096> req_headers;
    req_headers.insert(":method", "GET");
    EXPECT_EQ(stream.send_headers(req_headers, true), 0);
}

TEST(h2, connection_send_data) {
    auto dms = std::unique_ptr<DuplexMemoryStream>(new_duplex_memory_stream(4096));
    EXPECT_NE(dms, nullptr);
    auto client = std::unique_ptr<H2Connection>(new H2Connection(dms->endpoint_a, false));
    EXPECT_NE(client, nullptr);
    auto stream = client->create_stream();
    EXPECT_EQ(stream.send_data("hello", 5, true), 0);
}

TEST(h2, connection_send_data_iovec) {
    auto dms = std::unique_ptr<DuplexMemoryStream>(new_duplex_memory_stream(4096));
    EXPECT_NE(dms, nullptr);
    auto client = std::unique_ptr<H2Connection>(new H2Connection(dms->endpoint_a, false));
    EXPECT_NE(client, nullptr);
    auto stream = client->create_stream();
    iovec iov[2] = {{(void*)"abc", 3}, {(void*)"def", 3}};
    EXPECT_EQ(stream.send_data(iov, 2, true), 0);
}

TEST(h2, connection_multi_stream) {
    auto dms = std::unique_ptr<DuplexMemoryStream>(new_duplex_memory_stream(4096));
    EXPECT_NE(dms, nullptr);
    auto client = std::unique_ptr<H2Connection>(new H2Connection(dms->endpoint_a, false));
    EXPECT_NE(client, nullptr);
    auto s1 = client->create_stream();
    auto s3 = client->create_stream();
    EXPECT_EQ(s1.id(), 1u);
    EXPECT_EQ(s3.id(), 3u);
    EXPECT_NE(s1.id(), s3.id());
    EXPECT_EQ(s1.send_headers(CommonHeaders<4096>(), true), 0);
    EXPECT_EQ(s3.send_headers(CommonHeaders<4096>(), true), 0);
}

TEST(h2, connection_roundtrip_preface_only) {
    auto dms = std::unique_ptr<DuplexMemoryStream>(new_duplex_memory_stream(4096));
    EXPECT_NE(dms, nullptr);

    auto th = photon::thread_create11([&dms]() {
        auto server = std::unique_ptr<H2Connection>(new H2Connection(dms->endpoint_b, false));
        EXPECT_NE(server, nullptr);
        EXPECT_EQ(server->recv_preface(), 0);
        EXPECT_EQ(server->send_preface(), 0);
        auto srv_stream = server->accept_stream();
        CommonHeaders<4096> req_headers;
        EXPECT_EQ(srv_stream.recv_headers(req_headers), 0);
    });
    photon::thread_enable_join(th);

    auto client = std::unique_ptr<H2Connection>(new H2Connection(dms->endpoint_a, false));
    EXPECT_NE(client, nullptr);
    EXPECT_EQ(client->send_preface(), 0);
    EXPECT_EQ(client->recv_preface(), 0);
    auto stream = client->create_stream();
    CommonHeaders<4096> req_headers;
    req_headers.insert(":method", "GET");
    EXPECT_EQ(stream.send_headers(req_headers, true), 0);

    thread_join((photon::join_handle*)th);
}

int main(int argc, char** argv) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
