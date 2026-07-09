/*
Copyright 2025 The Photon Authors

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

uint32_t xxhash32_sw(const uint8_t* data, size_t len, uint32_t seed = 0);

inline uint32_t xxhash32(const uint8_t* data, size_t len, uint32_t seed = 0) {
    return xxhash32_sw(data, len, seed);
}

