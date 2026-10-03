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

// NBD protocol constants shared between nbd.cpp and its test. Like vduse-uapi.h,
// this header exists because the test exercises the wire protocol directly and
// needs the same magic numbers, option codes, command IDs and error codes that
// the implementation uses. Keeping one copy prevents the two from drifting apart.
//
// Reference: github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md
// All on-wire integers are big-endian; encode()/decode() in each translation
// unit convert with the __builtin_bswap family (little-endian hosts only).

#include <cstdint>

namespace photon {
namespace blk {

// handshake magic values
static constexpr uint64_t NBD_INIT_MAGIC       = 0x4e42444d41474943ull;  // "NBDMAGIC"
static constexpr uint64_t NBD_OPTS_MAGIC       = 0x49484156454f5054ull;  // "IHAVEOPT"
static constexpr uint64_t NBD_REP_MAGIC        = 0x0003e889045565a9ull;
static constexpr uint32_t NBD_REQ_MAGIC        = 0x25609513;
static constexpr uint32_t NBD_SIMPLE_REP_MAGIC = 0x67446698;

// client handshake flags
static constexpr uint32_t NBD_FLAG_C_FIXED_NEWSTYLE = 1u << 0;
static constexpr uint32_t NBD_FLAG_C_NO_ZEROES      = 1u << 1;

// options
static constexpr uint32_t NBD_OPT_EXPORT_NAME = 1;
static constexpr uint32_t NBD_OPT_INFO        = 6;
static constexpr uint32_t NBD_OPT_GO          = 7;

// option replies
static constexpr uint32_t NBD_REP_ACK         = 1;
static constexpr uint32_t NBD_REP_INFO        = 3;
static constexpr uint32_t NBD_REP_ERR_INVALID = (1u << 31) | 3;

// info types
static constexpr uint16_t NBD_INFO_EXPORT = 0;

// transmission flags; renamed NBD_TRANS_* to avoid <linux/nbd.h> macros of the
// same NBD_FLAG_* names
static constexpr uint16_t NBD_TRANS_READ_ONLY  = 1u << 1;
static constexpr uint16_t NBD_TRANS_SEND_FLUSH = 1u << 2;
static constexpr uint16_t NBD_TRANS_SEND_FUA   = 1u << 3;
#ifdef __linux__
static constexpr uint16_t NBD_TRANS_SEND_TRIM         = 1u << 5;
static constexpr uint16_t NBD_TRANS_SEND_WRITE_ZEROES = 1u << 6;
#endif

// commands
static constexpr uint16_t NBD_CMD_READ  = 0;
static constexpr uint16_t NBD_CMD_WRITE = 1;
static constexpr uint16_t NBD_CMD_DISC  = 2;
static constexpr uint16_t NBD_CMD_FLUSH = 3;
#ifdef __linux__
static constexpr uint16_t NBD_CMD_TRIM         = 4;
static constexpr uint16_t NBD_CMD_WRITE_ZEROES = 6;
#endif

// per-request command-flags field (16 bits at request offset 4). Named
// NBD_REQ_* to avoid the <linux/nbd.h> macro.
static constexpr uint16_t NBD_REQ_FUA = 1u << 0;

// error codes
static constexpr uint32_t NBD_SUCCESS = 0;
static constexpr uint32_t NBD_EPERM   = 1;
#ifdef __linux__
// Gated with the command rather than with the other status codes because the
// only case that asks for it is the WRITE_ZEROES one, which is Linux-only
// along with its handler.
static constexpr uint32_t NBD_EIO = 5;
#endif
static constexpr uint32_t NBD_EINVAL  = 22;
static constexpr uint32_t NBD_ENOTSUP = 95;

}  // namespace blk
}  // namespace photon
