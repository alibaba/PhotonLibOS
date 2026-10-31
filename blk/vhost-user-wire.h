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

// Vhost-user wire protocol definitions, in a header rather than inside
// vhost-user.cpp because the test needs them too: the mock frontend in
// test-vhost-user.cpp is an independent implementation of the OTHER end of
// the same protocol, and it used to keep its own renamed copy of every
// constant and struct here. Two copies of a wire format that must agree
// byte-for-byte is exactly how a suite stays green against a format no real
// peer speaks -- the mock and the backend drifted apart silently, each
// consistent with itself and wrong against the world. Sharing one copy is
// what keeps the test and the implementation from drifting apart, and keeps
// the static_asserts in one place.
//
// Source of truth: QEMU docs/interop/vhost-user.rst (the protocol's
// specification document) and <linux/vhost_types.h>.

#include <cstddef>      // offsetof
#include <cstdint>

namespace photon {
namespace blk {

// ----------------------------------------------------------------------------
// vhost-user message IDs (from vhost-user.rst)
// ----------------------------------------------------------------------------

enum : int32_t {
    VHOST_USER_NONE = 0,
    VHOST_USER_GET_FEATURES = 1,
    VHOST_USER_SET_FEATURES = 2,
    VHOST_USER_SET_OWNER = 3,
    VHOST_USER_RESET_OWNER = 4,
    VHOST_USER_SET_MEM_TABLE = 5,
    VHOST_USER_SET_LOG_BASE = 6,
    VHOST_USER_SET_LOG_FD = 7,
    VHOST_USER_SET_VRING_NUM = 8,
    VHOST_USER_SET_VRING_ADDR = 9,
    VHOST_USER_SET_VRING_BASE = 10,
    VHOST_USER_GET_VRING_BASE = 11,
    VHOST_USER_SET_VRING_KICK = 12,
    VHOST_USER_SET_VRING_CALL = 13,
    VHOST_USER_SET_VRING_ERR = 14,
    VHOST_USER_GET_PROTOCOL_FEATURES = 15,
    VHOST_USER_SET_PROTOCOL_FEATURES = 16,
    VHOST_USER_GET_QUEUE_NUM = 17,
    VHOST_USER_SET_VRING_ENABLE = 18,
    VHOST_USER_SEND_RARP = 19,
    VHOST_USER_NET_SET_MTU = 20,
    VHOST_USER_SET_BACKEND_REQ_FD = 21,
    VHOST_USER_IOTLB_MSG = 22,
    VHOST_USER_SET_VRING_ENDIAN = 23,
    VHOST_USER_GET_CONFIG = 24,
    VHOST_USER_SET_CONFIG = 25,
    VHOST_USER_RESET_DEVICE = 34,
    VHOST_USER_VRING_KICK = 35,
    VHOST_USER_GET_MAX_MEM_SLOTS = 36,
    VHOST_USER_ADD_MEM_REG = 37,
    VHOST_USER_REM_MEM_REG = 38,
    VHOST_USER_SET_STATUS = 39,
    VHOST_USER_GET_STATUS = 40,
    // backend -> frontend, on the SET_BACKEND_REQ_FD channel
    VHOST_USER_BACKEND_CONFIG_CHANGE_MSG = 2,
};

// ----------------------------------------------------------------------------
// vhost-user flags and masks
// ----------------------------------------------------------------------------

#define VHOST_USER_VERSION          1
#define VHOST_USER_VERSION_MASK     0x3u
#define VHOST_USER_REPLY_MASK       (0x1u << 2)
#define VHOST_USER_NEED_REPLY_MASK  (0x1u << 3)
#define VHOST_USER_VRING_IDX_MASK   0xFFu
#define VHOST_USER_VRING_NOFD_MASK  0x100u

// ----------------------------------------------------------------------------
// vhost-user protocol features
// ----------------------------------------------------------------------------

// Bit 0 is multiple-queue support. vhost-user.rst states the feature "is
// supported only when the protocol feature VHOST_USER_PROTOCOL_F_MQ (bit 0)
// is set", and GET_QUEUE_NUM is how the primary learns the count.
#define VHOST_USER_PROTOCOL_F_MQ            0
#define VHOST_USER_PROTOCOL_F_REPLY_ACK     3
#define VHOST_USER_PROTOCOL_F_BACKEND_REQ   5
#define VHOST_USER_PROTOCOL_F_CONFIG        9
// Bit 13: the gate on RESET_DEVICE being a valid message at all.
#define VHOST_USER_PROTOCOL_F_RESET_DEVICE  13

// NOT a protocol feature: bit 30 of the DEVICE feature word, and the gate on
// the whole protocol-feature negotiation. vhost-user.rst defines it as the
// bit that "signals back-end support for VHOST_USER_GET_PROTOCOL_FEATURES
// and VHOST_USER_SET_PROTOCOL_FEATURES".
#define VHOST_USER_F_PROTOCOL_FEATURES      30

// ----------------------------------------------------------------------------
// vhost-user wire-format structs (verbatim from <linux/vhost_types.h>)
// ----------------------------------------------------------------------------

struct vhost_user_memory_region {
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;    // the FRONTEND's VA of the region (QVA space)
    uint64_t mmap_offset;
};
struct vhost_user_memory {
    uint32_t nregions;
    uint32_t padding;
    vhost_user_memory_region regions[8];   // VHOST_MEMORY_BASELINE_NREGIONS
};
struct vhost_vring_state { uint32_t index, num; };
struct vhost_vring_addr {
    uint32_t index;
    uint32_t flags;
    uint64_t desc_user_addr;    // QVAs -- translate via the region table
    uint64_t used_user_addr;
    uint64_t avail_user_addr;
    uint64_t log_guest_addr;
};
struct vhost_user_config {
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
    uint8_t region[256];        // VHOST_USER_MAX_CONFIG_SIZE
};

// The wire header is request(4) + flags(4) + size(4) = 12 bytes and the
// payload follows IMMEDIATELY, with no padding -- that is vhost-user.rst's
// message layout. Unpacked, the union's uint64_t would force 8-byte alignment
// and pad the header to 16 -- 4 bytes of garbage on the wire ahead of every
// payload, and a length check no conforming frontend can satisfy. The nested
// types keep their own layouts, which already match the protocol
// (vhost_user_memory's padding field is part of it), so packing the outer
// struct is what aligns us.
// sizeof is deliberately NOT asserted: whether packed propagates into the
// anonymous union differs by compiler, while the payload offset does not.
//
// Access rule: reading a SCALAR member in place is fine -- the compiler knows
// the reduced alignment and emits a load to match. What is NOT fine is taking
// the ADDRESS of a nested struct: that pointer would be under-aligned for its
// type (UB, and -Waddress-of-packed-member). Handlers that need a whole
// nested struct therefore memcpy it out first.
struct __attribute__((packed)) vhost_user_msg {
    int32_t request;
    uint32_t flags;
    uint32_t size;
    union {
        uint64_t u64;
        struct vhost_vring_state state;
        struct vhost_vring_addr addr;
        struct vhost_user_memory memory;
        struct vhost_user_config config;
    } payload;
};

// ----------------------------------------------------------------------------
// static_assert checks -- pinned so that no compiler or platform can silently
// shift a field and break the wire format
// ----------------------------------------------------------------------------

static_assert(offsetof(vhost_user_msg, payload) == 12,
              "vhost-user payload must follow the 12-byte header with no padding");

// The multiqueue path reads these two `index` fields off the wire for the
// first time; both were already parsed and then dropped. They keep their own
// natural layout even nested inside the packed vhost_user_msg (packing the
// outer struct does not repack a named nested type), which is what makes
// in-place scalar reads legal.
static_assert(sizeof(vhost_vring_state) == 8, "vhost_vring_state size");
static_assert(offsetof(vhost_vring_state, index) == 0, "vhost_vring_state index offset");
static_assert(offsetof(vhost_vring_state, num) == 4, "vhost_vring_state num offset");
static_assert(sizeof(vhost_vring_addr) == 40, "vhost_vring_addr size");
static_assert(offsetof(vhost_vring_addr, index) == 0, "vhost_vring_addr index offset");
static_assert(offsetof(vhost_vring_addr, flags) == 4, "vhost_vring_addr flags offset");
static_assert(offsetof(vhost_vring_addr, desc_user_addr) == 8, "vhost_vring_addr desc offset");
static_assert(offsetof(vhost_vring_addr, used_user_addr) == 16, "vhost_vring_addr used offset");
static_assert(offsetof(vhost_vring_addr, avail_user_addr) == 24, "vhost_vring_addr avail offset");

}  // namespace blk
}  // namespace photon
