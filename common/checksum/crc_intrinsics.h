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

#include <cstdint>
#include <cstddef>
#include <cassert>

// Architecture detection - headers only, pragma management by caller (crc.cpp)
#if defined(__x86_64__)
    #include <immintrin.h>
#elif defined(__aarch64__)
    #include <arm_neon.h>
    #include <arm_acle.h>
#else
    #error "Unsupported architecture"
#endif

namespace crc_intrinsics {

// =============================================================================
// CRC32C Hardware Instructions - Unified Interface
// =============================================================================

#if defined(__x86_64__)

inline __attribute__((always_inline)) uint32_t crc32c_u8(uint32_t crc, uint8_t data) {
    return __builtin_ia32_crc32qi(crc, data);
}

inline __attribute__((always_inline)) uint32_t crc32c_u16(uint32_t crc, uint16_t data) {
    return __builtin_ia32_crc32hi(crc, data);
}

inline __attribute__((always_inline)) uint32_t crc32c_u32(uint32_t crc, uint32_t data) {
    return __builtin_ia32_crc32si(crc, data);
}

inline __attribute__((always_inline)) uint32_t crc32c_u64(uint32_t crc, uint64_t data) {
    // Use inline asm to avoid extra 64-bit to 32-bit conversion
    asm volatile ("crc32q %1, %q0" : "+r"(crc) : "rm"(data));
    return crc;
}

#elif defined(__aarch64__)

#if defined(__ARM_FEATURE_CRC32)
    #if defined(__clang__)
    inline __attribute__((always_inline)) uint32_t crc32c_u8(uint32_t crc, uint8_t data) {
        return __builtin_arm_crc32cb(crc, data);
    }
    inline __attribute__((always_inline)) uint32_t crc32c_u16(uint32_t crc, uint16_t data) {
        return __builtin_arm_crc32ch(crc, data);
    }
    inline __attribute__((always_inline)) uint32_t crc32c_u32(uint32_t crc, uint32_t data) {
        return __builtin_arm_crc32cw(crc, data);
    }
    inline __attribute__((always_inline)) uint32_t crc32c_u64(uint32_t crc, uint64_t data) {
        return (uint32_t)__builtin_arm_crc32cd(crc, data);
    }
    #else // GCC
    inline __attribute__((always_inline)) uint32_t crc32c_u8(uint32_t crc, uint8_t data) {
        return __builtin_aarch64_crc32cb(crc, data);
    }
    inline __attribute__((always_inline)) uint32_t crc32c_u16(uint32_t crc, uint16_t data) {
        return __builtin_aarch64_crc32ch(crc, data);
    }
    inline __attribute__((always_inline)) uint32_t crc32c_u32(uint32_t crc, uint32_t data) {
        return __builtin_aarch64_crc32cw(crc, data);
    }
    inline __attribute__((always_inline)) uint32_t crc32c_u64(uint32_t crc, uint64_t data) {
        return (uint32_t)__builtin_aarch64_crc32cx(crc, data);
    }
    #endif
#else // No __ARM_FEATURE_CRC32, use inline asm
    inline __attribute__((always_inline)) uint32_t crc32c_u8(uint32_t crc, uint8_t value) {
        __asm__("crc32cb %w[c], %w[c], %w[v]" : [c]"+r"(crc) : [v]"r"(value));
        return crc;
    }
    inline __attribute__((always_inline)) uint32_t crc32c_u16(uint32_t crc, uint16_t value) {
        __asm__("crc32ch %w[c], %w[c], %w[v]" : [c]"+r"(crc) : [v]"r"(value));
        return crc;
    }
    inline __attribute__((always_inline)) uint32_t crc32c_u32(uint32_t crc, uint32_t value) {
        __asm__("crc32cw %w[c], %w[c], %w[v]" : [c]"+r"(crc) : [v]"r"(value));
        return crc;
    }
    inline __attribute__((always_inline)) uint32_t crc32c_u64(uint32_t crc, uint64_t value) {
        __asm__("crc32cx %w[c], %w[c], %x[v]" : [c]"+r"(crc) : [v]"r"(value));
        return crc;
    }
#endif

#endif // architecture

// =============================================================================
// SIMD 128-bit Operations - Unified Interface
// =============================================================================

#if defined(__x86_64__)

struct SIMD128 {
    using v128 = __m128i;

    static inline __attribute__((always_inline)) v128 loadu(const void* ptr) {
        return _mm_loadu_si128((const v128*)ptr);
    }

    static inline __attribute__((always_inline)) v128 pshufb(v128 x, v128 y) {
        return _mm_shuffle_epi8(x, y);
    }

    static inline __attribute__((always_inline)) v128 pblendvb(v128 x, v128 y, v128 z) {
        return _mm_blendv_epi8(x, y, z);
    }

    template<uint8_t imm>
    static inline __attribute__((always_inline)) v128 pclmulqdq(v128 x, const uint64_t* rk) {
        return _mm_clmulepi64_si128(x, *(const v128*)rk, imm);
    }

    // Direct v128 to v128 pclmulqdq
    template<uint8_t imm>
    static inline __attribute__((always_inline)) v128 clmul(v128 x, v128 y) {
        return _mm_clmulepi64_si128(x, y, imm);
    }

    static inline __attribute__((always_inline)) v128 op(v128 x, const uint64_t* rk) {
        return pclmulqdq<0x10>(x, rk) ^ pclmulqdq<0x01>(x, rk);
    }

    static inline __attribute__((always_inline)) v128 bsl8(v128 x) {
        return _mm_bslli_si128(x, 8);
    }

    static inline __attribute__((always_inline)) v128 bsr8(v128 x) {
        return _mm_bsrli_si128(x, 8);
    }

    // Create v128 from single 64-bit value (low 64 bits)
    static inline __attribute__((always_inline)) v128 from_u64(uint64_t x) {
        return _mm_cvtsi64_si128(x);
    }

    // Create v128 from single 32-bit value (low 32 bits)
    static inline __attribute__((always_inline)) v128 from_u32(uint32_t x) {
        return _mm_setr_epi32(x, 0, 0, 0);
    }

    // Extract low 64 bits
    static inline __attribute__((always_inline)) uint64_t to_u64(v128 x) {
        return _mm_cvtsi128_si64(x);
    }

    // Extract 64-bit value at index (0 or 1)
    // Note: _mm_extract_epi64 requires compile-time constant for idx
    static inline __attribute__((always_inline)) uint64_t extract_u64(v128 x, int idx) {
        return (idx == 0) ? _mm_extract_epi64(x, 0) : _mm_extract_epi64(x, 1);
    }

    // XOR two v128
    static inline __attribute__((always_inline)) v128 xor_v(v128 a, v128 b) {
        return _mm_xor_si128(a, b);
    }

    // Load aligned
    static inline __attribute__((always_inline)) v128 load(const void* ptr) {
        return _mm_load_si128((const v128*)ptr);
    }

    #pragma GCC diagnostic ignored "-Wstrict-aliasing"
    static inline __attribute__((always_inline)) v128 load_small(const void* data, size_t n) {
        assert(n < 16);
        long x = 0;
        auto end = (const char*)data + n;
        if (n & 4) x = *--(const uint32_t*&)end;
        if (n & 2) x = (x << 16) | *--(const uint16_t*&)end;
        if (n & 1) x = (x << 8) | *--(const uint8_t*&)end;
        return (n & 8) ? v128{*(long*)data, x} : v128{x, 0};
    }
};

#elif defined(__aarch64__)

namespace detail {
    // Template helpers for selecting polynomial elements based on imm value
    // Replaces if constexpr for C++14 compatibility
    // x86 PCLMULQDQ: imm[0] selects xmm1 (first operand) lane, imm[4] selects xmm2 (second operand) lane
    template<uint8_t imm, int lane_a = (imm & 0x01) ? 1 : 0, int lane_b = (imm & 0x10) ? 1 : 0>
    struct PclmulSelector {
        static inline __attribute__((always_inline))
        uint8x16_t compute(uint64x2_t a, uint64x2_t b) {
            poly64_t a_val = vgetq_lane_p64(vreinterpretq_p64_u64(a), lane_a);
            poly64_t b_val = vgetq_lane_p64(vreinterpretq_p64_u64(b), lane_b);
            return vreinterpretq_u8_p128(vmull_p64(a_val, b_val));
        }
    };
} // namespace detail

struct SIMD128 {
    using v128 = uint8x16_t;

    static inline __attribute__((always_inline)) v128 loadu(const void* ptr) {
        return vld1q_u8((const uint8_t*)ptr);
    }

    static inline __attribute__((always_inline)) v128 pshufb(v128 x, v128 y) {
        return vqtbl1q_u8(x, y);
    }

    static inline __attribute__((always_inline)) v128 pblendvb(v128 x, v128 y, v128 z) {
        // x86 pblendv: select y if z's high bit is 1, else select x
        // ARM vbslq_u8 is bit-wise, so we need to expand high bit to full byte
        int8x16_t mask = vshrq_n_s8(vreinterpretq_s8_u8(z), 7);  // arithmetic shift right by 7
        return vbslq_u8(vreinterpretq_u8_s8(mask), y, x);
    }

    template<uint8_t imm>
    static inline __attribute__((always_inline)) v128 pclmulqdq(v128 x, const uint64_t* rk) {
        // On ARM, use PMULL for carry-less multiplication
        // imm: 0x00 = low64 x low64, 0x01 = low64 x high64, 0x10 = high64 x low64, 0x11 = high64 x high64
        uint64x2_t a = vreinterpretq_u64_u8(x);
        uint64x2_t b = vld1q_u64(rk);
        return detail::PclmulSelector<imm>::compute(a, b);
    }

    // Direct v128 to v128 pclmulqdq
    template<uint8_t imm>
    static inline __attribute__((always_inline)) v128 clmul(v128 x, v128 y) {
        uint64x2_t a = vreinterpretq_u64_u8(x);
        uint64x2_t b = vreinterpretq_u64_u8(y);
        return detail::PclmulSelector<imm>::compute(a, b);
    }

    static inline __attribute__((always_inline)) v128 op(v128 x, const uint64_t* rk) {
        return veorq_u8(pclmulqdq<0x10>(x, rk), pclmulqdq<0x01>(x, rk));
    }

    static inline __attribute__((always_inline)) v128 bsl8(v128 x) {
        return vextq_u8(vdupq_n_u8(0), x, 8);
    }

    static inline __attribute__((always_inline)) v128 bsr8(v128 x) {
        return vextq_u8(x, vdupq_n_u8(0), 8);
    }

    // Create v128 from single 64-bit value (low 64 bits)
    static inline __attribute__((always_inline)) v128 from_u64(uint64_t x) {
        uint64x2_t v = vdupq_n_u64(0);
        v = vsetq_lane_u64(x, v, 0);
        return vreinterpretq_u8_u64(v);
    }

    // Create v128 from single 32-bit value (low 32 bits)
    static inline __attribute__((always_inline)) v128 from_u32(uint32_t x) {
        uint32x4_t v = vdupq_n_u32(0);
        v = vsetq_lane_u32(x, v, 0);
        return vreinterpretq_u8_u32(v);
    }

    // Extract low 64 bits
    static inline __attribute__((always_inline)) uint64_t to_u64(v128 x) {
        return vgetq_lane_u64(vreinterpretq_u64_u8(x), 0);
    }

    // Extract 64-bit value at index (0 or 1)
    static inline __attribute__((always_inline)) uint64_t extract_u64(v128 x, int idx) {
        uint64x2_t v = vreinterpretq_u64_u8(x);
        return (idx == 0) ? vgetq_lane_u64(v, 0) : vgetq_lane_u64(v, 1);
    }

    // XOR two v128
    static inline __attribute__((always_inline)) v128 xor_v(v128 a, v128 b) {
        return veorq_u8(a, b);
    }

    // Load aligned (same as unaligned on ARM)
    static inline __attribute__((always_inline)) v128 load(const void* ptr) {
        return vld1q_u8((const uint8_t*)ptr);
    }

    #pragma GCC diagnostic ignored "-Wstrict-aliasing"
    static inline __attribute__((always_inline)) v128 load_small(const void* data, size_t n) {
        assert(n < 16);
        uint64_t x = 0;
        auto end = (const char*)data + n;
        if (n & 4) x = *--(const uint32_t*&)end;
        if (n & 2) x = (x << 16) | *--(const uint16_t*&)end;
        if (n & 1) x = (x << 8) | *--(const uint8_t*&)end;
        uint64_t vals[2] = {x, 0};
        if (n & 8) {
            vals[0] = *(const uint64_t*)data;
            vals[1] = x;
        }
        return vld1q_u8((const uint8_t*)vals);
    }
};

#endif // architecture

// =============================================================================
// AVX-512 Support (x86_64 only)
// Note: Functions use __attribute__((target(...))) for AVX-512 instructions
// =============================================================================

#if defined(__x86_64__)

struct SIMD512 {
    using v512 = __m512i;

    static inline __attribute__((always_inline, target("avx512f,avx512dq,avx512vl,vpclmulqdq"))) v512 loadu(const void* ptr) {
        return _mm512_loadu_si512(ptr);
    }

    template<uint8_t imm>
    static inline __attribute__((always_inline, target("avx512f,avx512dq,avx512vl,vpclmulqdq"))) v512 clmul(v512 a, v512 b) {
        return _mm512_clmulepi64_epi128(a, b, imm);
    }

    static inline __attribute__((always_inline, target("avx512f,avx512dq,avx512vl,vpclmulqdq"))) v512 ternary_logic(v512 a, v512 b, v512 c, int imm) {
        return _mm512_ternarylogic_epi64(a, b, c, imm);
    }

    static inline __attribute__((always_inline, target("avx512f,avx512dq,avx512vl,vpclmulqdq"))) __m128i extract_i64x2(v512 x, int imm) {
        return _mm512_extracti64x2_epi64(x, imm);
    }

    static inline __attribute__((always_inline, target("avx512f,avx512dq,avx512vl,vpclmulqdq"))) v512 shuffle_i64x2(v512 a, v512 b, int imm) {
        return _mm512_shuffle_i64x2(a, b, imm);
    }

    static inline __attribute__((always_inline, target("avx512f,avx512dq,avx512vl,vpclmulqdq"))) __m256i cast_to_256(v512& x) {
        return (__m256i&)x;
    }

    static inline __attribute__((always_inline, target("avx512f,avx512dq,avx512vl,vpclmulqdq"))) v512 broadcast_i32x4(__m128i x) {
        return _mm512_broadcast_i32x4(x);
    }
};

// Helper function for 3-way operation with AVX-512
inline __attribute__((always_inline, target("avx512f,avx512dq,avx512vl,vpclmulqdq"))) SIMD512::v512 OP512(SIMD512::v512 a, SIMD512::v512 b, SIMD512::v512 c) {
    auto x = SIMD512::clmul<0x01>(a, b);
    auto y = SIMD512::clmul<0x10>(a, b);
    return SIMD512::ternary_logic(x, y, c, 0x96);
}

inline __attribute__((always_inline, target("avx512f,avx512dq,avx512vl,vpclmulqdq"))) SIMD512::v512 OP512(SIMD512::v512 a, SIMD512::v512 b, const SIMD512::v512* c) {
    return OP512(a, b, SIMD512::loadu(c));
}

#endif // __x86_64__

} // namespace crc_intrinsics
