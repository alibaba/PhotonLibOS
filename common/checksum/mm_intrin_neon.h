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

// SSE to NEON translation layer for CRC operations
// This file provides x86 SSE intrinsics implemented using ARM NEON
// Only includes intrinsics actually used by crc.cpp

#pragma once

#ifdef __aarch64__

#include <arm_neon.h>
#include <arm_acle.h>
#include <cstdint>

// =============================================================================
// Type definitions
// =============================================================================

// __m128i is represented as int64x2_t for easy 64-bit lane access
typedef int64x2_t __m128i;

// For unaligned access
typedef int64x2_t __m128i_u __attribute__((__aligned__(1)));

// =============================================================================
// Load/Store operations
// =============================================================================

// Load 128 bits of integer data (unaligned)
inline __attribute__((always_inline))
__m128i _mm_loadu_si128(const __m128i* ptr) {
    return vreinterpretq_s64_u8(vld1q_u8((const uint8_t*)ptr));
}

// Load 128 bits of integer data (aligned)
inline __attribute__((always_inline))
__m128i _mm_load_si128(const __m128i* ptr) {
    return vreinterpretq_s64_u8(vld1q_u8((const uint8_t*)ptr));
}

// Store 128 bits (unaligned)
inline __attribute__((always_inline))
void _mm_storeu_si128(__m128i* ptr, __m128i a) {
    vst1q_u8((uint8_t*)ptr, vreinterpretq_u8_s64(a));
}

// =============================================================================
// Arithmetic/Logic operations
// =============================================================================

// XOR
inline __attribute__((always_inline))
__m128i _mm_xor_si128(__m128i a, __m128i b) {
    return veorq_s64(a, b);
}

// AND
inline __attribute__((always_inline))
__m128i _mm_and_si128(__m128i a, __m128i b) {
    return vandq_s64(a, b);
}

// OR
inline __attribute__((always_inline))
__m128i _mm_or_si128(__m128i a, __m128i b) {
    return vorrq_s64(a, b);
}

// =============================================================================
// Shuffle operations
// =============================================================================

// PSHUFB - shuffle bytes
inline __attribute__((always_inline))
__m128i _mm_shuffle_epi8(__m128i a, __m128i b) {
    uint8x16_t tbl = vreinterpretq_u8_s64(a);
    uint8x16_t idx = vreinterpretq_u8_s64(b);
    return vreinterpretq_s64_u8(vqtbl1q_u8(tbl, idx));
}

// PBLENDVB - variable blend
inline __attribute__((always_inline))
__m128i _mm_blendv_epi8(__m128i a, __m128i b, __m128i mask) {
    // x86: select b if mask's high bit is 1, else select a
    int8x16_t m = vshrq_n_s8(vreinterpretq_s8_s64(mask), 7);  // expand sign bit
    uint8x16_t sel = vreinterpretq_u8_s8(m);
    return vreinterpretq_s64_u8(vbslq_u8(sel, vreinterpretq_u8_s64(b), vreinterpretq_u8_s64(a)));
}

// =============================================================================
// Byte shift operations
// =============================================================================

// Shift left by 8 bytes (64 bits)
inline __attribute__((always_inline))
__m128i _mm_bslli_si128_8(__m128i a) {
    return vreinterpretq_s64_u8(vextq_u8(vdupq_n_u8(0), vreinterpretq_u8_s64(a), 8));
}

// Shift right by 8 bytes (64 bits)
inline __attribute__((always_inline))
__m128i _mm_bsrli_si128_8(__m128i a) {
    return vreinterpretq_s64_u8(vextq_u8(vreinterpretq_u8_s64(a), vdupq_n_u8(0), 8));
}

// Generic byte shift left (only n=8 is used in crc.cpp)
#define _mm_bslli_si128(a, n) \
    ((n) == 8 ? _mm_bslli_si128_8(a) : vreinterpretq_s64_u8(vextq_u8(vdupq_n_u8(0), vreinterpretq_u8_s64(a), 16-(n))))

// Generic byte shift right (only n=8 is used in crc.cpp)
#define _mm_bsrli_si128(a, n) \
    ((n) == 8 ? _mm_bsrli_si128_8(a) : vreinterpretq_s64_u8(vextq_u8(vreinterpretq_u8_s64(a), vdupq_n_u8(0), (n))))

// Alias for compatibility
#define _mm_slli_si128 _mm_bslli_si128
#define _mm_srli_si128 _mm_bsrli_si128

// =============================================================================
// Conversion operations
// =============================================================================

// Set low 64 bits from integer
inline __attribute__((always_inline))
__m128i _mm_cvtsi64_si128(int64_t a) {
    return vsetq_lane_s64(a, vdupq_n_s64(0), 0);
}

// Extract low 64 bits as integer
inline __attribute__((always_inline))
int64_t _mm_cvtsi128_si64(__m128i a) {
    return vgetq_lane_s64(a, 0);
}

// Set 4 32-bit integers (a0 is lowest)
inline __attribute__((always_inline))
__m128i _mm_setr_epi32(int32_t a0, int32_t a1, int32_t a2, int32_t a3) {
    int32_t arr[4] = {a0, a1, a2, a3};
    return vreinterpretq_s64_s32(vld1q_s32(arr));
}

inline __attribute__((always_inline))
__m128i _mm_set_epi64x(uint64_t a0, uint64_t a1) {
    uint64_t arr[2] = {a0, a1};
    return vreinterpretq_s64_u64(vld1q_u64(arr));
}


// Set all elements to zero
inline __attribute__((always_inline))
__m128i _mm_setzero_si128(void) {
    return vdupq_n_s64(0);
}

// =============================================================================
// Extract operations
// =============================================================================

// Extract 64-bit integer at index (imm must be 0 or 1)
// Note: imm must be compile-time constant
template<int imm>
inline __attribute__((always_inline))
int64_t _mm_extract_epi64_impl(__m128i a) {
    static_assert(imm == 0 || imm == 1, "imm must be 0 or 1");
    return vgetq_lane_s64(a, imm);
}

#define _mm_extract_epi64(a, imm) _mm_extract_epi64_impl<imm>(a)

// =============================================================================
// Carry-less multiplication (PCLMULQDQ)
// =============================================================================
// This is the critical operation for CRC computation
// imm selects which 64-bit halves to multiply:
//   0x00: a[0] * b[0]
//   0x01: a[1] * b[0]  (note: reversed from x86 convention in some docs)
//   0x10: a[0] * b[1]
//   0x11: a[1] * b[1]

#if defined(__clang__)
#define PHOTON_NEON_AESCRC_TARGET __attribute__((target("aes,crc")))
#else
#define PHOTON_NEON_AESCRC_TARGET
#endif

namespace _sse2neon_detail {
    // Helper to select lane based on imm bit
    template<int imm, int lane_a = (imm & 0x01) ? 1 : 0, int lane_b = (imm & 0x10) ? 1 : 0>
    struct PclmulImpl {
        static inline __attribute__((always_inline)) PHOTON_NEON_AESCRC_TARGET
        __m128i compute(__m128i a, __m128i b) {
            poly64_t av = (poly64_t)vgetq_lane_u64(vreinterpretq_u64_s64(a), lane_a);
            poly64_t bv = (poly64_t)vgetq_lane_u64(vreinterpretq_u64_s64(b), lane_b);
            poly128_t result = vmull_p64(av, bv);
            return vreinterpretq_s64_p128(result);
        }
    };
} // namespace _sse2neon_detail

template<int imm>
inline __attribute__((always_inline)) PHOTON_NEON_AESCRC_TARGET
__m128i _mm_clmulepi64_si128_impl(__m128i a, __m128i b) {
    return _sse2neon_detail::PclmulImpl<imm>::compute(a, b);
}

#define _mm_clmulepi64_si128(a, b, imm) _mm_clmulepi64_si128_impl<imm>(a, b)

// =============================================================================
// CRC32C hardware instructions
// =============================================================================
// Both GCC and modern Clang (including Apple Clang 17+) provide __crc32c* via arm_acle.h
#include <arm_acle.h>

#endif // __aarch64__
