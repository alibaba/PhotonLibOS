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

uint64_t crc64ecma_sw(const uint8_t *buffer, size_t nbytes, uint64_t crc);
uint64_t crc64ecma_hw(const uint8_t *buffer, size_t nbytes, uint64_t crc);

inline uint64_t crc64ecma_extend(const void *data, size_t nbytes, uint64_t crc) {
    extern uint64_t (*crc64ecma_auto)(const uint8_t *data, size_t nbytes, uint64_t crc);
    return crc64ecma_auto((uint8_t*)data, nbytes, crc);
}

inline uint64_t crc64ecma_extend(std::string_view text, uint64_t crc) {
    return crc64ecma_extend(text.data(), text.size(), crc);
}

inline uint32_t crc64ecma(std::string_view text) {
    return crc64ecma_extend(text, 0);
}

inline uint64_t crc64ecma(const void *buffer, size_t nbytes, uint64_t crc) {
    return crc64ecma_extend(buffer, nbytes, crc);
}

inline bool is_crc64ecma_hw_available() {
    extern uint64_t (*crc64ecma_auto)(const uint8_t *data, size_t nbytes, uint64_t crc);
    return crc64ecma_auto != crc64ecma_sw;
}

void crc64ecma_series_sw(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint64_t* crc_parts);
void crc64ecma_series_hw(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint64_t* crc_parts);
inline void crc64ecma_series(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint64_t* crc_parts) {
    extern void (*crc64ecma_series_auto)(const uint8_t *buffer, uint32_t part_size, uint32_t n_parts, uint64_t* crc_parts);
    crc64ecma_series_auto(buffer, part_size, n_parts, crc_parts);
}


uint64_t crc64ecma_combine_sw(uint64_t crc1, uint64_t crc2, uint32_t len2);
uint64_t crc64ecma_combine_hw(uint64_t crc1, uint64_t crc2, uint32_t len2);
inline uint64_t crc64ecma_combine(uint64_t crc1, uint64_t crc2, uint32_t len2) {
    extern uint64_t (*crc64ecma_combine_auto)(uint64_t crc1, uint64_t crc2, uint32_t len2);
    return crc64ecma_combine_auto(crc1, crc2, len2);
}

// combine a series of CRC32C values of a fixed part size
uint64_t crc64ecma_combine_series_sw(uint64_t* crc, uint32_t part_size, uint32_t n_parts);
uint64_t crc64ecma_combine_series_hw(uint64_t* crc, uint32_t part_size, uint32_t n_parts);
inline uint32_t crc64ecma_combine_series(uint64_t* crc, uint32_t part_size, uint32_t n_parts) {
    extern uint64_t (*crc64ecma_combine_series_auto)(uint64_t* crc, uint32_t part_size, uint32_t n_parts);
    return crc64ecma_combine_series_auto(crc, part_size, n_parts);
}
