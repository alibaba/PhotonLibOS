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

#include <cstring>
#include <memory>
#include <gtest/gtest.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include "lz4.h"

static const unsigned int kSmallLen = 4096;
static const unsigned int kLargeLen = 128 * 1024;   // 128KB — two 64KB blocks
static const unsigned int kOutputMargin = 64;        // extra space for incompressible overhead

using photon::qat::ICodec;

struct CodecDeleter {
    void operator()(ICodec* c) const { delete c; }
};
using CodecPtr = std::unique_ptr<ICodec, CodecDeleter>;

TEST(QATLz4Test, CompressDecompressRoundtrip) {
    CodecPtr codec(photon::qat::new_lz4_codec());
    ASSERT_TRUE(codec);

    // Prepare input data (repetitive — compresses well)
    std::vector<uint8_t> input(kSmallLen);
    for (unsigned int i = 0; i < kSmallLen; i++) {
        input[i] = static_cast<uint8_t>('a' + (i % 26));
    }
    std::vector<uint8_t> compressed(kSmallLen + kOutputMargin);
    std::vector<uint8_t> decompressed(kSmallLen);

    ICodec::Buffer src{input.data(), kSmallLen};
    ICodec::Buffer dst{compressed.data(), compressed.size()};
    int comp_size = codec->compress(src, dst);
    ASSERT_GT(comp_size, 0) << "Compress failed";
    EXPECT_LT(static_cast<unsigned int>(comp_size), kSmallLen)
        << "Compressed size should be smaller for repetitive data";

    ICodec::Buffer dsrc{compressed.data(), static_cast<size_t>(comp_size)};
    ICodec::Buffer ddst{decompressed.data(), kSmallLen};
    int decomp_size = codec->decompress(dsrc, ddst);
    ASSERT_GT(decomp_size, 0) << "Decompress failed";
    EXPECT_EQ(static_cast<unsigned int>(decomp_size), kSmallLen);
    EXPECT_EQ(memcmp(input.data(), decompressed.data(), kSmallLen), 0);
}

TEST(QATLz4Test, CompressSmallData) {
    CodecPtr codec(photon::qat::new_lz4_codec());
    ASSERT_TRUE(codec);

    const char* msg = "Hello QAT LZ4!";
    unsigned int msg_len = strlen(msg);
    std::vector<uint8_t> compressed(1024);
    std::vector<uint8_t> decompressed(1024);

    ICodec::Buffer src{const_cast<char*>(msg), msg_len};
    ICodec::Buffer dst{compressed.data(), 1024};
    int comp_size = codec->compress(src, dst);
    ASSERT_GT(comp_size, 0);

    ICodec::Buffer dsrc{compressed.data(), static_cast<size_t>(comp_size)};
    ICodec::Buffer ddst{decompressed.data(), 1024};
    int decomp_size = codec->decompress(dsrc, ddst);
    ASSERT_GT(decomp_size, 0);
    EXPECT_EQ(static_cast<unsigned int>(decomp_size), msg_len);
    EXPECT_EQ(memcmp(msg, decompressed.data(), msg_len), 0);
}

TEST(QATLz4Test, MultiBlockRoundtrip) {
    CodecPtr codec(photon::qat::new_lz4_codec());
    ASSERT_TRUE(codec);

    // 128KB input — requires two 64KB blocks
    std::vector<uint8_t> input(kLargeLen);
    for (unsigned int i = 0; i < kLargeLen; i++) {
        input[i] = static_cast<uint8_t>('a' + (i % 26));
    }
    std::vector<uint8_t> compressed(kLargeLen + kOutputMargin);
    std::vector<uint8_t> decompressed(kLargeLen);

    ICodec::Buffer src{input.data(), kLargeLen};
    ICodec::Buffer dst{compressed.data(), compressed.size()};
    int comp_size = codec->compress(src, dst);
    ASSERT_GT(comp_size, 0) << "Multi-block compress failed";

    ICodec::Buffer dsrc{compressed.data(), static_cast<size_t>(comp_size)};
    ICodec::Buffer ddst{decompressed.data(), kLargeLen};
    int decomp_size = codec->decompress(dsrc, ddst);
    ASSERT_GT(decomp_size, 0) << "Multi-block decompress failed";
    EXPECT_EQ(static_cast<unsigned int>(decomp_size), kLargeLen);
    EXPECT_EQ(memcmp(input.data(), decompressed.data(), kLargeLen), 0);
}

TEST(QATLz4Test, IncompressibleData) {
    CodecPtr codec(photon::qat::new_lz4_codec());
    ASSERT_TRUE(codec);

    // Pseudo-random data — likely incompressible
    std::vector<uint8_t> input(kSmallLen);
    for (unsigned int i = 0; i < kSmallLen; i++) {
        input[i] = static_cast<uint8_t>(i * 131 + 17);
    }
    // For incompressible data, output may be larger than input
    std::vector<uint8_t> compressed(kSmallLen + kOutputMargin);
    std::vector<uint8_t> decompressed(kSmallLen);

    ICodec::Buffer src{input.data(), kSmallLen};
    ICodec::Buffer dst{compressed.data(), compressed.size()};
    int comp_size = codec->compress(src, dst);
    ASSERT_GT(comp_size, 0) << "Compress failed";

    ICodec::Buffer dsrc{compressed.data(), static_cast<size_t>(comp_size)};
    ICodec::Buffer ddst{decompressed.data(), kSmallLen};
    int decomp_size = codec->decompress(dsrc, ddst);
    ASSERT_GT(decomp_size, 0) << "Decompress failed";
    EXPECT_EQ(static_cast<unsigned int>(decomp_size), kSmallLen);
    EXPECT_EQ(memcmp(input.data(), decompressed.data(), kSmallLen), 0);
}

TEST(QATLz4Test, ConcurrentCompress) {
    CodecPtr codec(photon::qat::new_lz4_codec());
    ASSERT_TRUE(codec);

    const int kThreads = 4;
    std::vector<uint8_t> input(kSmallLen, 'x');
    std::vector<std::vector<uint8_t>> outputs(kThreads,
                                               std::vector<uint8_t>(kSmallLen + kOutputMargin));

    std::vector<photon::join_handle*> handles;
    for (int i = 0; i < kThreads; i++) {
        auto* th = photon::thread_create11([&, i]() {
            ICodec::Buffer src{input.data(), kSmallLen};
            ICodec::Buffer dst{outputs[i].data(), outputs[i].size()};
            int comp_size = codec->compress(src, dst);
            EXPECT_GT(comp_size, 0);

            std::vector<uint8_t> decomp(kSmallLen);
            ICodec::Buffer dsrc{outputs[i].data(), static_cast<size_t>(comp_size)};
            ICodec::Buffer ddst{decomp.data(), kSmallLen};
            int decomp_size = codec->decompress(dsrc, ddst);
            EXPECT_EQ(decomp_size, static_cast<int>(kSmallLen));
            EXPECT_EQ(memcmp(input.data(), decomp.data(), kSmallLen), 0);
        });
        handles.push_back(photon::thread_enable_join(th));
    }

    // Wait for all threads
    for (auto* h : handles) photon::thread_join(h);
}

TEST(QATLz4Test, NoBlockCksumRoundtrip) {
    // block_cksum=false, content_cksum=false
    CodecPtr codec(photon::qat::new_lz4_codec(-1, 4, false, false));
    ASSERT_TRUE(codec);

    std::vector<uint8_t> input(kSmallLen);
    for (unsigned int i = 0; i < kSmallLen; i++)
        input[i] = static_cast<uint8_t>('a' + (i % 26));
    std::vector<uint8_t> compressed(kSmallLen + kOutputMargin);
    std::vector<uint8_t> decompressed(kSmallLen);

    ICodec::Buffer src{input.data(), kSmallLen};
    ICodec::Buffer dst{compressed.data(), compressed.size()};
    int comp_size = codec->compress(src, dst);
    ASSERT_GT(comp_size, 0);

    ICodec::Buffer dsrc{compressed.data(), static_cast<size_t>(comp_size)};
    ICodec::Buffer ddst{decompressed.data(), kSmallLen};
    int decomp_size = codec->decompress(dsrc, ddst);
    ASSERT_GT(decomp_size, 0);
    EXPECT_EQ(static_cast<unsigned int>(decomp_size), kSmallLen);
    EXPECT_EQ(memcmp(input.data(), decompressed.data(), kSmallLen), 0);
}

TEST(QATLz4Test, ContentCksumOnlyRoundtrip) {
    // block_cksum=false, content_cksum=true (content checksum is active)
    CodecPtr codec(photon::qat::new_lz4_codec(-1, 4, false, true));
    ASSERT_TRUE(codec);

    std::vector<uint8_t> input(kSmallLen);
    for (unsigned int i = 0; i < kSmallLen; i++)
        input[i] = static_cast<uint8_t>('a' + (i % 26));
    std::vector<uint8_t> compressed(kSmallLen + kOutputMargin);
    std::vector<uint8_t> decompressed(kSmallLen);

    ICodec::Buffer src{input.data(), kSmallLen};
    ICodec::Buffer dst{compressed.data(), compressed.size()};
    int comp_size = codec->compress(src, dst);
    ASSERT_GT(comp_size, 0);

    ICodec::Buffer dsrc{compressed.data(), static_cast<size_t>(comp_size)};
    ICodec::Buffer ddst{decompressed.data(), kSmallLen};
    int decomp_size = codec->decompress(dsrc, ddst);
    ASSERT_GT(decomp_size, 0);
    EXPECT_EQ(static_cast<unsigned int>(decomp_size), kSmallLen);
    EXPECT_EQ(memcmp(input.data(), decompressed.data(), kSmallLen), 0);
}

TEST(QATLz4Test, ContentCksumOffWhenBlockCksumOn) {
    // block_cksum=true, content_cksum=true (content_cksum is considered off)
    CodecPtr codec(photon::qat::new_lz4_codec(-1, 4, true, true));
    ASSERT_TRUE(codec);

    std::vector<uint8_t> input(kSmallLen);
    for (unsigned int i = 0; i < kSmallLen; i++)
        input[i] = static_cast<uint8_t>('a' + (i % 26));
    std::vector<uint8_t> compressed(kSmallLen + kOutputMargin);
    std::vector<uint8_t> decompressed(kSmallLen);

    ICodec::Buffer src{input.data(), kSmallLen};
    ICodec::Buffer dst{compressed.data(), compressed.size()};
    int comp_size = codec->compress(src, dst);
    ASSERT_GT(comp_size, 0);

    // Output should NOT contain content checksum (considered off when block_cksum is on)
    // FLG byte should have bit4=1, bit2=0
    EXPECT_NE((compressed.data()[4] & 0x10), 0);  // block_cksum bit set
    EXPECT_EQ((compressed.data()[4] & 0x04), 0);  // content_cksum bit clear

    ICodec::Buffer dsrc{compressed.data(), static_cast<size_t>(comp_size)};
    ICodec::Buffer ddst{decompressed.data(), kSmallLen};
    int decomp_size = codec->decompress(dsrc, ddst);
    ASSERT_GT(decomp_size, 0);
    EXPECT_EQ(static_cast<unsigned int>(decomp_size), kSmallLen);
    EXPECT_EQ(memcmp(input.data(), decompressed.data(), kSmallLen), 0);
}

int main(int argc, char** argv) {
    photon::vcpu_init(0);
    photon::fd_events_init(0);
    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    photon::fd_events_fini();
    photon::vcpu_fini();
    return ret;
}
