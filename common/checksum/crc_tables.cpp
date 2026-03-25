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
#include <utility>  // for std::index_sequence

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
// Compile-time array generator using std::index_sequence
// Produces native arrays with constexpr static storage
// -----------------------------------------------------------------------------
template<typename T, size_t N, T (*Gen)(size_t), typename Seq>
struct CompileTimeTableImpl;

template<typename T, size_t N, T (*Gen)(size_t), size_t... Is>
struct CompileTimeTableImpl<T, N, Gen, std::index_sequence<Is...>> {
    static constexpr T value[N] = { Gen(Is)... };
};

// C++14 requires out-of-class definition for constexpr static members
template<typename T, size_t N, T (*Gen)(size_t), size_t... Is>
constexpr T CompileTimeTableImpl<T, N, Gen, std::index_sequence<Is...>>::value[N];

// Convenience alias
template<typename T, size_t N, T (*Gen)(size_t)>
using CompileTimeTable = CompileTimeTableImpl<T, N, Gen, std::make_index_sequence<N>>;

} // anonymous namespace

// =============================================================================
// CRC32C shift tables (hardware version)
// =============================================================================
//
// lshift_table_hw[i] = x^(128 * 2^i - 33) mod POLY
// Used for combining CRC values when len2 >> 4 is passed to crc_apply_shifts.
// The formula accounts for: PCLMULQDQ produces 64-bit result, then crc32c()
// hardware instruction reduces it (multiplying by x^(-33)).
// Indices 0..27 correspond to shifts by 16, 32, 64, ..., 2G bytes (128, 256, 512... bits).

const uint32_t (&crc32c_lshift_table_hw)[CRC32_LSHIFT_TABLE_HW_SIZE] =
    CompileTimeTable<uint32_t, CRC32_LSHIFT_TABLE_HW_SIZE, crc32c_lshift_hw>::value;

// rshift_table_hw[i] = x^(-(2^(i+3)) - 32 - 1) mod POLY
const uint32_t (&crc32c_rshift_table_hw)[CRC32_SHIFT_TABLE_SIZE] =
    CompileTimeTable<uint32_t, CRC32_SHIFT_TABLE_SIZE, crc32c_rshift_hw>::value;

// =============================================================================
// CRC32C shift tables (software version)
// =============================================================================
//
// lshift_table_sw[i] = x^(2^(i+3)) mod POLY
const uint32_t (&crc32c_lshift_table_sw)[CRC32_SHIFT_TABLE_SIZE] =
    CompileTimeTable<uint32_t, CRC32_SHIFT_TABLE_SIZE, crc32c_lshift_sw>::value;

// rshift_table_sw[i] = x^(-(2^(i+3))) mod POLY
const uint32_t (&crc32c_rshift_table_sw)[CRC32_SHIFT_TABLE_SIZE] =
    CompileTimeTable<uint32_t, CRC32_SHIFT_TABLE_SIZE, crc32c_rshift_sw>::value;

// =============================================================================
// CRC64ECMA shift tables
// =============================================================================
//
// lshift_table[i] for i = 0..3: x^(8 * 2^i - 1) for bytes 1, 2, 4, 8
// lshift_table[i] for i = 4..32: x^(8 * 2^(i+1) - 1) for larger sizes

const uint64_t (&crc64ecma_lshift_table)[CRC64_LSHIFT_TABLE_SIZE] =
    CompileTimeTable<uint64_t, CRC64_LSHIFT_TABLE_SIZE, crc64_lshift>::value;

// rshift_table[i] = x^(-(2^(i+3) + 1)) mod POLY
const uint64_t (&crc64ecma_rshift_table)[CRC64_RSHIFT_TABLE_SIZE] =
    CompileTimeTable<uint64_t, CRC64_RSHIFT_TABLE_SIZE, crc64_rshift>::value;

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

struct Crc64RkTableHolder {
    alignas(16) static constexpr uint64_t value[CRC64_RK_TABLE_SIZE] = {
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
};
alignas(16) constexpr uint64_t Crc64RkTableHolder::value[CRC64_RK_TABLE_SIZE];
const uint64_t (&crc64_rk_table)[CRC64_RK_TABLE_SIZE] = Crc64RkTableHolder::value;

// =============================================================================
// CRC64ECMA AVX-512 constants
// =============================================================================

struct Crc64Rk512TableHolder {
    alignas(16) static constexpr uint64_t value[CRC64_RK512_TABLE_SIZE] = {
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
};
alignas(16) constexpr uint64_t Crc64Rk512TableHolder::value[CRC64_RK512_TABLE_SIZE];
const uint64_t (&crc64_rk512_table)[CRC64_RK512_TABLE_SIZE] = Crc64Rk512TableHolder::value;
