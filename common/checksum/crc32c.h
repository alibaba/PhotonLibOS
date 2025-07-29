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
#include <photon/common/string_view.h>

uint32_t crc32c_sw(const uint8_t *buffer, size_t nbytes, uint32_t crc);

uint32_t crc32c_hw(const uint8_t *data, size_t nbytes, uint32_t crc);

/**
 * @brief We recommand using of crc32c() and crc32c_extend(), which can exploit hardware
 * acceleartion automatically. In case of using crc32c_hw() directly, please make sure invoking
 * is_crc32c_hw_available() to detect whether hardware acceleartion is available at first;
 *
 */
inline uint32_t crc32c_extend(const void *data, size_t nbytes, uint32_t crc) {
    extern uint32_t (*crc32c_auto)(const uint8_t *data, size_t nbytes, uint32_t crc);
    return crc32c_auto((uint8_t*)data, nbytes, crc);
}

inline uint32_t crc32c(const void *data, size_t nbytes) {
    return crc32c_extend((uint8_t*)data, nbytes, 0);
}

inline uint32_t crc32c_extend(std::string_view text, uint32_t crc) {
    return crc32c_extend(text.data(), text.size(), crc);
}

inline uint32_t crc32c(std::string_view text) {
    return crc32c_extend(text, 0);
}

/// @brief  Calculate the CRC32C values of a series of n parts that forms the buffer.
/// @param buffer the data buffer of part_size * n_parts in bytes
/// @param part_size size of each part in bytes
/// @param n_parts number of parts
/// @param crc_parts the buffer to store the CRC32C values of the parts, which is 4 * n_parts in bytes
void crc32c_series_sw(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint32_t* crc_parts);
void crc32c_series_hw(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint32_t* crc_parts);
inline void crc32c_series(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint32_t* crc_parts) {
    extern void (*crc32c_series_auto)(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint32_t* crc_parts);
    crc32c_series_auto(buffer, part_size, n_parts, crc_parts);
}

// crc1 and crc2 are the crc values of the first and second parts, to be combined.
// len2 is the length of the second part
uint32_t crc32c_combine_sw(uint32_t crc1, uint32_t crc2, uint32_t len2);
uint32_t crc32c_combine_hw(uint32_t crc1, uint32_t crc2, uint32_t len2);
inline uint32_t crc32c_combine(uint32_t crc1, uint32_t crc2, uint32_t len2) {
    extern uint32_t (*crc32c_combine_auto)(uint32_t crc1, uint32_t crc2, uint32_t len2);
    return crc32c_combine_auto(crc1, crc2, len2);
}

// combine a series of CRC32C values of a fixed part size
uint32_t crc32c_combine_series_sw(uint32_t* crc, uint32_t part_size, uint32_t n_parts);
uint32_t crc32c_combine_series_hw(uint32_t* crc, uint32_t part_size, uint32_t n_parts);
inline uint32_t crc32c_combine_series(uint32_t* crc, uint32_t part_size, uint32_t n_parts) {
    extern uint32_t (*crc32c_combine_series_auto)(uint32_t* crc, uint32_t part_size, uint32_t n_parts);
    return crc32c_combine_series_auto(crc, part_size, n_parts);
}

struct CRC32C_Component {
    uint32_t crc;
    uint32_t size;
};

// this function removes the prefix and suffix components a crc32c value
uint32_t crc32c_trim_sw(CRC32C_Component all, CRC32C_Component prefix, CRC32C_Component suffix);
inline uint32_t crc32c_trim(CRC32C_Component all, CRC32C_Component prefix, CRC32C_Component suffix) {
    return crc32c_trim_sw(all, prefix, suffix);
}

inline bool is_crc32c_hw_available() {
    extern uint32_t (*crc32c_auto)(const uint8_t *data, size_t nbytes, uint32_t crc);
    return crc32c_auto != crc32c_sw;
}
