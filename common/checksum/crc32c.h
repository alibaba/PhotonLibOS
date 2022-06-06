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
 * Alibaba Group Inc. 2019 Copyrights
 */

#include <cstdint>
#include <string>

#pragma once


uint32_t crc32c(const void *data, size_t nbytes);
uint32_t crc32c(const std::string &text);

uint32_t crc32c_extend(const void *data, size_t nbytes, uint32_t crc);
uint32_t crc32c_extend(const std::string &text, uint32_t crc);

namespace testing {
uint32_t crc32c_slow(const void *data, size_t nbytes, uint32_t crc);
uint32_t crc32c_fast(const void *data, size_t nbytes, uint32_t crc);
} // namespace testing


