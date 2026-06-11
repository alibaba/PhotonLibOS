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
#include "extfs.h"
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <photon/common/alog.h>

extern "C" {
extern errcode_t resize_fs(ext2_filsys fs, blk64_t *new_size, int flags,
                           errcode_t (*progress)(void*, int, unsigned long, unsigned long));
}

namespace photon {
namespace fs {

extern io_manager new_io_manager(photon::fs::IFile *file);

int resize_extfs(photon::fs::IFile *file, uint64_t new_size, int flags) {
    auto manager = new_io_manager(file);

    ext2_filsys fs;
    errcode_t ret = ext2fs_open2("virtual-dev", NULL,
                                  EXT2_FLAG_RW | EXT2_FLAG_64BITS,
                                  0, 0, manager, &fs);
    if (ret) {
        LOG_ERRNO_RETURN(0, -1, "ext2fs_open2 failed, ret=`", ret);
    }

    fs->default_bitmap_type = EXT2FS_BMAP64_RBTREE;
    fs->io->manager->zeroout = NULL;

    blk64_t blks = new_size / fs->blocksize;
    ret = resize_fs(fs, &blks, flags, NULL);
    if (ret) {
        ext2fs_close_free(&fs);
        LOG_ERRNO_RETURN(0, -1, "resize_fs failed, ret=`", ret);
    }
    ext2fs_close_free(&fs);
    return 0;
}

}
}
