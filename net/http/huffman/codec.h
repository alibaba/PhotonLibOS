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

#include <sys/types.h>
#include <inttypes.h>
#include <photon/common/string_view.h>

namespace photon {
namespace net {
namespace http {

size_t huffman_encoded_length(std::string_view src);

ssize_t huffman_encode(std::string_view src, char* dst, const char* dst_end);

ssize_t huffman_decode(std::string_view src, char* dst, const char* dst_end);

inline ssize_t huffman_encode(std::string_view src, char* dst, size_t dst_size) {
    return huffman_encode(src, dst, dst + dst_size);
}

inline ssize_t huffman_decode(std::string_view src, char* dst, size_t dst_size) {
    return huffman_decode(src, dst, dst + dst_size);
}

}
}
}
