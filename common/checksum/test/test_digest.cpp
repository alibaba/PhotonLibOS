const static char test_data[] = R"(/*
 * Copyright 2013-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */)";

#include <photon/common/string_view.h>
#include <photon/common/estring.h>
#include "../../../test/gtest.h"
#include "../digest.h"

 template<typename T, typename F>
 void test_digest(T&& shax, F truth_hasher) {
    std::string_view s { test_data, sizeof(test_data) };
    while (s.size()) {
        unsigned int len = rand() % 64;
        if (len > s.size()) len = s.size();
        printf("%u ", len);
        shax.update(s.data(), len);
        s.remove_prefix(len);
    }
    puts("OK");
    unsigned char hash[T::DIGEST_LENGTH] = {0},
                 truth[T::DIGEST_LENGTH] = {0};
    shax.finalize(hash);
    truth_hasher((unsigned char*)test_data, sizeof(test_data), truth);
    EXPECT_EQ(0, memcmp(truth, hash, T::DIGEST_LENGTH));
 }

TEST(hash, sha1) {
    test_digest(photon::sha1(), &SHA1);
}

TEST(hash, sha256) {
    test_digest(photon::sha256(), &SHA256);
}

TEST(hash, sha512) {
    test_digest(photon::sha512(), &SHA512);
}

TEST(hash, md5) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    auto MD5 = [](const unsigned char* data, size_t len, unsigned char* hash){
        photon::md5 md5;
        md5.update(data, len);
        md5.finalize(hash);
    };
#endif
    test_digest(photon::md5(), MD5);
}

const static char test_key[] = "test_key";

template<typename HMAC, size_t N>
void test_hmac(const char (&truth)[N]) {
    auto hmac = HMAC(test_key, test_data);
    std::string_view TRUTH{truth, N - 1};
    EXPECT_EQ(hmac, TRUTH);
    if (hmac != TRUTH)
        for(unsigned char c: hmac.finalize())
            printf("\\x%02x", c);
}

TEST(hash, hmac_sha1) {
    test_hmac<photon::HMAC_SHA1>("\x99\x14\x57\x35\xb9\x0c\xad\xfd\x4c\x6a\xb8\xbd\x0f\x20\x97\x6a\x34\x82\x23\xc1");
}

TEST(hash, hmac_sha256) {
    test_hmac<photon::HMAC_SHA256>("\xc6\xa7\x4c\x00\xd8\xe7\x28\xac\x8e\x9a\x41\x13\xbe\x34\x8e\xc7\xc0\xd8\x70\x54\xbd\x27\x7c\xd0\x7d\x0f\xd6\x3b\xd2\x85\xaa\x71");
}

TEST(hash, hmac_sha512) {
    test_hmac<photon::HMAC_SHA512>("\x56\xaa\x86\x74\x2c\xc8\xc9\x0a\x10\xc8\xa8\x3a\x4d\x5d\x8a\x6f\xab\x2f\x3f\x9e\xc7\x6b\x82\xc6\x3b\x70\x62\x23\x07\x8c\xc5\x0a\x78\xc0\xc6\xc8\x98\x67\xe9\x31\x66\xab\xf1\xe2\x34\xd6\x24\xfd\x59\x40\xfc\x9a\x98\x7e\xb5\xad\x8f\xce\x88\x7a\x1c\x5a\xee\x71");
}


int main() {
    testing::InitGoogleTest();
    return RUN_ALL_TESTS();
}