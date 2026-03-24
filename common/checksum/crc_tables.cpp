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

/*
 * CRC Tables - Generated at compile time using constexpr functions
 *
 * All tables are computed using carry-less polynomial arithmetic in GF(2).
 *
 * Key formulas:
 * - x^n mod POLY: using square-and-multiply algorithm
 * - x^(-n) mod POLY: using precomputed x^(-1) and power algorithm
 * - (a * b) mod POLY: carry-less multiplication with reduction
 *
 * Verification: All values can be verified by calling the constexpr functions
 * at runtime and comparing with the table entries.
 */

#include "crc_tables.h"

// =============================================================================
// Internal constexpr functions for compile-time CRC computation
// =============================================================================
namespace {

// Precomputed x^(-1) values
// CRC32C: x^(-1) = x^(2^32 - 3) mod POLY = 0x05ec76f1
// CRC64ECMA: x^(-1) = 0x92d8af2baf0e1e85
constexpr uint32_t CRC32C_X_INV = 0x05ec76f1;
constexpr uint64_t CRC64ECMA_X_INV = 0x92d8af2baf0e1e85;

// -----------------------------------------------------------------------------
// Carry-less multiplication modulo polynomial
// -----------------------------------------------------------------------------
template<typename T, T POLY>
constexpr T clmul_modp(T a, T b) {
    T result = 0;
    for (size_t i = 0; i < sizeof(T) * 8; ++i) {
        T reduce = (result & 1) ? POLY : 0;
        T add_a = (b & 1) ? a : 0;
        result = (result >> 1) ^ reduce ^ add_a;
        b >>= 1;
    }
    return result;
}

// -----------------------------------------------------------------------------
// Power of x modulo polynomial: x^n mod POLY
// -----------------------------------------------------------------------------
template<typename T, T POLY>
constexpr T pow_modp(uint64_t n) {
    constexpr T X = T(1) << (sizeof(T) * 8 - 2);
    constexpr T ONE = T(1) << (sizeof(T) * 8 - 1);
    if (n == 0) return ONE;
    T result = ONE;
    T base = X;
    while (n > 0) {
        if (n & 1) result = clmul_modp<T, POLY>(result, base);
        base = clmul_modp<T, POLY>(base, base);
        n >>= 1;
    }
    return result;
}

constexpr uint32_t pow32(uint64_t n) { return pow_modp<uint32_t, CRC32C_POLY>(n); }
constexpr uint64_t pow64(uint64_t n) { return pow_modp<uint64_t, CRC64ECMA_POLY>(n); }

// -----------------------------------------------------------------------------
// Inverse power: x^(-n) mod POLY
// -----------------------------------------------------------------------------
template<typename T, T POLY, T X_INV>
constexpr T ipow_modp(uint64_t n) {
    constexpr T ONE = T(1) << (sizeof(T) * 8 - 1);
    if (n == 0) return ONE;
    T result = ONE;
    T base = X_INV;
    while (n > 0) {
        if (n & 1) result = clmul_modp<T, POLY>(result, base);
        base = clmul_modp<T, POLY>(base, base);
        n >>= 1;
    }
    return result;
}

constexpr uint32_t ipow32(uint64_t n) { return ipow_modp<uint32_t, CRC32C_POLY, CRC32C_X_INV>(n); }
constexpr uint64_t ipow64(uint64_t n) { return ipow_modp<uint64_t, CRC64ECMA_POLY, CRC64ECMA_X_INV>(n); }

// -----------------------------------------------------------------------------
// CRC32C table generation formulas
// -----------------------------------------------------------------------------
constexpr uint32_t crc32c_lshift_hw(size_t i) { return pow32((128ULL << i) - 33); }
constexpr uint32_t crc32c_rshift_hw(size_t i) { return ipow32((1ULL << (i + 3)) + 33); }
constexpr uint32_t crc32c_lshift_sw(size_t i) { return pow32(1ULL << (i + 3)); }
constexpr uint32_t crc32c_rshift_sw(size_t i) { return ipow32(1ULL << (i + 3)); }

// -----------------------------------------------------------------------------
// CRC64ECMA table generation formulas
// -----------------------------------------------------------------------------
constexpr uint64_t crc64_lshift(size_t i) { return pow64((1ULL << (i + 3)) - 1); }
constexpr uint64_t crc64_rshift(size_t i) { return ipow64((1ULL << (i + 3)) + 1); }

// -----------------------------------------------------------------------------
// CRC32C merge table (33-bit PCLMULQDQ constants)
// -----------------------------------------------------------------------------
// XOR bitmap for 256 entries (bit[idx] = 1 means XOR with X_INV is needed)
constexpr uint64_t CRC32_MERGE_XOR_BITMAP[4] = {
    0x094d03d41b841e1bULL, 0x016fa58b3219a890ULL,
    0x6940792842010b65ULL, 0x736ec0a8619b5349ULL,
};

constexpr bool crc32_merge_need_xor(size_t idx) {
    return (CRC32_MERGE_XOR_BITMAP[idx / 64] >> (idx % 64)) & 1;
}

constexpr uint64_t crc32_merge_entry(size_t idx) {
    size_t blksz = (idx / 2 + 1) * 8;
    size_t multiplier = (idx % 2 == 0) ? 2 : 1;
    uint64_t bits = multiplier * blksz * 8;
    uint32_t v = pow32(bits - 33);
    bool need_xor = crc32_merge_need_xor(idx);
    uint32_t low32 = need_xor ? (v ^ CRC32C_X_INV) : v;
    return ((uint64_t)need_xor << 32) | low32;
}

constexpr uint64_t crc32_merge_k0(size_t blksz) { return crc32_merge_entry((blksz / 8 - 1) * 2); }
constexpr uint64_t crc32_merge_k1(size_t blksz) { return crc32_merge_entry((blksz / 8 - 1) * 2 + 1); }

} // anonymous namespace

// =============================================================================
// CRC32C merge table for 3-way ILP (Instruction-Level Parallelism)
// =============================================================================
//
// Only for block sizes actually used: 64, 128, 256, 512 bytes
// Each pair contains folding constants for PCLMULQDQ:
//   k[0] = crc32_merge_k0(B) - fold crc by 2*B*8 bits
//   k[1] = crc32_merge_k1(B) - fold crc1 by B*8 bits
//
// These are 33-bit PCLMULQDQ constants computed at compile time.
// The formula uses a bitmap to determine when to XOR with x^(-1):
//   1. v = x^(bits - 33) mod POLY32
//   2. If bitmap bit is set: low32 = v ^ X_INV, bit33 = 1
//   3. Otherwise: low32 = v, bit33 = 0
//   4. result = (bit33 << 32) | low32

// Helper macro for generating merge table entries
#define MERGE_ENTRY(blksz) crc32_merge_k0(blksz), crc32_merge_k1(blksz)

alignas(16) const uint64_t crc32_merge_table_pclmulqdq[CRC32_MERGE_TABLE_SIZE * 2] = {
    MERGE_ENTRY(64),   // index 0-1: blksz=64
    MERGE_ENTRY(128),  // index 2-3: blksz=128
    MERGE_ENTRY(256),  // index 4-5: blksz=256
    MERGE_ENTRY(512),  // index 6-7: blksz=512
};

#undef MERGE_ENTRY

// =============================================================================
// CRC32C shift tables (hardware version)
// =============================================================================
//
// lshift_table_hw[i] = x^(128 * 2^i - 33) mod POLY
// Used for combining CRC values when len2 >> 4 is passed to crc_apply_shifts.
// The formula accounts for: PCLMULQDQ produces 64-bit result, then crc32c()
// hardware instruction reduces it (multiplying by x^(-33)).
// Indices 0..27 correspond to shifts by 16, 32, 64, ..., 2G bytes (128, 256, 512... bits).

const uint32_t crc32c_lshift_table_hw[CRC32_LSHIFT_TABLE_HW_SIZE] = {
    crc32c_lshift_hw(0),  crc32c_lshift_hw(1),  crc32c_lshift_hw(2),  crc32c_lshift_hw(3),
    crc32c_lshift_hw(4),  crc32c_lshift_hw(5),  crc32c_lshift_hw(6),  crc32c_lshift_hw(7),
    crc32c_lshift_hw(8),  crc32c_lshift_hw(9),  crc32c_lshift_hw(10), crc32c_lshift_hw(11),
    crc32c_lshift_hw(12), crc32c_lshift_hw(13), crc32c_lshift_hw(14), crc32c_lshift_hw(15),
    crc32c_lshift_hw(16), crc32c_lshift_hw(17), crc32c_lshift_hw(18), crc32c_lshift_hw(19),
    crc32c_lshift_hw(20), crc32c_lshift_hw(21), crc32c_lshift_hw(22), crc32c_lshift_hw(23),
    crc32c_lshift_hw(24), crc32c_lshift_hw(25), crc32c_lshift_hw(26), crc32c_lshift_hw(27),
};

// rshift_table_hw[i] = x^(-(2^(i+3)) - 32 - 1) mod POLY
const uint32_t crc32c_rshift_table_hw[CRC32_SHIFT_TABLE_SIZE] = {
    crc32c_rshift_hw(0),  crc32c_rshift_hw(1),  crc32c_rshift_hw(2),  crc32c_rshift_hw(3),
    crc32c_rshift_hw(4),  crc32c_rshift_hw(5),  crc32c_rshift_hw(6),  crc32c_rshift_hw(7),
    crc32c_rshift_hw(8),  crc32c_rshift_hw(9),  crc32c_rshift_hw(10), crc32c_rshift_hw(11),
    crc32c_rshift_hw(12), crc32c_rshift_hw(13), crc32c_rshift_hw(14), crc32c_rshift_hw(15),
    crc32c_rshift_hw(16), crc32c_rshift_hw(17), crc32c_rshift_hw(18), crc32c_rshift_hw(19),
    crc32c_rshift_hw(20), crc32c_rshift_hw(21), crc32c_rshift_hw(22), crc32c_rshift_hw(23),
    crc32c_rshift_hw(24), crc32c_rshift_hw(25), crc32c_rshift_hw(26), crc32c_rshift_hw(27),
    crc32c_rshift_hw(28), crc32c_rshift_hw(29), crc32c_rshift_hw(30), crc32c_rshift_hw(31),
};

// =============================================================================
// CRC32C shift tables (software version)
// =============================================================================
//
// lshift_table_sw[i] = x^(2^(i+3)) mod POLY
const uint32_t crc32c_lshift_table_sw[CRC32_SHIFT_TABLE_SIZE] = {
    crc32c_lshift_sw(0),  crc32c_lshift_sw(1),  crc32c_lshift_sw(2),  crc32c_lshift_sw(3),
    crc32c_lshift_sw(4),  crc32c_lshift_sw(5),  crc32c_lshift_sw(6),  crc32c_lshift_sw(7),
    crc32c_lshift_sw(8),  crc32c_lshift_sw(9),  crc32c_lshift_sw(10), crc32c_lshift_sw(11),
    crc32c_lshift_sw(12), crc32c_lshift_sw(13), crc32c_lshift_sw(14), crc32c_lshift_sw(15),
    crc32c_lshift_sw(16), crc32c_lshift_sw(17), crc32c_lshift_sw(18), crc32c_lshift_sw(19),
    crc32c_lshift_sw(20), crc32c_lshift_sw(21), crc32c_lshift_sw(22), crc32c_lshift_sw(23),
    crc32c_lshift_sw(24), crc32c_lshift_sw(25), crc32c_lshift_sw(26), crc32c_lshift_sw(27),
    crc32c_lshift_sw(28), crc32c_lshift_sw(29), crc32c_lshift_sw(30), crc32c_lshift_sw(31),
};

// rshift_table_sw[i] = x^(-(2^(i+3))) mod POLY
const uint32_t crc32c_rshift_table_sw[CRC32_SHIFT_TABLE_SIZE] = {
    crc32c_rshift_sw(0),  crc32c_rshift_sw(1),  crc32c_rshift_sw(2),  crc32c_rshift_sw(3),
    crc32c_rshift_sw(4),  crc32c_rshift_sw(5),  crc32c_rshift_sw(6),  crc32c_rshift_sw(7),
    crc32c_rshift_sw(8),  crc32c_rshift_sw(9),  crc32c_rshift_sw(10), crc32c_rshift_sw(11),
    crc32c_rshift_sw(12), crc32c_rshift_sw(13), crc32c_rshift_sw(14), crc32c_rshift_sw(15),
    crc32c_rshift_sw(16), crc32c_rshift_sw(17), crc32c_rshift_sw(18), crc32c_rshift_sw(19),
    crc32c_rshift_sw(20), crc32c_rshift_sw(21), crc32c_rshift_sw(22), crc32c_rshift_sw(23),
    crc32c_rshift_sw(24), crc32c_rshift_sw(25), crc32c_rshift_sw(26), crc32c_rshift_sw(27),
    crc32c_rshift_sw(28), crc32c_rshift_sw(29), crc32c_rshift_sw(30), crc32c_rshift_sw(31),
};

// =============================================================================
// CRC64ECMA shift tables
// =============================================================================
//
// lshift_table[i] for i = 0..3: x^(8 * 2^i - 1) for bytes 1, 2, 4, 8
// lshift_table[i] for i = 4..32: x^(8 * 2^(i+1) - 1) for larger sizes

const uint64_t crc64ecma_lshift_table[CRC64_LSHIFT_TABLE_SIZE] = {
    crc64_lshift(0),  crc64_lshift(1),  crc64_lshift(2),  crc64_lshift(3),
    crc64_lshift(4),  crc64_lshift(5),  crc64_lshift(6),  crc64_lshift(7),
    crc64_lshift(8),  crc64_lshift(9),  crc64_lshift(10), crc64_lshift(11),
    crc64_lshift(12), crc64_lshift(13), crc64_lshift(14), crc64_lshift(15),
    crc64_lshift(16), crc64_lshift(17), crc64_lshift(18), crc64_lshift(19),
    crc64_lshift(20), crc64_lshift(21), crc64_lshift(22), crc64_lshift(23),
    crc64_lshift(24), crc64_lshift(25), crc64_lshift(26), crc64_lshift(27),
    crc64_lshift(28), crc64_lshift(29), crc64_lshift(30), crc64_lshift(31),
    crc64_lshift(32),
};

// rshift_table[i] = x^(-(2^(i+3) + 1)) mod POLY
const uint64_t crc64ecma_rshift_table[CRC64_RSHIFT_TABLE_SIZE] = {
    crc64_rshift(0),  crc64_rshift(1),  crc64_rshift(2),  crc64_rshift(3),
    crc64_rshift(4),  crc64_rshift(5),  crc64_rshift(6),  crc64_rshift(7),
    crc64_rshift(8),  crc64_rshift(9),  crc64_rshift(10), crc64_rshift(11),
    crc64_rshift(12), crc64_rshift(13), crc64_rshift(14), crc64_rshift(15),
    crc64_rshift(16), crc64_rshift(17), crc64_rshift(18), crc64_rshift(19),
    crc64_rshift(20), crc64_rshift(21), crc64_rshift(22), crc64_rshift(23),
    crc64_rshift(24), crc64_rshift(25), crc64_rshift(26), crc64_rshift(27),
    crc64_rshift(28), crc64_rshift(29), crc64_rshift(30), crc64_rshift(31),
};

// =============================================================================
// CRC64ECMA constants (rk) for PCLMULQDQ computation
// =============================================================================
//
// rk1, rk5: x^127 mod POLY (128-bit to 64-bit folding)
// rk2: x^191 mod POLY
// rk3: x^1023 mod POLY (512-bit folding)
// rk4: x^1087 mod POLY
// rk6: 0 (unused padding)
// rk7: Barrett reduction constant mu = 0x9c3e466c172963d5
// rk8: Barrett constant = 0x92d8af2baf0e1e84
// rk9~rk20: folding constants for SSE reduction
//   For i = 0..5, k = 7-i: rk[2*i+9] = x^(128*k-1), rk[2*i+10] = x^(128*k+63)

alignas(16) const uint64_t crc64_rk_table[CRC64_RK_TABLE_SIZE] = {
    pow64(127),                // rk1:  x^127
    pow64(191),                // rk2:  x^191
    pow64(1023),               // rk3:  x^1023
    pow64(1087),               // rk4:  x^1087
    pow64(127),                // rk5:  x^127 (same as rk1)
    0,                         // rk6:  unused padding
    0x9c3e466c172963d5ULL,     // rk7:  Barrett mu constant
    0x92d8af2baf0e1e84ULL,     // rk8:  Barrett constant
    pow64(895),                // rk9:  x^(128*7-1) for folding xmm[0]
    pow64(959),                // rk10: x^(128*7+63)
    pow64(767),                // rk11: x^(128*6-1) for folding xmm[1]
    pow64(831),                // rk12: x^(128*6+63)
    pow64(639),                // rk13: x^(128*5-1) for folding xmm[2]
    pow64(703),                // rk14: x^(128*5+63)
    pow64(511),                // rk15: x^(128*4-1) for folding xmm[3]
    pow64(575),                // rk16: x^(128*4+63)
    pow64(383),                // rk17: x^(128*3-1) for folding xmm[4]
    pow64(447),                // rk18: x^(128*3+63)
    pow64(255),                // rk19: x^(128*2-1) for folding xmm[5]
    pow64(319),                // rk20: x^(128*2+63)
};

// =============================================================================
// CRC64ECMA AVX-512 constants
// =============================================================================

alignas(16) const uint64_t crc64_rk512_table[CRC64_RK512_TABLE_SIZE] = {
    pow64(2047),               // rk_1: x^2047 for 256-byte folding
    pow64(2111),               // rk_2: x^2111
    pow64(127),                // rk1
    pow64(191),                // rk2
    pow64(1023),               // rk3
    pow64(1087),               // rk4
    pow64(127),                // rk5
    0,                         // rk6
    0x9c3e466c172963d5ULL,     // rk7
    0x92d8af2baf0e1e84ULL,     // rk8
    pow64(895),                // rk9
    pow64(959),                // rk10
    pow64(767),                // rk11
    pow64(831),                // rk12
    pow64(639),                // rk13
    pow64(703),                // rk14
    pow64(511),                // rk15
    pow64(575),                // rk16
    pow64(383),                // rk17
    pow64(447),                // rk18
    pow64(255),                // rk19
    pow64(319),                // rk20
    pow64(127),                // rk_1b (copy for alignment)
    pow64(191),                // rk_2b
    0,                         // padding
    0,                         // padding
};

// =============================================================================
// SIMD helper tables (constant, not computed)
// =============================================================================

// Mask table for SIMD blending operations
alignas(16) const uint64_t simd_mask_table[SIMD_MASK_TABLE_SIZE] = {
    0xFFFFFFFFFFFFFFFF, 0x0000000000000000,  // mask1: all 1s, all 0s
    0xFFFFFFFF00000000, 0xFFFFFFFFFFFFFFFF,  // mask2: upper 32 bits
    0x8080808080808080, 0x8080808080808080,  // mask3: sign bits
};

// PSHUFB shift table for byte shuffling
// These are fixed shuffle patterns, not polynomial-based
alignas(16) const uint64_t pshufb_shf_table[PSHUFB_SHF_TABLE_SIZE] = {
    0x8786858483828100, 0x8f8e8d8c8b8a8988,  // shift left pattern
    0x0706050403020100, 0x000e0d0c0b0a0908,  // identity + right shift
};
