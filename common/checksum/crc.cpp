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
#include "crc_tables.h"
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

// Conditional value: returns val if c is true, 0 otherwise
template<typename T>
inline __attribute__((always_inline)) T cval(bool c, T val) {
    return (-T(c)) & val;
}

template<typename T>
struct TableCRC {
    typedef T (*Table)[256];
    Table table = (Table) malloc(sizeof(*table) * 8);
    ~TableCRC() { free(table); }
    TableCRC(T POLY) {
        for (int n = 0; n < 256; n++) {
            T crc = n;
            static_loop<0, 7, 1>([&](size_t) {
                crc = cval(crc & 1, POLY) ^ (crc >> 1);
            });
            table[0][n] = crc;
        }
        for (int n = 0; n < 256; n++) {
            T crc = table[0][n];
            static_loop<1, 7, 1>([&](size_t k) {
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
            static_loop<0, 7, 1>([&](size_t i) {
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

// =============================================================================
// Architecture-specific implementation
// =============================================================================

#if defined(__x86_64__)
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

// CRC32C hardware instructions for x86
inline __attribute__((always_inline)) uint32_t crc32c(uint32_t crc, uint8_t data) {
    return __builtin_ia32_crc32qi(crc, data);
}
inline __attribute__((always_inline)) uint32_t crc32c(uint32_t crc, uint16_t data) {
    return __builtin_ia32_crc32hi(crc, data);
}
inline __attribute__((always_inline)) uint32_t crc32c(uint32_t crc, uint32_t data) {
    return __builtin_ia32_crc32si(crc, data);
}
inline __attribute__((always_inline)) uint32_t crc32c(uint32_t crc, uint64_t data) {
    asm volatile ("crc32q %1, %q0" : "+r"(crc) : "rm"(data));
    return crc;
}

#elif defined(__aarch64__)
#include "mm_intrin_neon.h"
#ifdef __clang__
#pragma clang attribute push (__attribute__((target("aes,crc"))), apply_to=function)
#else // __GNUC__
#pragma GCC push_options
#pragma GCC target ("+crc+crypto")
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

// CRC32C hardware instructions for ARM
inline __attribute__((always_inline)) uint32_t crc32c(uint32_t crc, uint8_t data) {
    return __crc32cb(crc, data);
}
inline __attribute__((always_inline)) uint32_t crc32c(uint32_t crc, uint16_t data) {
    return __crc32ch(crc, data);
}
inline __attribute__((always_inline)) uint32_t crc32c(uint32_t crc, uint32_t data) {
    return __crc32cw(crc, data);
}
inline __attribute__((always_inline)) uint32_t crc32c(uint32_t crc, uint64_t data) {
    return __crc32cd(crc, data);
}

#else
#error "Unsupported architecture"
#endif

// =============================================================================
// SIMD helpers
// =============================================================================

using v128 = __m128i;

// Combined PCLMULQDQ operation: clmul<0x10> ^ clmul<0x01>
inline __attribute__((always_inline))
v128 clmul_op(v128 x, const uint64_t* rk) {
    return _mm_xor_si128(_mm_clmulepi64_si128(x, *(const v128*)rk, 0x10),
                         _mm_clmulepi64_si128(x, *(const v128*)rk, 0x01));
}

// Load small buffer (< 16 bytes)
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
inline __attribute__((always_inline)) v128 load_small(const void* data, size_t n) {
    assert(n < 16);
    uint64_t x = 0;
    auto end = (const char*)data + n;
    if (n & 4) x = *--(const uint32_t*&)end;
    if (n & 2) x = (x << 16) | *--(const uint16_t*&)end;
    if (n & 1) x = (x << 8) | *--(const uint8_t*&)end;
#if defined(__x86_64__)
    long vals[2] = {0, 0};
    if (n & 8) {
        vals[0] = *(const long*)data;
        vals[1] = (long)x;
    } else {
        vals[0] = (long)x;
    }
    return _mm_loadu_si128((const v128*)vals);
#else
    uint64_t vals[2] = {x, 0};
    if (n & 8) {
        vals[0] = *(const uint64_t*)data;
        vals[1] = x;
    }
    return _mm_loadu_si128((const v128*)vals);
#endif
}

template<size_t blksz, typename T> inline __attribute__((always_inline))
void crc32c_hw_block(const uint8_t*& data, size_t& nbytes, uint32_t& crc) {
    if (nbytes & blksz) {
        static_loop<0, blksz - sizeof(T), sizeof(T)>([&](size_t i) {
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
    static_loop<0, blksz_8 - 2, 1>([&](size_t i) {
        if (i < blksz_8 * 3 / 4)
            __builtin_prefetch(data + blksz*3 + i*8*4, 0, 0);
        crc  = crc32c(crc,  ptr[i]);
        crc1 = crc32c(crc1, ptr[i + blksz_8]);
        crc2 = crc32c(crc2, ptr[i + blksz_8 * 2]);
    });
    crc = crc32c(crc, ptr[blksz_8 - 1]);
    crc1 = crc32c(crc1, ptr[blksz_8 * 2 - 1]);
    // crc2 = crc32c(crc2, ptr[blksz_8 * 3 - 1]);

    auto k = crc32_merge_k<blksz>();
    v128 c0 = _mm_cvtsi64_si128(crc), c1 = _mm_cvtsi64_si128(crc1);
    auto t = _mm_xor_si128(_mm_clmulepi64_si128(c0, *(const v128*)k, 0x00),
                           _mm_clmulepi64_si128(c1, *(const v128*)k, 0x10));
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
    v128 crc1x = _mm_setr_epi32(crc1, 0, 0, 0);
    v128 cnstx = _mm_setr_epi32(x, 0, 0, 0);
    v128 result = _mm_clmulepi64_si128(crc1x, cnstx, 0x00);
    uint64_t dat64 = _mm_cvtsi128_si64(result);
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
        pd = (pd>>1) ^ cval(pd & 1, POLY) ^ cval(b & 1, a);
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

// SIMD mask accessor
inline __attribute__((always_inline))
v128 simd_mask(int i) {
    auto p = &simd_mask_table[(i - 1) * 2];
    return *(const v128*)p;
}

inline void* get_shf_table(size_t i) {
    return (char*)pshufb_shf_table + i;
}

inline __attribute__((always_inline))
v128 crc64ecma_hw_big_sse(const uint8_t*& data, size_t& nbytes, uint64_t crc) {
    v128 xmm[8];
    auto& ptr = (const v128*&)data;
    static_loop<0, 7, 1>([&](size_t i){ xmm[i] = _mm_loadu_si128(ptr+i); });
    xmm[0] = _mm_xor_si128(xmm[0], _mm_cvtsi64_si128(~crc)); ptr += 8; nbytes -= 128;
    do {
        static_loop<0, 7, 1>([&](size_t i) {
            xmm[i] = _mm_xor_si128(clmul_op(xmm[i], crc64_rk(3)), _mm_loadu_si128(ptr+i));
        });
        ptr += 8; nbytes -= 128;
    } while (nbytes >= 128);
    static_loop<0, 6, 1>([&](size_t i) {
        auto I = (i == 6) ? 1 : (9 + i * 2);
        xmm[7] = _mm_xor_si128(xmm[7], clmul_op(xmm[i], crc64_rk(I)));
    });
    return xmm[7];
}

template<typename F> inline __attribute__((always_inline))
uint64_t crc64ecma_hw_portable(const uint8_t *data, size_t nbytes, uint64_t crc, F hw_big) {
    if (unlikely(!nbytes || !data)) return crc;
    v128 xmm7 = _mm_cvtsi64_si128(~crc);
    auto& ptr = (const v128*&)data;
    if (nbytes >= 256) {
        xmm7 = hw_big(data, nbytes, crc);
    } else if (nbytes >= 16) {
        xmm7 = _mm_xor_si128(xmm7, _mm_loadu_si128(ptr++));
        nbytes -= 16;
    } else /* 0 < nbytes < 16*/ {
        xmm7 = _mm_xor_si128(xmm7, load_small(data, nbytes));
        if (nbytes >= 8) {
            auto shf = _mm_loadu_si128((const v128*)get_shf_table(nbytes));
            xmm7 = _mm_shuffle_epi8(xmm7, shf);
            goto _128_done;
        } else {
            auto shf = _mm_loadu_si128((const v128*)get_shf_table(nbytes + 8));
            xmm7 = _mm_shuffle_epi8(xmm7, shf);
            goto _barrett;
        }
    }

    while (nbytes >= 16) {
        xmm7 = _mm_xor_si128(clmul_op(xmm7, crc64_rk(1)), _mm_loadu_si128(ptr++));
        nbytes -= 16;
    }

    if (nbytes) {
        auto p = data + nbytes - 16;
        auto remainder = _mm_loadu_si128((const v128*)p);
        auto xmm0 = _mm_loadu_si128((const v128*)get_shf_table(nbytes));
        auto xmm2 = xmm7;
        xmm7 = _mm_shuffle_epi8(xmm7, xmm0);
        xmm0 = _mm_xor_si128(xmm0, simd_mask(3));
        xmm2 = _mm_shuffle_epi8(xmm2, xmm0);
        xmm2 = _mm_blendv_epi8(xmm2, remainder, xmm0);
        xmm7 = _mm_xor_si128(xmm2, clmul_op(xmm7, crc64_rk(1)));
    }
_128_done:
    xmm7 = _mm_xor_si128(_mm_clmulepi64_si128(xmm7, *(const v128*)crc64_rk(5), 0x00), _mm_bsrli_si128(xmm7, 8));
_barrett:
    auto t = _mm_clmulepi64_si128(xmm7, *(const v128*)crc64_rk(7), 0x00);
    xmm7 = _mm_xor_si128(xmm7, _mm_xor_si128(_mm_clmulepi64_si128(t, *(const v128*)crc64_rk(7), 0x10), _mm_bslli_si128(t, 8)));
    auto p = (uint64_t*)&xmm7;
    crc = ~p[1];
    return crc;
}

uint64_t crc64ecma_hw_sse128(const uint8_t *buf, size_t len, uint64_t crc) {
    return crc64ecma_hw_portable(buf, len, crc, crc64ecma_hw_big_sse);
}

// crc64ecma_lshift_table is now in crc_tables.cpp

static uint64_t clmul_modp_crc64ecma_hw(uint64_t crc, uint64_t x) {
    v128 rk7 = _mm_load_si128((const v128*)crc64_rk(7));

    v128 crc1x = _mm_cvtsi64_si128(crc);
    v128 constx = _mm_cvtsi64_si128(x);
    crc1x = _mm_clmulepi64_si128(crc1x, constx, 0x00);

    // Barrett Reduction
    v128 crc2x = _mm_clmulepi64_si128(crc1x, rk7, 0x00);
    v128 crc3x = _mm_clmulepi64_si128(crc2x, rk7, 0x10);
    crc2x = _mm_bslli_si128(crc2x, 8);
    crc1x = _mm_xor_si128(crc1x, crc2x);
    crc1x = _mm_xor_si128(crc1x, crc3x);
    return ((uint64_t*)&crc1x)[1];
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
        pd = (pd>>1) ^ cval(pd & 1, CRC64ECMA_POLY) ^ cval(b & 1, a);
    return pd;
}

uint64_t crc64ecma_combine_sw(uint64_t crc1, uint64_t crc2, uint32_t len2) {
    if (unlikely(!crc1)) return crc2;
    return crc2 ^ crc_apply_shifts(crc1, len2,
        crc64ecma_lshift_table, &clmul_modp64_sw);
}

// crc64ecma_rshift_table is now in crc_tables.cpp

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
// crc64_rk512 accessor is in crc_tables.h

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
        auto rk3 = _mm512_broadcast_i32x4(*(__m128i*)crc64_rk512(3));
        do { // fold 128 bytes each iteration
            zmm0 = OP(zmm0, rk3, ptr++);
            zmm4 = OP(zmm4, rk3, ptr++);
            nbytes -= 128;
        } while (nbytes >= 128);
    } else { // nbytes >= 384
        auto rk_1_2 = _mm512_broadcast_i32x4(*(__m128i*)&crc64_rk512_table[0]);
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
        auto rk3 = _mm512_broadcast_i32x4(*(__m128i*)crc64_rk512(3));
        zmm0 = OP(zmm0, rk3, zmm7);
        zmm4 = OP(zmm4, rk3, zmm8);
    }
    auto t = _mm512_extracti64x2_epi64(zmm4, 0x03);
    auto zmm7 = v512{t[0], t[1]};
    auto zmm1 = OP(zmm0, *(v512*)crc64_rk512(9), zmm7);
         zmm1 = OP(zmm4, *(v512*)crc64_rk512(17), zmm1);
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

// Pop the basic SSE/PCLMUL pragma that was pushed at the beginning of this file
#if defined(__x86_64__)
#ifdef __clang__
#pragma clang attribute pop
#else // __GNUC__
#pragma GCC pop_options
#endif
#elif defined(__aarch64__)
#ifdef __clang__
#pragma clang attribute pop
#else // __GNUC__
#pragma GCC pop_options
#endif
#endif // architecture
