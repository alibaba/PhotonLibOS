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
#include <assert.h>
#include <photon/common/string_view.h>
extern "C" {
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>
}

namespace photon {

template<size_t LENGTH, const EVP_MD* (*MD)()>
struct Digest {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    EVP_MD_CTX* _ctx = EVP_MD_CTX_new();
    ~Digest() { EVP_MD_CTX_free(_ctx); }
#else
    EVP_MD_CTX ctx{}, *_ctx = &ctx;
    ~Digest() { EVP_MD_CTX_cleanup(_ctx); }
#endif
    Digest()  { EVP_DigestInit_ex(_ctx, MD(), NULL); }
    Digest(std::string_view data) : Digest() { update(data); }
    Digest(const void* data, size_t len) : Digest() { update(data, len); }
    void update(std::string_view data) { update(data.data(), data.size()); }
    void update(const void* data, size_t len) { EVP_DigestUpdate(_ctx, data, len); }
    enum { DIGEST_LENGTH = LENGTH };
    mutable char _md[LENGTH];
    mutable bool _filled = false;
    operator std::string_view() const {
        return finalize();
    }
    std::string_view finalize() const {
        if (!_filled)
             _filled = finalize(_md), true;
        return {_md, LENGTH};
    }
    void finalize(void* buf) {
        unsigned int len = 0; (void)len;
        EVP_DigestFinal_ex(_ctx, (unsigned char*)buf, &len);
        assert(len == DIGEST_LENGTH);
    }
};

using sha1   = Digest<SHA_DIGEST_LENGTH,    &EVP_sha1>;
using sha256 = Digest<SHA256_DIGEST_LENGTH, &EVP_sha256>;
using sha512 = Digest<SHA512_DIGEST_LENGTH, &EVP_sha512>;
using md5    = Digest<MD5_DIGEST_LENGTH,    &EVP_md5>;


template<size_t LENGTH, const EVP_MD* (*MD)()>
struct HMAC {
    char _md[LENGTH];
    enum { DIGEST_LENGTH = LENGTH };
    HMAC(std::string_view key, std::string_view data) : HMAC(key, data, _md) { }
    HMAC(std::string_view key, std::string_view data, char* md) {
        unsigned int _md_len; (void)_md_len;
        ::HMAC(MD(), key.data(), (int)key.size(),
            (const unsigned char*)data.data(), data.size(),
            (unsigned char*)md, &_md_len);
        assert(_md_len == LENGTH);
    }
    operator std::string_view() const {
        return {_md, LENGTH};
    }
    std::string_view finalize() const {
        return *this;
    }
};

using HMAC_SHA1   = HMAC<SHA_DIGEST_LENGTH,    EVP_sha1>;
using HMAC_SHA256 = HMAC<SHA256_DIGEST_LENGTH, EVP_sha256>;
using HMAC_SHA512 = HMAC<SHA512_DIGEST_LENGTH, EVP_sha512>;
}
