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
extern blk64_t calculate_minimum_resize_size(ext2_filsys fs, int flags);
extern void initialize_ext2_error_table(void);
}

namespace photon {
namespace fs {

extern io_manager new_io_manager(photon::fs::IFile *file);

errcode_t resize_extfs(photon::fs::IFile *file, uint64_t *new_size, int flags) {
    if (!new_size)
        LOG_ERROR_RETURN(EINVAL, EXT2_ET_INVALID_ARGUMENT, "new_size is null");

    // register once so error_message() can decode ext2 errcodes
    static bool once = []{ initialize_ext2_error_table(); return true; }();
    (void)once;

    auto manager = new_io_manager(file);

    ext2_filsys fs;
    errcode_t ret = ext2fs_open2("virtual-dev", NULL,
                                  EXT2_FLAG_RW | EXT2_FLAG_64BITS,
                                  0, 0, manager, &fs);
    if (ret)
        LOG_ERROR_RETURN(0, ret, "ext2fs_open2 failed, ret=`", ret);

    fs->default_bitmap_type = EXT2FS_BMAP64_RBTREE;
    fs->io->manager->zeroout = NULL;

    auto blocksize = fs->blocksize;
    blk64_t blks = *new_size / blocksize;
    ret = resize_fs(fs, &blks, flags, NULL);
    if (ret) {
        if (ret == EXT2_ET_TOOSMALL || ret == EXT2_ET_BLOCK_ALLOC_FAIL) {
            // some resize_fs failure paths leave blks unchanged; estimate the
            // minimum size in that case
            if (blks == *new_size / blocksize)
                blks = calculate_minimum_resize_size(fs, flags);
            *new_size = (uint64_t)blks * blocksize;
            LOG_ERROR("resize_fs failed, ret=` (`), minimum size required: ` MB (` blocks)",
                      ret, error_message(ret), *new_size >> 20, blks);
        } else {
            LOG_ERROR("resize_fs failed, ret=` (`)", ret, error_message(ret));
        }
        // resize_fs frees fs on success, but leaves it to us on failure
        ext2fs_close_free(&fs);
        return ret;
    }
    // resize_fs has already freed fs on success, so use the cached blocksize
    *new_size = (uint64_t)blks * blocksize;
    return 0;
}

}
}
