/*
Copyright 2023 The Photon Authors

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
#include <photon/fs/filesystem.h>
#include <stdint.h>
#ifdef PHOTON_ENABLE_RESIZE
#include <ext2fs/ext2fs.h>
#endif

namespace photon {
namespace fs {


// when buffer is true, defaults to 8MB buffer size and 128KB block size
photon::fs::IFileSystem *new_extfs(photon::fs::IFile *file, bool buffer = true);

photon::fs::IFileSystem *new_extfs_with_buffer(photon::fs::IFile *file,
                                               uint32_t buffer_size = 8 << 20,
                                               uint32_t block_size = 128 << 10);

// make extfs on an prezeroed IFile,
// should be truncated to specified size in advance
int make_extfs(photon::fs::IFile *file, char *uuid = nullptr);

#ifdef PHOTON_ENABLE_RESIZE
// Offline resize of an extfs image. Not transactional: on failure the image
// may be partially updated; run fsck_extfs() on it or discard it.
// new_size (in/out, in bytes):
//   - in: the target size
//   - out on success: the actual size, possibly rounded down to a block-group
//     boundary
//   - out on EXT2_ET_TOOSMALL / EXT2_ET_BLOCK_ALLOC_FAIL: the minimum size
//     required (a lower-bound estimate)
// flags: only RESIZE_ENABLE_64BIT and RESIZE_DISABLE_64BIT are meaningful;
//   setting both fails with EXT2_ET_INVALID_ARGUMENT
// returns 0 on success, otherwise the errcode_t from resize_fs() unchanged,
//   e.g. EXT2_ET_TOOSMALL, EXT2_ET_BLOCK_ALLOC_FAIL, EXT2_ET_NO_MEMORY,
//   EXT2_ET_BAD_MAGIC, or a positive errno (e.g. EIO) on I/O errors
errcode_t resize_extfs(photon::fs::IFile *file, uint64_t *new_size, int flags = 0);
#endif

}
}
