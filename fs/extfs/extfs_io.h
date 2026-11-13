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

#include <ext2fs/ext2fs.h>
#include <photon/fs/filesystem.h>

namespace photon {
namespace fs {

// Overloads of the libext2fs entry points that take an IFile* backend instead
// of a device path. The device name is derived from the IFile*, so each open is
// self-contained and different images can be opened, resized or fscked
// concurrently on multiple vCPUs without locks, TLS or a registry.
//
// On success the channel is bound to `file` and released by ext2fs_close_free();
// on failure *fs is nullptr. `flags` are EXT2_FLAG_*; superblock/block_size have
// the same meaning as in the corresponding ext2fs_* call. `file` must outlive
// the returned ext2_filsys.
errcode_t ext2fs_open2(IFile* file, int flags, unsigned int superblock,
                      unsigned int block_size, ext2_filsys* fs);

// Same as ext2fs_open(): also reads bitmaps and fixes up the /lost+found
// htree index. Used by the mount path.
errcode_t ext2fs_open(IFile* file, int flags, unsigned int superblock,
                      unsigned int block_size, ext2_filsys* fs);

// Same as ext2fs_initialize(). Used by make_extfs().
errcode_t ext2fs_initialize(IFile* file, int flags,
                           struct ext2_super_block* param, ext2_filsys* fs);

}
}
