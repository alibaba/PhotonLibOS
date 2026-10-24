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

// The VDUSE uapi copies, in a header rather than inside vduse.cpp because the
// test needs them too: its raw registration path -- the one blk itself cannot
// create, used to make an orphan for adoption to find -- issues the same ioctls
// against the same structures. Taking them from <linux/vduse.h> is exactly what
// these copies exist to avoid. That header arrived in the kernel later than many
// build hosts' header packages, so a translation unit that includes it does not
// compile there at all. Sharing one copy is what keeps the test and the
// implementation from drifting apart, and keeps the static_asserts in one place.

#include <sys/ioctl.h>      // _IO, _IOR, _IOW, _IOWR
#include <cstddef>          // offsetof
#include <cstdint>

namespace photon {
namespace blk {

// ----------------------------------------------------------------------------
// VDUSE UAPI subset (verbatim from <linux/vduse.h>)
// ----------------------------------------------------------------------------

#define VDUSE_BASE 0x81

#define VDUSE_API_VERSION   0

#define VDUSE_GET_API_VERSION   _IOR(VDUSE_BASE, 0x00, uint64_t)
#define VDUSE_SET_API_VERSION   _IOW(VDUSE_BASE, 0x01, uint64_t)

#define VDUSE_NAME_MAX 256

struct vduse_dev_config {
    char name[VDUSE_NAME_MAX];
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t features;
    uint32_t vq_num;
    uint32_t vq_align;
    uint32_t ngroups;   // api version >= 1 only
    uint32_t nas;       // api version >= 1 only
    uint32_t reserved[11];
    uint32_t config_size;
    uint8_t config[];
};

#define VDUSE_CREATE_DEV      _IOW(VDUSE_BASE, 0x02, struct vduse_dev_config)
#define VDUSE_DESTROY_DEV     _IOW(VDUSE_BASE, 0x03, char[VDUSE_NAME_MAX])

struct vduse_iotlb_entry {
    uint64_t offset;    // mmap offset on the returned fd
    uint64_t start;
    uint64_t last;
#define VDUSE_ACCESS_RO 0x1
#define VDUSE_ACCESS_WO 0x2
#define VDUSE_ACCESS_RW 0x3
    uint8_t perm;
};
#define VDUSE_IOTLB_GET_FD    _IOWR(VDUSE_BASE, 0x10, struct vduse_iotlb_entry)

#define VDUSE_DEV_GET_FEATURES    _IOR(VDUSE_BASE, 0x11, uint64_t)

struct vduse_config_data {
    uint32_t offset;
    uint32_t length;
    uint8_t buffer[];
};
#define VDUSE_DEV_SET_CONFIG          _IOW(VDUSE_BASE, 0x12, struct vduse_config_data)
#define VDUSE_DEV_INJECT_CONFIG_IRQ   _IO(VDUSE_BASE, 0x13)

struct vduse_vq_config {
    uint32_t index;
    uint16_t max_size;
    uint16_t reserved1;
    uint32_t group;
    uint16_t reserved2[10];
};
#define VDUSE_VQ_SETUP    _IOW(VDUSE_BASE, 0x14, struct vduse_vq_config)

struct vduse_vq_state_split  { uint16_t avail_index; };
struct vduse_vq_state_packed { uint16_t last_avail_counter, last_avail_idx,
                                       last_used_counter, last_used_idx; };

struct vduse_vq_info {
    uint32_t index;
    uint32_t num;
    uint64_t desc_addr;
    uint64_t driver_addr;    // the avail ring
    uint64_t device_addr;    // the used ring
    union {
        struct vduse_vq_state_split split;
        struct vduse_vq_state_packed packed;
    };
    uint8_t ready;
};
#define VDUSE_VQ_GET_INFO   _IOWR(VDUSE_BASE, 0x15, struct vduse_vq_info)

struct vduse_vq_eventfd {
    uint32_t index;
#define VDUSE_EVENTFD_DEASSIGN -1
    int fd;
};
#define VDUSE_VQ_SETUP_KICKFD   _IOW(VDUSE_BASE, 0x16, struct vduse_vq_eventfd)
#define VDUSE_VQ_INJECT_IRQ     _IOW(VDUSE_BASE, 0x17, uint32_t)

enum vduse_req_type {
    VDUSE_GET_VQ_STATE,
    VDUSE_SET_STATUS,
    VDUSE_UPDATE_IOTLB,
    VDUSE_SET_VQ_GROUP_ASID,
};

struct vduse_vq_state {
    uint32_t index;
    union {
        struct vduse_vq_state_split split;
        struct vduse_vq_state_packed packed;
    };
};
struct vduse_dev_status { uint8_t status; };
struct vduse_iova_range { uint64_t start, last; };

struct vduse_dev_request {
    uint32_t type;
    uint32_t request_id;
    uint32_t reserved[4];
    union {
        struct vduse_vq_state vq_state;
        struct vduse_dev_status s;
        struct vduse_iova_range iova;
        uint32_t padding[32];
    };
};

struct vduse_dev_response {
    uint32_t request_id;
#define VDUSE_REQ_RESULT_OK     0x00
#define VDUSE_REQ_RESULT_FAILED 0x01
    uint32_t result;
    uint32_t reserved[4];    // the kernel rejects a nonzero reserved area
    union {
        struct vduse_vq_state vq_state;
        uint32_t padding[32];
    };
};

// The five structs below are the ones the multiqueue path reads an `index` out of.
// They are hand-copied from <linux/vduse.h> because that header is too new for
// many build hosts (the same reason ublk.cpp copies <linux/ublk_cmd.h>), and a
// copied struct proves nothing on its own: a transposed field or a wrong width
// compiles clean and only shows up as a mis-directed ioctl once a queue index
// other than 0 is in play. These pin what we actually read. They check that WE
// copied correctly -- deliberately NOT a cross-check against the kernel header,
// which would need a C translation unit (vduse.h is C++-clean, unlike tcmu's).
static_assert(sizeof(vduse_dev_config) == 336, "vduse_dev_config size");
static_assert(offsetof(vduse_dev_config, features) == 264, "vduse_dev_config features offset");
static_assert(offsetof(vduse_dev_config, vq_num) == 272, "vduse_dev_config vq_num offset");
static_assert(offsetof(vduse_dev_config, config_size) == 332, "vduse_dev_config config_size offset");

static_assert(sizeof(vduse_vq_config) == 32, "vduse_vq_config size");
static_assert(offsetof(vduse_vq_config, index) == 0, "vduse_vq_config index offset");
static_assert(offsetof(vduse_vq_config, max_size) == 4, "vduse_vq_config max_size offset");

static_assert(sizeof(vduse_vq_info) == 48, "vduse_vq_info size");
static_assert(offsetof(vduse_vq_info, index) == 0, "vduse_vq_info index offset");
static_assert(offsetof(vduse_vq_info, num) == 4, "vduse_vq_info num offset");
static_assert(offsetof(vduse_vq_info, desc_addr) == 8, "vduse_vq_info desc_addr offset");
static_assert(offsetof(vduse_vq_info, driver_addr) == 16, "vduse_vq_info driver_addr offset");
static_assert(offsetof(vduse_vq_info, device_addr) == 24, "vduse_vq_info device_addr offset");
static_assert(offsetof(vduse_vq_info, ready) == 40, "vduse_vq_info ready offset");

static_assert(sizeof(vduse_vq_eventfd) == 8, "vduse_vq_eventfd size");
static_assert(offsetof(vduse_vq_eventfd, index) == 0, "vduse_vq_eventfd index offset");
static_assert(offsetof(vduse_vq_eventfd, fd) == 4, "vduse_vq_eventfd fd offset");

static_assert(sizeof(vduse_vq_state) == 12, "vduse_vq_state size");
static_assert(offsetof(vduse_vq_state, index) == 0, "vduse_vq_state index offset");

// read today, asserted here so the multiqueue change is not the first to depend
// on an unpinned layout
static_assert(sizeof(vduse_iotlb_entry) == 32, "vduse_iotlb_entry size");
static_assert(offsetof(vduse_iotlb_entry, offset) == 0, "vduse_iotlb_entry offset field");
static_assert(offsetof(vduse_iotlb_entry, start) == 8, "vduse_iotlb_entry start offset");
static_assert(offsetof(vduse_iotlb_entry, last) == 16, "vduse_iotlb_entry last offset");
static_assert(offsetof(vduse_iotlb_entry, perm) == 24, "vduse_iotlb_entry perm offset");
static_assert(sizeof(vduse_iova_range) == 16, "vduse_iova_range size");
static_assert(sizeof(vduse_dev_status) == 1, "vduse_dev_status size");
static_assert(sizeof(vduse_config_data) == 8, "vduse_config_data size");

// ----------------------------------------------------------------------------

}  // namespace blk
}  // namespace photon
