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

#include "crc32c.h"
#include "crc64ecma.h"
#include <stdlib.h>
#if defined(__linux__) && defined(__aarch64__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#include <photon/common/utility.h>
#include <photon/common/alog.h>

template<typename T, typename F1, typename F8> __attribute__((always_inline))
inline T do_crc(const uint8_t *data, size_t nbytes, T crc, F1 f1, F8 f8) {
    size_t offset = 0;
    // Process bytes one at a time until we reach an 8-byte boundary and can
    // start doing aligned 64-bit reads.
    static uintptr_t ALIGN_MASK = sizeof(uint64_t) - 1;
    size_t mask = (size_t)((uintptr_t)data & ALIGN_MASK);
    if (mask != 0) {
        size_t limit = std::min(nbytes, sizeof(uint64_t) - mask);
        while (offset < limit) {
            crc = f1(crc, data[offset]);
            offset++;
        }
    }

    // Process 8 bytes at a time until we have fewer than 8 bytes left.
    while (offset + sizeof(uint64_t) <= nbytes) {
        crc = f8(crc, *(uint64_t*)(data + offset));
        offset += sizeof(uint64_t);
    }
    // Process any bytes remaining after the last aligned 8-byte block.
    while (offset < nbytes) {
        crc = f1(crc, data[offset]);
        offset++;
    }
    return crc;
}

template<size_t begin, size_t end, ssize_t step, typename F,
        typename = typename std::enable_if<begin != end>::type>
inline __attribute__((always_inline))
void static_loop(const F& f) {
    f(begin);
    static_loop<begin + step, end, step>(f);
}

template<size_t begin, size_t end, ssize_t step, typename F, typename = void,
        typename = typename std::enable_if<begin == end>::type>
inline __attribute__((always_inline))
void static_loop(const F& f) {
    f(begin);
}

// gcc sometimes doesn't allow always_inline
#define BODY(i) [&](size_t i) /*__attribute__((always_inline))*/


#define CVAL(c, val) (-!!(c) & val)

const uint32_t CRC32C_POLY = 0x82f63b78;
const uint64_t CRC64ECMA_POLY = 0xc96c5795d7870f42;

template<typename T>
struct TableCRC {
    typedef T (*Table)[256];
    Table table = (Table) malloc(sizeof(*table) * 8);
    ~TableCRC() { free(table); }
    TableCRC(T POLY) {
        for (int n = 0; n < 256; n++) {
            T crc = n;
            static_loop<0, 7, 1>(BODY(k) {
                crc = CVAL(crc&1, POLY) ^ (crc >> 1);
            });
            table[0][n] = crc;
        }
        for (int n = 0; n < 256; n++) {
            T crc = table[0][n];
            static_loop<1, 7, 1>(BODY(k) {
                crc = table[0][crc & 0xff] ^ (crc >> 8);
                table[k][n] = crc;
            });
        }
    }
    __attribute__((always_inline))
    T operator()(const uint8_t *buffer, size_t nbytes, T crc) const {
        auto f1 = [&](T crc, uint8_t b) {
            return table[0][(crc ^ b) & 0xff] ^ (crc >> 8);
        };
        auto f8 = [&](T crc, uint64_t x) {
            x ^= crc; crc = 0;
            static_loop<0, 7, 1>(BODY(i) {
                crc ^= table[7-i][(x >> (i*8)) & 0xff];
            });
            return crc;
        };
        return do_crc(buffer, nbytes, crc, f1, f8);
    }
};

uint32_t crc32c_sw(const uint8_t *buffer, size_t nbytes, uint32_t crc) {
    const static TableCRC<uint32_t> calc(CRC32C_POLY);
    return calc(buffer, nbytes, crc);
}

uint64_t crc64ecma_sw(const uint8_t *buffer, size_t nbytes, uint64_t crc) {
    const static TableCRC<uint64_t> calc(CRC64ECMA_POLY);
    return ~calc(buffer, nbytes, ~crc);
}

uint64_t crc64ecma_hw_sse128(const uint8_t *buf, size_t len, uint64_t crc);
uint64_t crc64ecma_hw_avx512(const uint8_t *buf, size_t len, uint64_t crc);
uint32_t (*crc32c_auto)(const uint8_t*, size_t, uint32_t) = nullptr;
uint32_t (*crc32c_combine_auto)(uint32_t crc1, uint32_t crc2, uint32_t len2);
uint32_t (*crc32c_combine_series_auto)(uint32_t* crc, uint32_t part_size, uint32_t n_parts);
void (*crc32c_series_auto)(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint32_t* crc_parts);
uint64_t (*crc64ecma_auto)(const uint8_t *data, size_t nbytes, uint64_t crc);
uint64_t (*crc64ecma_combine_auto)(uint64_t crc1, uint64_t crc2, uint32_t len2);
uint64_t (*crc64ecma_combine_series_auto)(uint64_t* crc, uint32_t part_size, uint32_t n_parts);
void (*crc64ecma_series_auto)(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint64_t* crc_parts);
uint32_t (*crc32c_trim_auto)(CRC32C_Component all, CRC32C_Component prefix, CRC32C_Component suffix);
uint64_t (*crc64ecma_trim_auto)(CRC64ECMA_Component all, CRC64ECMA_Component prefix,CRC64ECMA_Component suffix);

__attribute__((constructor))
static void crc_init() {
    auto tie32 = std::tie(crc32c_auto, crc32c_series_auto, crc32c_combine_auto, crc32c_combine_series_auto, crc32c_trim_auto);
    auto hw32 = std::make_tuple(crc32c_hw, crc32c_series_hw, crc32c_combine_hw, crc32c_combine_series_hw, crc32c_trim_hw); (void)hw32;
    auto sw32 = std::make_tuple(crc32c_sw, crc32c_series_sw, crc32c_combine_sw, crc32c_combine_series_sw, crc32c_trim_sw); (void)sw32;
#if defined(__x86_64__)
    __builtin_cpu_init();
    tie32       = __builtin_cpu_supports("sse4.2") ? hw32 : sw32;
    auto avx512 = __builtin_cpu_supports("avx512f")  &&
                  __builtin_cpu_supports("avx512dq") &&
                  __builtin_cpu_supports("avx512vl") &&
                  __builtin_cpu_supports("vpclmulqdq");
    crc64ecma_auto = avx512 ? crc64ecma_hw_avx512 :
                 (__builtin_cpu_supports("sse") &&
                  __builtin_cpu_supports("pclmul")) ?
                        crc64ecma_hw_sse128 : crc64ecma_sw;
#elif defined(__aarch64__)
#ifdef __APPLE__  // apple silicon has hw for both crc
    tie32 = hw32;
    crc64ecma_auto = crc64ecma_hw_sse128;
#elif defined(__linux__)  // linux on arm: runtime detection
    long hwcaps= getauxval(AT_HWCAP);
    tie32 = (hwcaps & HWCAP_CRC32) ? hw32 : sw32;
    crc64ecma_auto = (hwcaps & HWCAP_PMULL) ? crc64ecma_hw_sse128 : crc64ecma_sw;
#else
    tie32 = sw32;
    crc64ecma_auto = crc64ecma_sw;
#endif
#else // not __aarch64__, not __x86_64__
    tie32 = sw32;
    crc64ecma_auto = crc64ecma_sw;
#endif
    crc64ecma_combine_auto = (crc64ecma_auto == crc64ecma_sw) ?
                              crc64ecma_combine_sw :
                              crc64ecma_combine_hw ;
    crc64ecma_trim_auto = (crc64ecma_auto == crc64ecma_sw) ?
                           crc64ecma_trim_sw :
                           crc64ecma_trim_hw ;
}

#ifdef __x86_64__
#include <immintrin.h>
#ifdef __clang__
#pragma clang attribute push (__attribute__((target("crc32,sse4.1,pclmul"))), apply_to=function)
#else // __GNUC__
#pragma GCC push_options
#pragma GCC target ("crc32,sse4.1,pclmul")
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Winit-self"
#endif
#elif defined(__aarch64__)
#if !defined(__clang__) && defined(__GNUC__) && (__GNUC__ < 10)
#undef __GNUC__
#define __GNUC__ 10
#endif
#define SSE2NEON_SUPPRESS_WARNINGS
#include "sse2neon.h"
#else
#error "Unsupported architecture"
#endif

struct SSE {
public:
    typedef __m128i v128;
    static v128 loadu(const void* ptr) {
        return _mm_loadu_si128((v128*)ptr);
    }
    static v128 pshufb(v128& x, const v128& y) {
        return (v128)_mm_shuffle_epi8((__m128i&)x, (const __m128i&)y);
    }
    static v128 pblendvb(v128& x, v128& y, v128& z) {
        return (v128)_mm_blendv_epi8((__m128i&)x, (__m128i&)y, (__m128i&)z);
    }
    template<uint8_t imm>
    static v128 pclmulqdq(v128 x, const uint64_t* rk) {
        return _mm_clmulepi64_si128(x, *(const v128*)rk, imm);
    }
    static v128 op(v128 x, const uint64_t* rk) {
        return pclmulqdq<0x10>(x, rk) ^ pclmulqdq<0x01>(x, rk);
    }
    #pragma GCC diagnostic ignored "-Wstrict-aliasing"
    static v128 load_small(const void* data, size_t n) {
        assert(n < 16);
        long x = 0;
        auto end = (const char*)data + n;
        if (n & 4) x = *--(const uint32_t*&)end;
        if (n & 2) x = (x<<16) | *--(const uint16_t*&)end ;
        if (n & 1) x = (x <<8) | *--(const uint8_t *&)end ;
        return (n & 8) ? v128{*(long*)data, x} : v128{x, 0};
    }
    static v128 bsl8(v128 x) {
        return _mm_bslli_si128(x, 8);
    }
    static v128 bsr8(v128 x) {
        return _mm_bsrli_si128(x, 8);
    }
};

#if (defined(__aarch64__) && defined(__ARM_FEATURE_CRC32))
#if (defined(__clang__))
inline uint32_t crc32c(uint32_t crc, uint8_t data) {
    return __builtin_arm_crc32cb(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint16_t data) {
    return __builtin_arm_crc32ch(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint32_t data) {
    return __builtin_arm_crc32cw(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint64_t data) {
    return (uint32_t) __builtin_arm_crc32cd(crc, data);
}
#else
inline uint32_t crc32c(uint32_t crc, uint8_t data) {
    return __builtin_aarch64_crc32cb(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint16_t data) {
    return __builtin_aarch64_crc32ch(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint32_t data) {
    return __builtin_aarch64_crc32cw(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint64_t data) {
    return (uint32_t) __builtin_aarch64_crc32cx(crc, data);
}
#endif
#elif (defined(__aarch64__))
inline uint32_t crc32c(uint32_t crc, uint8_t value) {
    __asm__("crc32cb %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value));
    return crc;
}
inline uint32_t crc32c(uint32_t crc, uint16_t value) {
    __asm__("crc32ch %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value));
    return crc;
}
inline uint32_t crc32c(uint32_t crc, uint32_t value) {
    __asm__("crc32cw %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value));
    return crc;
}
inline uint32_t crc32c(uint32_t crc, uint64_t value) {
    __asm__("crc32cx %w[c], %w[c], %x[v]":[c]"+r"(crc):[v]"r"(value));
    return crc;
}
#else
inline uint32_t crc32c(uint32_t crc, uint8_t data) {
    return __builtin_ia32_crc32qi(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint16_t data) {
    return __builtin_ia32_crc32hi(crc, data);
}
inline uint32_t crc32c(uint32_t crc, uint32_t data) {
    return __builtin_ia32_crc32si(crc, data);
}
inline uint32_t crc32c(uint32_t crc, const uint64_t& data) {
    // not using the built-in intrinsic to
    // avoid an extra 64-bit to 32-bit conversion
    asm volatile ("crc32q %1, %q0" : "+r"(crc) : "rm"(data));
    return crc;
}
#endif

__attribute__((aligned(16), used))
static const uint64_t crc32_merge_table_pclmulqdq[128*2] = {
    0x14cd00bd6, 0x105ec76f0,
    0x0ba4fc28e, 0x14cd00bd6,
    0x1d82c63da, 0x0f20c0dfe,
    0x09e4addf8, 0x0ba4fc28e,
    0x039d3b296, 0x1384aa63a,
    0x102f9b8a2, 0x1d82c63da,
    0x14237f5e6, 0x01c291d04,
    0x00d3b6092, 0x09e4addf8,
    0x0c96cfdc0, 0x0740eef02,
    0x18266e456, 0x039d3b296,
    0x0daece73e, 0x0083a6eec,
    0x0ab7aff2a, 0x102f9b8a2,
    0x1248ea574, 0x1c1733996,
    0x083348832, 0x14237f5e6,
    0x12c743124, 0x02ad91c30,
    0x0b9e02b86, 0x00d3b6092,
    0x018b33a4e, 0x06992cea2,
    0x1b331e26a, 0x0c96cfdc0,
    0x17d35ba46, 0x07e908048,
    0x1bf2e8b8a, 0x18266e456,
    0x1a3e0968a, 0x11ed1f9d8,
    0x0ce7f39f4, 0x0daece73e,
    0x061d82e56, 0x0f1d0f55e,
    0x0d270f1a2, 0x0ab7aff2a,
    0x1c3f5f66c, 0x0a87ab8a8,
    0x12ed0daac, 0x1248ea574,
    0x065863b64, 0x08462d800,
    0x11eef4f8e, 0x083348832,
    0x1ee54f54c, 0x071d111a8,
    0x0b3e32c28, 0x12c743124,
    0x0064f7f26, 0x0ffd852c6,
    0x0dd7e3b0c, 0x0b9e02b86,
    0x0f285651c, 0x0dcb17aa4,
    0x010746f3c, 0x018b33a4e,
    0x1c24afea4, 0x0f37c5aee,
    0x0271d9844, 0x1b331e26a,
    0x08e766a0c, 0x06051d5a2,
    0x093a5f730, 0x17d35ba46,
    0x06cb08e5c, 0x11d5ca20e,
    0x06b749fb2, 0x1bf2e8b8a,
    0x1167f94f2, 0x021f3d99c,
    0x0cec3662e, 0x1a3e0968a,
    0x19329634a, 0x08f158014,
    0x0e6fc4e6a, 0x0ce7f39f4,
    0x08227bb8a, 0x1a5e82106,
    0x0b0cd4768, 0x061d82e56,
    0x13c2b89c4, 0x188815ab2,
    0x0d7a4825c, 0x0d270f1a2,
    0x10f5ff2ba, 0x105405f3e,
    0x00167d312, 0x1c3f5f66c,
    0x0f6076544, 0x0e9adf796,
    0x026f6a60a, 0x12ed0daac,
    0x1a2adb74e, 0x096638b34,
    0x19d34af3a, 0x065863b64,
    0x049c3cc9c, 0x1e50585a0,
    0x068bce87a, 0x11eef4f8e,
    0x1524fa6c6, 0x19f1c69dc,
    0x16cba8aca, 0x1ee54f54c,
    0x042d98888, 0x12913343e,
    0x1329d9f7e, 0x0b3e32c28,
    0x1b1c69528, 0x088f25a3a,
    0x02178513a, 0x0064f7f26,
    0x0e0ac139e, 0x04e36f0b0,
    0x0170076fa, 0x0dd7e3b0c,
    0x141a1a2e2, 0x0bd6f81f8,
    0x16ad828b4, 0x0f285651c,
    0x041d17b64, 0x19425cbba,
    0x1fae1cc66, 0x010746f3c,
    0x1a75b4b00, 0x18db37e8a,
    0x0f872e54c, 0x1c24afea4,
    0x01e41e9fc, 0x04c144932,
    0x086d8e4d2, 0x0271d9844,
    0x160f7af7a, 0x052148f02,
    0x05bb8f1bc, 0x08e766a0c,
    0x0a90fd27a, 0x0a3c6f37a,
    0x0b3af077a, 0x093a5f730,
    0x04984d782, 0x1d22c238e,
    0x0ca6ef3ac, 0x06cb08e5c,
    0x0234e0b26, 0x063ded06a,
    0x1d88abd4a, 0x06b749fb2,
    0x04597456a, 0x04d56973c,
    0x0e9e28eb4, 0x1167f94f2,
    0x07b3ff57a, 0x19385bf2e,
    0x0c9c8b782, 0x0cec3662e,
    0x13a9cba9e, 0x0e417f38a,
    0x093e106a4, 0x19329634a,
    0x167001a9c, 0x14e727980,
    0x1ddffc5d4, 0x0e6fc4e6a,
    0x00df04680, 0x0d104b8fc,
    0x02342001e, 0x08227bb8a,
    0x00a2a8d7e, 0x05b397730,
    0x168763fa6, 0x0b0cd4768,
    0x1ed5a407a, 0x0e78eb416,
    0x0d2c3ed1a, 0x13c2b89c4,
    0x0995a5724, 0x1641378f0,
    0x19b1afbc4, 0x0d7a4825c,
    0x109ffedc0, 0x08d96551c,
    0x0f2271e60, 0x10f5ff2ba,
    0x00b0bf8ca, 0x00bf80dd2,
    0x123888b7a, 0x00167d312,
    0x1e888f7dc, 0x18dcddd1c,
    0x002ee03b2, 0x0f6076544,
    0x183e8d8fe, 0x06a45d2b2,
    0x133d7a042, 0x026f6a60a,
    0x116b0f50c, 0x1dd3e10e8,
    0x05fabe670, 0x1a2adb74e,
    0x130004488, 0x0de87806c,
    0x000bcf5f6, 0x19d34af3a,
    0x18f0c7078, 0x014338754,
    0x017f27698, 0x049c3cc9c,
    0x058ca5f00, 0x15e3e77ee,
    0x1af900c24, 0x068bce87a,
    0x0b5cfca28, 0x0dd07448e,
    0x0ded288f8, 0x1524fa6c6,
    0x059f229bc, 0x1d8048348,
    0x06d390dec, 0x16cba8aca,
    0x037170390, 0x0a3e3e02c,
    0x06353c1cc, 0x042d98888,
    0x0c4584f5c, 0x0d73c7bea,
    0x1f16a3418, 0x1329d9f7e,
    0x0531377e2, 0x185137662,
    0x1d8d9ca7c, 0x1b1c69528,
    0x0b25b29f2, 0x18a08b5bc,
    0x19fb2a8b0, 0x02178513a,
    0x1a08fe6ac, 0x1da758ae0,
    0x045cddf4e, 0x0e0ac139e,
    0x1a91647f2, 0x169cf9eb0,
    0x1a0f717c4, 0x0170076fa,
};

template<size_t blksz, typename T> inline __attribute__((always_inline))
void crc32c_hw_block(const uint8_t*& data, size_t& nbytes, uint32_t& crc) {
    if (nbytes & blksz) {
        static_loop<0, blksz - sizeof(T), sizeof(T)>(BODY(i) {
            crc = crc32c(crc, *(T*)(data + i));
        });
        nbytes -= blksz;
        data += blksz;
    }
}

inline __attribute__((always_inline))
void crc32c_hw_tiny(uint64_t d, size_t nbytes, uint32_t& crc) {
    assert(nbytes <= 7);
    if (nbytes & 1) { crc = crc32c(crc, (uint8_t)d); d >>= 8; }
    if (nbytes & 2) { crc = crc32c(crc, (uint16_t)d); d >>= 16; }
    if (nbytes & 4) { crc = crc32c(crc, (uint32_t)d); }
}

inline __attribute__((always_inline))
void crc32c_hw_small(const uint8_t*& data, size_t nbytes, uint32_t& crc) {
    assert(nbytes < 256);
    if (unlikely(!nbytes || !data)) return;
    crc32c_hw_block<128, uint64_t>(data, nbytes, crc);
    crc32c_hw_block<64,  uint64_t>(data, nbytes, crc);
    crc32c_hw_block<32,  uint64_t>(data, nbytes, crc);
    crc32c_hw_block<16,  uint64_t>(data, nbytes, crc);
    crc32c_hw_block<8,   uint64_t>(data, nbytes, crc);
    if (unlikely(nbytes)) {
        auto x = 8 - nbytes; // buffer size >= 8
        auto d = *(uint64_t*)(data - x);
        d >>= x * 8;
        data += nbytes;
        crc32c_hw_tiny(d, nbytes, crc);
    }
}

template<uint16_t blksz> inline __attribute__((always_inline))
bool crc32c_3way_ILP(const uint8_t*& data, size_t& nbytes, uint32_t& crc) {
    if (nbytes < blksz * 3) return false;
    auto ptr = (const uint64_t*)data;
    const size_t blksz_8 = blksz / 8;
    uint32_t crc1 = 0, crc2 = 0;
    static_loop<0, blksz_8 - 2, 1>(BODY(i) {
        if (i < blksz_8 * 3 / 4)
            __builtin_prefetch(data + blksz*3 + i*8*4, 0, 0);
        crc  = crc32c(crc,  ptr[i]);
        crc1 = crc32c(crc1, ptr[i + blksz_8]);
        crc2 = crc32c(crc2, ptr[i + blksz_8 * 2]);
    });
    crc = crc32c(crc, ptr[blksz_8 - 1]);
    crc1 = crc32c(crc1, ptr[blksz_8 * 2 - 1]);
    // crc2 = crc32c(crc2, ptr[blksz_8 * 3 - 1]);

    static_assert((blksz/8 - 1)*2 + 1< LEN(crc32_merge_table_pclmulqdq), "...");
    auto k = &crc32_merge_table_pclmulqdq[(blksz/8 - 1)*2];
    __m128i c0 = {(long)crc}, c1 = {(long)crc1};
    auto t = SSE::pclmulqdq<0x00>(c0, k) ^
             SSE::pclmulqdq<0x10>(c1, k) ;
    crc = crc32c(crc2, ptr[blksz_8 * 3 - 1] ^ (uint64_t&)t);
    data += blksz * 3;
    nbytes -= blksz * 3;
    return true;
}

// This is a portable re-imlementation of crc32_iscsi_00()
// in pure x86_64 assembly in ISA-L. It is as fast as the
// latter one for both small and big data blocks. And it is
// event faster than the ARMv8 counterpart in ISA-L, i.e.
// crc32_iscsi_crc_ext().
uint32_t crc32c_hw_portable(const uint8_t *data, size_t nbytes, uint32_t crc) {
    if (unlikely(!nbytes)) return crc;
    if (unlikely(nbytes < 8)) {
        while(nbytes--)
            crc = crc32c(crc, *data++);
        return crc;
    }
    uint8_t l = (~((uint64_t)data) + 1) & 7;
    if (unlikely(l)) {
        auto d = *(uint64_t*)data; // nbytes >= 8
        data += l; nbytes -= l;
        crc32c_hw_tiny(d, l, crc);
    }
    while(crc32c_3way_ILP<512>(data, nbytes, crc));
    crc32c_3way_ILP<256>(data, nbytes, crc);
    crc32c_3way_ILP<128>(data, nbytes, crc);
    crc32c_3way_ILP<64 >(data, nbytes, crc);
    crc32c_hw_small(data, nbytes, crc);
    return crc;
}

uint32_t crc32c_hw_simple(const uint8_t *data, size_t nbytes, uint32_t crc) {
    auto f1 = [](uint32_t crc, uint8_t x)  { return crc32c(crc, x); };
    auto f8 = [](uint32_t crc, uint64_t x) { return crc32c(crc, x); };
    return do_crc(data, nbytes, crc, f1, f8);
}

uint32_t crc32c_hw(const uint8_t *data, size_t nbytes, uint32_t crc) {
    return crc32c_hw_portable(data, nbytes, crc);
}

const static uint32_t crc32c_lshift_table_hw[] = {
    // by length of 16, 32, ..., 2G bytes
    0x493c7d27, 0xba4fc28e, 0x9e4addf8, 0x0d3b6092,
    0xb9e02b86, 0xdd7e3b0c, 0x170076fa, 0xa51b6135,
    0x82f89c77, 0x54a86326, 0x1dc403cc, 0x5ae703ab,
    0xc5013a36, 0xac2ac6dd, 0x9b4615a9, 0x688d1c61,
    0xf6af14e6, 0xb6ffe386, 0xb717425b, 0x478b0d30,
    0x54cc62e5, 0x7b2102ee, 0x8a99adef, 0xa7568c8f,
    0xd610d67e, 0x6b086b3f, 0xd94f3c0b, 0xbf818109,
};

const static uint32_t crc32c_rshift_table_hw[32] = {
    0xa738873b, 0x616f3095, 0xc915ea3b, 0x77f5096b,
    0x55953016, 0x8d6b52b5, 0x0e6e0c8a, 0xd6667510,
    0xbe7257f3, 0xa319ac44, 0x095a58f0, 0x7bea9ec2,
    0xca41fd90, 0x1ef45b26, 0xfd76e8a2, 0xc6a704fd,
    0x0a9bdf61, 0xbd5060e9, 0x0d4e18dd, 0xf58257a3,
    0x697849dd, 0xec59259e, 0x97cbee2c, 0x74a64152,
    0x64c0d836, 0xb9280158, 0xac2fda6b, 0x4b416fef,
    0x5677c2eb, 0xacef85d6, 0xb866faba, 0xa738873b,
};

const static uint32_t crc32c_lshift_table_sw[32] = {
    0x00800000, 0x00008000, 0x82f63b78, 0x6ea2d55c,
    0x18b8ea18, 0x510ac59a, 0xb82be955, 0xb8fdb1e7,
    0x88e56f72, 0x74c360a4, 0xe4172b16, 0x0d65762a,
    0x35d73a62, 0x28461564, 0xbf455269, 0xe2ea32dc,
    0xfe7740e6, 0xf946610b, 0x3c204f8f, 0x538586e3,
    0x59726915, 0x734d5309, 0xbc1ac763, 0x7d0722cc,
    0xd289cabe, 0xe94ca9bc, 0x05b74f3f, 0xa51e1f42,
    0x40000000, 0x20000000, 0x08000000, 0x00800000,
};

const static uint32_t crc32c_rshift_table_sw[32] = {
    0xfde39562, 0xbef0965e, 0xd610d67e, 0xe67cce65,
    0xa268b79e, 0x134fb088, 0x32998d96, 0xcedac2cc,
    0x70118575, 0x0e004a40, 0xa7864c8b, 0xbc7be916,
    0x10ba2894, 0x6077197b, 0x98448e4e, 0x8baf845d,
    0xe93e07fc, 0xf58027d7, 0x5e2b422d, 0x9db2851c,
    0x9270ed25, 0x5984e7b3, 0x7af026f1, 0xe0f4116b,
    0xace8a6b0, 0x9e09f006, 0x6a60ea71, 0x4fd04875,
    0x05ec76f1, 0x0bd8ede2, 0x2f63b788, 0xfde39562,
};

// *virtually* pad or remove `len` bytes of trailing 0s
// to source data, and return resulting crc value
template<typename T> inline
T crc_apply_shifts(T crc, uint32_t len, const T* table,
                   T (*apply_shift)(T, T)) {
    for (; len; len &= len - 1) {
        auto x = table[__builtin_ctzll(len)];
        crc = apply_shift(crc, x);
    }
    return crc;
}

static uint32_t clmul_modp_crc32c_hw(uint32_t crc1, uint32_t x) {
    uint64_t dat64;
    __m128i crc1x, cnstx;
    crc1x = _mm_setr_epi32(crc1, 0, 0, 0);
    cnstx = _mm_setr_epi32(x, 0, 0, 0);
    crc1x = _mm_clmulepi64_si128(crc1x, cnstx, 0);
    dat64 = _mm_cvtsi128_si64(crc1x);
    return crc32c((uint32_t)0, dat64);
}

uint32_t crc32c_combine_hw(uint32_t crc1, uint32_t crc2, uint32_t len2) {
    if (unlikely(!crc1)) return crc2;
    if (unlikely(!len2)) return crc1;
    if (unlikely(len2 & 15)) {
        if (unlikely(len2 & 8)) crc1 = crc32c(crc1, (uint64_t)0);
        if (unlikely(len2 & 4)) crc1 = crc32c(crc1, (uint32_t)0);
        if (unlikely(len2 & 2)) crc1 = crc32c(crc1, (uint16_t)0);
        if (unlikely(len2 & 1)) crc1 = crc32c(crc1, (uint8_t)0);
    }
    crc1 = crc_apply_shifts(crc1, len2>>4,
        crc32c_lshift_table_hw, clmul_modp_crc32c_hw);
    return crc1 ^ crc2;
}

uint32_t crc32c_combine_series_hw(uint32_t* crc, uint32_t part_size, uint32_t n_parts) {
    if (unlikely(!n_parts)) return 0;
    auto res = crc[0];
    for (uint32_t i = 1; i < n_parts; ++i)
        res = crc32c_combine_hw(res, crc[i], part_size);
    return res;
}

// (a*b) % poly
template<typename T, T POLY> inline
T clmul_modp_sw(T a, T b) {
    T pd = 0;
    for(uint64_t i = 0; i < sizeof(T)*8; i++, b>>=1)
        pd = (pd>>1) ^ CVAL(pd&1, POLY) ^ CVAL(b&1, a);
    return pd;
}

uint32_t crc32c_combine_sw(uint32_t crc1, uint32_t crc2, uint32_t len2) {
    if (unlikely(!crc1)) return crc2;
    if (unlikely(!len2)) return crc1;
    crc1 = crc_apply_shifts(crc1, len2, crc32c_lshift_table_sw,
            &clmul_modp_sw<uint32_t, CRC32C_POLY>);
    return crc1 ^ crc2;
}

static uint32_t crc32c_rshift_hw(uint32_t crc, size_t len) {
    return crc_apply_shifts(crc, len, crc32c_rshift_table_hw,
            &clmul_modp_crc32c_hw);
}

static uint32_t crc32c_rshift_sw(uint32_t crc, size_t len) {
    return crc_apply_shifts(crc, len, crc32c_rshift_table_sw,
            &clmul_modp_sw<uint32_t, CRC32C_POLY>);
}

template<typename T, typename F1, typename F2> static inline
auto do_crc_trim(T all, T prefix, T suffix, F1 rm_prefix, F2 rm_suffix) -> decltype(all.crc) {
    if (all.size < prefix.size + suffix.size)
        LOG_ERRNO_RETURN(EINVAL, 0, "total size (`) must be > summed sizes of prefix (`) + suffix (`)", all.size, prefix.size, suffix.size);
    if (unlikely(!prefix.size && !suffix.size))
        return all.crc;
    auto crc = all.crc;
    if (prefix.size)
        // crc ^= prefix.crc << (all.size - prefix.size);
        crc = rm_prefix(prefix.crc, crc, all.size - prefix.size);
    if (suffix.size)
        // crc = (crc ^ suffix.crc) >> suffix.size;
        crc = rm_suffix(crc ^ suffix.crc, suffix.size);
    return crc;
}

uint32_t crc32c_trim_sw(CRC32C_Component all, CRC32C_Component prefix, CRC32C_Component suffix) {
    return do_crc_trim(all, prefix, suffix, &crc32c_combine_sw, &crc32c_rshift_sw);
}

uint32_t crc32c_trim_hw(CRC32C_Component all, CRC32C_Component prefix, CRC32C_Component suffix) {
    return do_crc_trim(all, prefix, suffix, &crc32c_combine_hw, &crc32c_rshift_hw);
}

uint32_t crc32c_combine_series_sw(uint32_t* crc, uint32_t part_size, uint32_t n_parts) {
    if (unlikely(!n_parts)) return 0;
    auto res = crc[0];
    for (uint32_t i = 1; i < n_parts; ++i)
        res = crc32c_combine_sw(res, crc[i], part_size);
    return res;
}

void crc32c_series_sw(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint32_t* crc_parts) {
    for (uint32_t i = 0; i < n_parts; ++i)
        crc_parts[i] = crc32c_sw(buffer + i * part_size, part_size, 0);
}

void crc32c_series_hw(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint32_t* crc_parts) {
    const size_t BATCH = 4;
    auto part_main = part_size / 8 * 8;
    auto part_remain = part_size % 8;
    auto batch_crc = [&](auto ptr, size_t batch) __attribute__((always_inline)) {
        #pragma GCC unroll 4
        for (size_t k = 0; k < batch; ++k) {
            crc_parts[k] = crc32c(crc_parts[k], *ptr);
            ptr = decltype(ptr)((char*)ptr + part_size);
        }
    };
    auto batch_blocks_crc = [&](const uint8_t* ptr, auto batch) __attribute__((always_inline)) {
        for (size_t j = 0; j < batch; ++j)
            crc_parts[j] = 0;
        for (; ptr < buffer + part_main; ptr += 8) {
            batch_crc((uint64_t*)ptr, batch);
        }
        if (unlikely(part_main)) {
            if (part_remain & 4) { batch_crc((uint32_t*)ptr, batch); ptr += 4; }
            if (part_remain & 2) { batch_crc((uint16_t*)ptr, batch); ptr += 2; }
            if (part_remain & 1) { batch_crc((uint8_t*)ptr, batch); ptr += 1; }
        }
    };
    for (; n_parts >= BATCH; n_parts -= BATCH) {
        batch_blocks_crc(buffer, BATCH);
        buffer += part_size * BATCH;
        crc_parts += BATCH;
    }
    if (unlikely(n_parts))
        batch_blocks_crc(buffer, n_parts);
}

// rk1 ~ rk20
__attribute__((aligned(16), used))
const static uint64_t rk[20] = {
    0xdabe95afc7875f40,
    0xe05dd497ca393ae4,
    0xd7d86b2af73de740,
    0x8757d71d4fcc1000,
    0xdabe95afc7875f40,
    0x0000000000000000,
    0x9c3e466c172963d5,
    0x92d8af2baf0e1e84,
    0x947874de595052cb,
    0x9e735cb59b4724da,
    0xe4ce2cd55fea0037,
    0x2fe3fd2920ce82ec,
    0x0e31d519421a63a5,
    0x2e30203212cac325,
    0x081f6054a7842df4,
    0x6ae3efbb9dd441f3,
    0x69a35d91c3730254,
    0xb5ea1af9c013aca4,
    0x3be653a30fe1af51,
    0x60095b008a9efa44,
};

#define RK(i) &rk[i-1]

__attribute__((aligned(16), used))
const static uint64_t mask[6] = {
    0xFFFFFFFFFFFFFFFF, 0x0000000000000000,
    0xFFFFFFFF00000000, 0xFFFFFFFFFFFFFFFF,
    0x8080808080808080, 0x8080808080808080,
};

#define MASK(i) ({auto p = &mask[((i)-1)*2]; *(v128*)p;})

const static uint64_t pshufb_shf_table[4] = {
    0x8786858483828100, 0x8f8e8d8c8b8a8988,
    0x0706050403020100, 0x000e0d0c0b0a0908};

inline void* get_shf_table(size_t i) {
    return (char*)pshufb_shf_table + i;
}

inline __attribute__((always_inline))
__m128i crc64ecma_hw_big_sse(const uint8_t*& data, size_t& nbytes, uint64_t crc) {
    using SIMD = SSE;
    using v128 = typename SIMD::v128;
    v128 xmm[8];
    auto& ptr = (const v128*&)data;
    static_loop<0, 7, 1>(BODY(i){ xmm[i] = SIMD::loadu(ptr+i); });
    xmm[0] ^= v128{(long)~crc, 0}; ptr += 8; nbytes -= 128;
    do {
        static_loop<0, 7, 1>(BODY(i) {
            xmm[i] = SIMD::op(xmm[i], RK(3)) ^ SIMD::loadu(ptr+i);
        });
        ptr += 8; nbytes -= 128;
    } while (nbytes >= 128);
    static_loop<0, 6, 1>(BODY(i) {
        auto I = (i == 6) ? 1 : (9 + i * 2);
        xmm[7] ^= SIMD::op(xmm[i], RK(I));
    });
    return xmm[7];
}

template<typename F> inline __attribute__((always_inline))
uint64_t crc64ecma_hw_portable(const uint8_t *data, size_t nbytes, uint64_t crc, F hw_big) {
    if (unlikely(!nbytes || !data)) return crc;
    using SIMD = SSE;
    using v128 = typename SIMD::v128;
    v128 xmm7 = {(long)~crc};
    auto& ptr = (const v128*&)data;
    if (nbytes >= 256) {
        xmm7 = hw_big(data, nbytes, crc);
    } else if (nbytes >= 16) {
        xmm7 ^= SIMD::loadu(ptr++);
        nbytes -= 16;
    } else /* 0 < nbytes < 16*/ {
        xmm7 ^= SIMD::load_small(data, nbytes);
        if (nbytes >= 8) {
            auto shf = SIMD::loadu(get_shf_table(nbytes));
            xmm7 = SIMD::pshufb(xmm7, shf);
            goto _128_done;
        } else {
            auto shf = SIMD::loadu(get_shf_table(nbytes + 8));
            xmm7 = SIMD::pshufb(xmm7, shf);
            goto _barrett;
        }
    }

    while (nbytes >= 16) {
        xmm7 = SIMD::op(xmm7, RK(1)) ^ SIMD::loadu(ptr++);
        nbytes -= 16;
    }

    if (nbytes) {
        auto p = data + nbytes - 16;
        auto remainder = SIMD::loadu((v128*)p);
        auto xmm0 = SIMD::loadu(get_shf_table(nbytes));
        auto xmm2 = xmm7;
        xmm7 = SIMD::pshufb(xmm7, xmm0);
        xmm0 ^= MASK(3);
        xmm2 = SIMD::pshufb(xmm2, xmm0);
        xmm2 = SIMD::pblendvb(xmm2, remainder, xmm0);
        xmm7 = xmm2 ^ SIMD::op(xmm7, RK(1));
    }
_128_done:
    xmm7  =  SIMD::pclmulqdq<0>(xmm7, RK(5)) ^ SIMD::bsr8(xmm7);
_barrett:
    auto t = SIMD::pclmulqdq<0>(xmm7, RK(7));
    xmm7  ^= SIMD::pclmulqdq<0x10>(t, RK(7)) ^ SIMD::bsl8(t);
    auto p = (uint64_t*)&xmm7;
    crc = ~p[1];
    return crc;
}

uint64_t crc64ecma_hw_sse128(const uint8_t *buf, size_t len, uint64_t crc) {
    return crc64ecma_hw_portable(buf, len, crc, crc64ecma_hw_big_sse);
}

const static uint64_t crc64ecma_lshift_table[] = {
    // by length of 1, 2, 4, 8, ..., 4G bytes
    0x0100000000000000, 0x0001000000000000, 0x0000000100000000, 0x0000000000000001,
    0xdabe95afc7875f40, 0x3be653a30fe1af51, 0x081f6054a7842df4, 0xd7d86b2af73de740,
    0xf31fd9271e228b79, 0x430af18f45bfec70, 0xdb77241e49bab9e4, 0xf42819f9b69abd6a,
    0x8366e0bd97880af6, 0x3a0dc386f69b9d51, 0x6537db4df869f6e6, 0xa349f9a86a172f2e,
    0x629f50ac6fdf5d3a, 0xcb3c521f853fb4a1, 0x6d1de95abb95074b, 0x9172d2fcc1985c9c,
    0xc996d644b4a25645, 0x7d4a8a9b19cb8376, 0xbd09c74d8e1fd5e2, 0xb3223c4776751d27,
    0x57f0d333946ab755, 0x4b2c7a963a1e77c7, 0x78ff6b0f1d73d8c7, 0x740d65cdcd060ca9,
    0xc7f7a66770f6ce2e, 0xe8195ee90ae40993, 0xd9443a2e9c8cb27f, 0x17a88be197b6abdc,
    0x6975abb7ef289b8f,
};

static uint64_t clmul_modp_crc64ecma_hw(uint64_t crc, uint64_t x) {
    __m128i crc1x, crc2x, crc3x, constx;
    const __m128i rk7 = _mm_load_si128((__m128i*)&rk[7-1]);

    crc1x = _mm_cvtsi64_si128(crc);
    constx = _mm_cvtsi64_si128(x);
    crc1x = _mm_clmulepi64_si128(crc1x, constx, 0x00);

    // Barrett Reduction
    crc2x = _mm_clmulepi64_si128(crc1x, rk7, 0x00);
    crc3x = _mm_clmulepi64_si128(crc2x, rk7, 0x10);
    crc2x = _mm_bslli_si128(crc2x, 8);
    crc1x = _mm_xor_si128(crc1x, crc2x);
    crc1x = _mm_xor_si128(crc1x, crc3x);
    return _mm_extract_epi64(crc1x, 1);
}

uint64_t crc64ecma_combine_hw(uint64_t crc1, uint64_t crc2, uint32_t len2) {
    if (unlikely(!crc1)) return crc2;
    return crc2 ^ crc_apply_shifts(crc1, len2,
        crc64ecma_lshift_table, clmul_modp_crc64ecma_hw);
}

// (a*b) % poly
inline uint64_t clmul_modp64_sw(uint64_t a, uint64_t b) {
    uint64_t pd = 0;
    for(uint64_t i = 0; i <= sizeof(a)*8; i++, b>>=1)
        pd = (pd>>1) ^ CVAL(pd&1, CRC64ECMA_POLY) ^ CVAL(b&1, a);
    return pd;
}

uint64_t crc64ecma_combine_sw(uint64_t crc1, uint64_t crc2, uint32_t len2) {
    if (unlikely(!crc1)) return crc2;
    return crc2 ^ crc_apply_shifts(crc1, len2,
        crc64ecma_lshift_table, &clmul_modp64_sw);
}

const static uint64_t crc64ecma_rshift_table[] = {
    // by length of 1, 2, 4, 8, ..., 2G bytes
    0x5ddb47907c2b5ccd, 0x7906d53a625e2c59, 0xa7b7c93241ea6d5e, 0x11be34289ea15e0e,
    0xa75238d8d774465b, 0xcc05486166e62168, 0x4d73bcf10b5b97ba, 0x6404c4fc40140811,
    0x3c86d8103585616e, 0x4c9d5de80eb3fc42, 0xe621d2c2e1ea38e0, 0x4c015f25c72db7fd,
    0xb0298500527ffc22, 0x841f4ee73d4ee903, 0x52cb8880eee95001, 0xde1c06e1ab074f45,
    0xc403fa5c546155d8, 0x2a6d846bc92a4dcb, 0x740e9a93dabbe1c1, 0x3f2714f434a6183d,
    0x0d2ebca5c42743f8, 0xd344df0a66ee91cc, 0xa240548d7eec1c14, 0x7bb3c2d579a0539d,
    0xf30e741ac683fa8d, 0xc5a6159c1d582d1a, 0x3521482ae18cfce3, 0x620bf133c27942a9,
    0x55f28cebfe897757, 0xdbc399e528817a2f, 0xa60805d4db63ffdd, 0x43541bc2d03d5ebf,
};

static uint64_t crc64ecma_rshift_hw(uint64_t crc, uint64_t n) {
    return crc_apply_shifts(crc, n, crc64ecma_rshift_table,
        clmul_modp_crc64ecma_hw);
}

static uint64_t crc64ecma_rshift_sw(uint64_t crc, uint64_t n) {
    return crc_apply_shifts(crc, n, crc64ecma_rshift_table,
        &clmul_modp64_sw);
}

uint64_t crc64ecma_trim_hw(CRC64ECMA_Component all,
                           CRC64ECMA_Component prefix,
                           CRC64ECMA_Component suffix) {
    return do_crc_trim(all, prefix, suffix, &crc64ecma_combine_hw, &crc64ecma_rshift_hw);
}

uint64_t crc64ecma_trim_sw(CRC64ECMA_Component all,
                           CRC64ECMA_Component prefix,
                           CRC64ECMA_Component suffix) {
    return do_crc_trim(all, prefix, suffix, &crc64ecma_combine_sw, &crc64ecma_rshift_sw);
}


#ifdef __x86_64__
#ifdef __clang__
#pragma clang attribute pop
#pragma clang attribute push (__attribute__((target("crc32,sse4.1,pclmul,avx512f,avx512dq,avx512vl,vpclmulqdq"))), apply_to=function)
#else // __GNUC__
#pragma GCC push_options
#pragma GCC target ("crc32,sse4.1,pclmul,avx512f,avx512dq,avx512vl,vpclmulqdq")
#endif
static const uint64_t rk512[] = {
    0xf31fd9271e228b79, // rk_1
    0x8260adf2381ad81c, // rk_2
    0xdabe95afc7875f40, // rk1
    0xe05dd497ca393ae4, // rk2
    0xd7d86b2af73de740, // rk3
    0x8757d71d4fcc1000, // rk4
    0xdabe95afc7875f40,
    0x0000000000000000,
    0x9c3e466c172963d5,
    0x92d8af2baf0e1e84,
    0x947874de595052cb,
    0x9e735cb59b4724da,
    0xe4ce2cd55fea0037,
    0x2fe3fd2920ce82ec,
    0x0e31d519421a63a5,
    0x2e30203212cac325,
    0x081f6054a7842df4,
    0x6ae3efbb9dd441f3,
    0x69a35d91c3730254,
    0xb5ea1af9c013aca4,
    0x3be653a30fe1af51,
    0x60095b008a9efa44, // rk20
    0xdabe95afc7875f40, // rk_1b
    0xe05dd497ca393ae4, // rk_2b
    0x0000000000000000,
    0x0000000000000000,
};
#define _RK(i) &rk512[(i)+1]

using v512 = __m512i_u;
inline __attribute__((always_inline))
v512 OP(v512 a, v512 b, v512 c) {
    auto x = _mm512_clmulepi64_epi128((a), (b), 0x01);
    auto y = _mm512_clmulepi64_epi128((a), (b), 0x10);
    return   _mm512_ternarylogic_epi64(x, y, (c), 0x96);
};

inline __attribute__((always_inline))
v512 OP(v512 a, v512 b, const v512* c) {
    return OP(a, b, _mm512_loadu_si512(c));
}

inline __attribute__((always_inline))
__m128i crc64ecma_hw_big_avx512(const uint8_t*& data, size_t& nbytes, uint64_t crc) {
    assert(nbytes >= 256);
    __attribute__((aligned(16)))
    v512 crc0 = {(long)~crc};
    auto& ptr = (const v512*&)data;
    auto zmm0 = _mm512_loadu_si512(ptr++); zmm0 ^= crc0;
    auto zmm4 = _mm512_loadu_si512(ptr++);
    nbytes -= 128;
    if (nbytes < 384) {
        auto rk3 = _mm512_broadcast_i32x4(*(__m128i*)_RK(3));
        do { // fold 128 bytes each iteration
            zmm0 = OP(zmm0, rk3, ptr++);
            zmm4 = OP(zmm4, rk3, ptr++);
            nbytes -= 128;
        } while (nbytes >= 128);
    } else { // nbytes >= 384
        auto rk_1_2 = _mm512_broadcast_i32x4(*(__m128i*)&rk512[0]);
        auto zmm7   = _mm512_loadu_si512(ptr++);
        auto zmm8   = _mm512_loadu_si512(ptr++);
        nbytes -= 128;
        do { // fold 256 bytes each iteration
            zmm0 = OP(zmm0, rk_1_2, ptr++);
            zmm4 = OP(zmm4, rk_1_2, ptr++);
            zmm7 = OP(zmm7, rk_1_2, ptr++);
            zmm8 = OP(zmm8, rk_1_2, ptr++);
            nbytes -= 256;
        } while (nbytes >= 256);
        auto rk3 = _mm512_broadcast_i32x4(*(__m128i*)_RK(3));
        zmm0 = OP(zmm0, rk3, zmm7);
        zmm4 = OP(zmm4, rk3, zmm8);
    }
    auto t = _mm512_extracti64x2_epi64(zmm4, 0x03);
    auto zmm7 = v512{t[0], t[1]};
    auto zmm1 = OP(zmm0, *(v512*)_RK(9), zmm7);
         zmm1 = OP(zmm4, *(v512*)_RK(17), zmm1);
    auto zmm8 = _mm512_shuffle_i64x2(zmm1, zmm1, 0x4e);
    auto ymm8 = ((__m256i&)zmm8) ^ ((__m256i&)zmm1);
    return _mm256_extracti64x2_epi64(ymm8, 0) ^
           _mm256_extracti64x2_epi64(ymm8, 1) ;
}

uint64_t crc64ecma_hw_avx512(const uint8_t *buf, size_t len, uint64_t crc) {
    return crc64ecma_hw_portable(buf, len, crc, crc64ecma_hw_big_avx512);
}
#ifdef __clang__
#pragma clang attribute pop
#else // __GNUC__
#pragma GCC pop_options
#endif
#endif  // __x86_64__

uint64_t crc64ecma_hw(const uint8_t *buffer, size_t nbytes, uint64_t crc) {
    return crc64ecma_auto(buffer, nbytes, crc);
}

