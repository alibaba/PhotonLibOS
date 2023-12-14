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
#include <limits.h>
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <photon/photon.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/localfs.h>
#include <photon/common/alog.h>
#include <photon/common/uuid4.h>
#include <string>
#include <fcntl.h>

constexpr char DEFAULT_UUID[] = "bdf7bb2e-c231-43ce-87c2-photonextdev";

int mkdir_lost_found(ext2_filsys fs) {
    std::string name = "lost+found";
    fs->umask = 077;
    ext2_ino_t ret = ext2fs_mkdir(fs, EXT2_ROOT_INO, 0, name.c_str());
    if (ret) {
        LOG_ERRNO_RETURN(0, -1, "error mkdir for /lost+found ", VALUE(ret));
    }
    ext2_ino_t ino;
    ret = ext2fs_lookup(fs, EXT2_ROOT_INO, name.c_str(), name.length(), 0, &ino);
    if (ret) {
        LOG_ERRNO_RETURN(0, -1, "error looking up for /lost+found ", VALUE(ret));
    }
    // expand twice ensure at least 2 blocks
    for (int i = 0; i < 2; i++) {
        ret = ext2fs_expand_dir(fs, ino);
        if (ret) {
            LOG_ERRNO_RETURN(0, -1, "error expanding dir for /lost+found ", VALUE(ret));
        }
    }
    return 0;
}

int do_mkfs(io_manager manager, size_t size, char *uuid) {
    int blocksize = 4096;
    blk64_t blocks_count = size / blocksize;
    int inode_ratio = 16384;
    int inode_size = 256;
    double reserved_ratio = 0.05;

    struct ext2_super_block fs_param;
    memset(&fs_param, 0, sizeof(struct ext2_super_block));
    fs_param.s_rev_level = 1;

    ext2fs_set_feature_64bit(&fs_param);
    ext2fs_set_feature_sparse_super(&fs_param);
    ext2fs_set_feature_sparse_super2(&fs_param);
    ext2fs_set_feature_filetype(&fs_param);
    ext2fs_set_feature_resize_inode(&fs_param);
    ext2fs_set_feature_dir_index(&fs_param);
    ext2fs_set_feature_xattr(&fs_param);
    ext2fs_set_feature_dir_nlink(&fs_param);
    ext2fs_set_feature_large_file(&fs_param);
    ext2fs_set_feature_huge_file(&fs_param);
    ext2fs_set_feature_flex_bg(&fs_param);
    ext2fs_set_feature_extents(&fs_param);
    ext2fs_set_feature_extra_isize(&fs_param);

    fs_param.s_log_cluster_size = fs_param.s_log_block_size = 2;
    fs_param.s_desc_size = EXT2_MIN_DESC_SIZE_64BIT;
    fs_param.s_log_groups_per_flex = 0;
    fs_param.s_inode_size = inode_size;

    ext2fs_blocks_count_set(&fs_param, blocks_count);
    unsigned long long n = ext2fs_blocks_count(&fs_param) * blocksize / inode_ratio;
    fs_param.s_inodes_count = (n > UINT_MAX) ? UINT_MAX : n;

    ext2fs_r_blocks_count_set(&fs_param, reserved_ratio * ext2fs_blocks_count(&fs_param));
    fs_param.s_backup_bgs[0] = 1;
    fs_param.s_backup_bgs[1] = ~0;

    ext2_filsys fs;
    // init superblock
    errcode_t ret = ext2fs_initialize("virtual-dev", 0, &fs_param, manager, &fs);
    if (ret) {
        LOG_ERRNO_RETURN(0, -1, "error while setting up superblock ", VALUE(ret));
    }

    uuid4_parse(uuid, (char*)(fs->super->s_uuid));
    uuid4_parse(uuid, (char*)(fs_param.s_hash_seed));
    fs->super->s_kbytes_written = 1;
    fs->super->s_def_hash_version = EXT2_HASH_HALF_MD4;
    fs->super->s_max_mnt_count = -1;
    fs->stride = fs->super->s_raid_stride;
    // alloc tables
    ret = ext2fs_allocate_tables(fs);
    if (ret) {
        LOG_ERRNO_RETURN(0, -1, "error while allocating tables ", VALUE(ret));
    }
    // create root dir
    ret = ext2fs_mkdir(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, 0);
    if (ret) {
        LOG_ERRNO_RETURN(0, -1, "error make root dir ", VALUE(ret));
    }
    // mkdir for lost+found
    ret = mkdir_lost_found(fs);
    if (ret) {
        return ret;
    }
    // reserve inodes
    ext2fs_inode_alloc_stats2(fs, EXT2_BAD_INO, +1, 0);
    for (ext2_ino_t i = EXT2_ROOT_INO + 1; i < EXT2_FIRST_INODE(fs->super); i++)
        ext2fs_inode_alloc_stats2(fs, i, +1, 0);
    ext2fs_mark_ib_dirty(fs);
    // create resize inode
    ret = ext2fs_create_resize_inode(fs);
    if (ret) {
        LOG_ERRNO_RETURN(0, -1, "error creating resize inode ", VALUE(ret));
    }

    ret = ext2fs_close_free(&fs);
    if (ret) {
        LOG_ERRNO_RETURN(0, -1, "error closing fs ", VALUE(ret));
    }
    return 0;
}

namespace photon {
namespace fs {

extern io_manager new_io_manager(photon::fs::IFile *file);

int make_extfs(photon::fs::IFile *file, char *uuid) {
    struct stat st;
    auto ret = file->fstat(&st);
    if (ret) return ret;
    auto manager = new_io_manager(file);
    if (uuid == nullptr) {
        uuid = (char *)DEFAULT_UUID;
    }
    ret = do_mkfs(manager, st.st_size, uuid);
    return ret;
}

}
}
