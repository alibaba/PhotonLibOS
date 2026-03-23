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

// =============================================================================
// CRC Polynomials (reflected form)
// =============================================================================
//
// CRC32C (iSCSI polynomial):
//   Normal form:   0x1EDC6F41
//   Reflected:     0x82F63B78
//   Full:          x^32 + x^28 + x^27 + x^26 + x^25 + x^23 + x^22 + x^20 +
//                  x^19 + x^18 + x^14 + x^13 + x^11 + x^10 + x^9 + x^8 + x^6 + 1
//
// CRC64ECMA (ECMA-182):
//   Normal form:   0x42F0E1EBA9EA3693
//   Reflected:     0xC96C5795D7870F42
//   Full:          x^64 + x^62 + x^57 + x^55 + x^54 + x^53 + x^52 + x^47 +
//                  x^46 + x^45 + x^40 + x^39 + x^38 + x^37 + x^35 + x^33 +
//                  x^32 + x^31 + x^29 + x^27 + x^24 + x^23 + x^22 + x^21 +
//                  x^19 + x^17 + x^13 + x^12 + x^10 + x^9 + x^7 + x^4 + x + 1

constexpr uint32_t CRC32C_POLY = 0x82f63b78;
constexpr uint64_t CRC64ECMA_POLY = 0xc96c5795d7870f42;

// =============================================================================
// Table sizes
// =============================================================================

constexpr size_t CRC32_MERGE_TABLE_SIZE = 128;
constexpr size_t CRC32_LSHIFT_TABLE_HW_SIZE = 28;
constexpr size_t CRC32_SHIFT_TABLE_SIZE = 32;
constexpr size_t CRC64_LSHIFT_TABLE_SIZE = 33;
constexpr size_t CRC64_RSHIFT_TABLE_SIZE = 32;
constexpr size_t CRC64_RK_TABLE_SIZE = 20;
constexpr size_t CRC64_RK512_TABLE_SIZE = 26;
constexpr size_t SIMD_MASK_TABLE_SIZE = 6;
constexpr size_t PSHUFB_SHF_TABLE_SIZE = 4;

// =============================================================================
// External table declarations
// =============================================================================

extern const uint64_t crc32_merge_table_pclmulqdq[CRC32_MERGE_TABLE_SIZE * 2];
extern const uint32_t crc32c_lshift_table_hw[CRC32_LSHIFT_TABLE_HW_SIZE];
extern const uint32_t crc32c_rshift_table_hw[CRC32_SHIFT_TABLE_SIZE];
extern const uint32_t crc32c_lshift_table_sw[CRC32_SHIFT_TABLE_SIZE];
extern const uint32_t crc32c_rshift_table_sw[CRC32_SHIFT_TABLE_SIZE];
extern const uint64_t crc64ecma_lshift_table[CRC64_LSHIFT_TABLE_SIZE];
extern const uint64_t crc64ecma_rshift_table[CRC64_RSHIFT_TABLE_SIZE];
extern const uint64_t crc64_rk_table[CRC64_RK_TABLE_SIZE];
extern const uint64_t crc64_rk512_table[CRC64_RK512_TABLE_SIZE];
extern const uint64_t simd_mask_table[SIMD_MASK_TABLE_SIZE];
extern const uint64_t pshufb_shf_table[PSHUFB_SHF_TABLE_SIZE];

// =============================================================================
// Convenience macros
// =============================================================================

#define CRC64_RK(i) (&crc64_rk_table[(i)-1])
#define CRC64_RK512(i) (&crc64_rk512_table[(i)+1])
