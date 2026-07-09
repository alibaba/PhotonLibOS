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

#include <cstddef>
#include <cstdint>
#include <photon/common/object.h>

namespace photon {
namespace qat {

class ICodec : public Object {
public:
    struct Buffer {
        void* addr;
        ssize_t size;
    };

    // single-buffer (de)compression, returns resulting size after the operation, -1 for failure
    virtual int   compress(Buffer src, Buffer dst) = 0;
    virtual int decompress(Buffer src, Buffer dst) = 0;

    // multi-buffer (de)compression, returns 0 for success, -1 for failure
    // stores i-th resulting size after the operation in `dst[i].size`
    virtual int   compress(Buffer* src, Buffer* dst, size_t n) = 0;
    virtual int decompress(Buffer* src, Buffer* dst, size_t n) = 0;
};

/**
 * @brief Create a QAT LZ4 hardware-accelerated codec.
 *
 * Allocates and initializes a QAT LZ4 codec. The returned ICodec
 * supports concurrent use from multiple photon threads.
 * Caller must delete the returned object when done.
 *
 * @param device_id QAT device ID, -1 for auto-select
 * @param comp_level LZ4 compression level 1-12 (default 4)
 * @param block_cksum Enable per-block xxhash32 checksum (default true)
 * @param content_cksum Enable per-frame content checksum (default false).
 *        When block_cksum is on, content_cksum is considered off.
 * @return ICodec* on success, nullptr on error (errno set)
 */
ICodec* new_lz4_codec(int device_id = -1, int comp_level = 4,
                       bool block_cksum = true, bool content_cksum = false);

}  // namespace qat
}  // namespace photon
