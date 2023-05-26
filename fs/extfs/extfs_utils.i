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
#include <fcntl.h>
#include <dirent.h>
#include <vector>
#include <ext2fs/ext2fs.h>
#include <photon/common/alog.h>

static int __parse_extfs_error(ext2_filsys fs, errcode_t err, ext2_ino_t ino, std::string &err_msg);

// parse_extfs_error parse extfs error to unix error code
// print ERROR log for unclassified extfs error
#define parse_extfs_error(fs, ino, err) ({                                                      \
    std::string __msg;                                                                          \
    int __err = __parse_extfs_error((fs), (err), (ino), __msg);                                 \
    if (__msg != "") {                                                                          \
        if (ino) {                                                                              \
            LOG_ERROR("unclassified ext2fs error: `:` (inode #`)", err, __msg.c_str(), ino);    \
        } else {                                                                                \
            LOG_ERROR("unclassified ext2fs error: `:`", err, __msg.c_str());                    \
        }                                                                                       \
    }                                                                                           \
    __err;                                                                                      \
})

static inline void increment_version(struct ext2_inode *inode) {
    inode->osd1.linux1.l_i_version++;
}

static inline void get_now(struct timespec *now) {
#ifdef CLOCK_REALTIME
    if (!clock_gettime(CLOCK_REALTIME, now))
        return;
#endif
    now->tv_sec = time(NULL);
    now->tv_nsec = 0;
}

#define EXT_ATIME 1
#define EXT_CTIME 2
#define EXT_MTIME 4

static int update_xtime(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *pinode,
                        int flags, struct timespec *file_time = nullptr) {
#ifndef NO_TIMESTAMP
    errcode_t ret = 0;
    struct ext2_inode inode, *pino;

    if (pinode) {
        pino = pinode;
    } else {
        memset(&inode, 0, sizeof(inode));
        ret = ext2fs_read_inode(fs, ino, &inode);
        if (ret)
            return parse_extfs_error(fs, ino, ret);
        pino = &inode;
    }

    struct timespec now;
    if (!file_time) {
        get_now(&now);
    } else {
        now = *file_time;
    }

    if (flags & EXT_ATIME) pino->i_atime = now.tv_sec;
    if (flags & EXT_CTIME) pino->i_ctime = now.tv_sec;
    if (flags & EXT_MTIME) pino->i_mtime = now.tv_sec;
    increment_version(pino);

    if (!pinode) {
        ret = ext2fs_write_inode(fs, ino, &inode);
        if (ret)
            return parse_extfs_error(fs, ino, ret);
    }
#endif
    return 0;
}

static int update_xtime(ext2_file_t file, int flags, struct timespec *file_time = nullptr) {
#ifndef NO_TIMESTAMP
    errcode_t ret = 0;
    ext2_filsys fs = ext2fs_file_get_fs(file);
    ext2_ino_t ino = ext2fs_file_get_inode_num(file);
    struct ext2_inode *inode = ext2fs_file_get_inode(file);

    ret = ext2fs_read_inode(fs, ino, inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    ret = update_xtime(fs, ino, inode, flags, file_time);
    if (ret) return ret;

    ret = ext2fs_write_inode(fs, ino, inode);
    if (ret) return parse_extfs_error(fs, ino, ret);
#endif
    return 0;
}

static int ext2_file_type(unsigned int mode) {
    switch (mode & LINUX_S_IFMT) {
        case LINUX_S_IFLNK:  return EXT2_FT_SYMLINK;
        case LINUX_S_IFREG:  return EXT2_FT_REG_FILE;
        case LINUX_S_IFDIR:  return EXT2_FT_DIR;
        case LINUX_S_IFCHR:  return EXT2_FT_CHRDEV;
        case LINUX_S_IFBLK:  return EXT2_FT_BLKDEV;
        case LINUX_S_IFIFO:  return EXT2_FT_FIFO;
        case LINUX_S_IFSOCK: return EXT2_FT_SOCK;
        default:             return EXT2_FT_UNKNOWN;
    }
}

static unsigned int extfs_open_flags(unsigned int flags) {
    unsigned int ret = 0;
    if (flags & (O_WRONLY | O_RDWR)) ret |= EXT2_FILE_WRITE;
    if (flags & O_CREAT) ret |= EXT2_FILE_CREATE;
    return ret;
}

static ext2_ino_t string_to_inode(ext2_filsys fs, const char *str, int follow, bool release = false);

static int unlink_file_by_name(ext2_filsys fs, const char *path) {
    errcode_t ret = 0;
    ext2_ino_t ino;

    DEFER(LOG_DEBUG("unlink ", VALUE(path), VALUE(ino), VALUE(ret)));
    char *filename = strdup(path);
    DEFER(free(filename););
    char *base_name;

    base_name = strrchr(filename, '/');
    if (base_name) {
        *base_name++ = '\0';
        ino = string_to_inode(fs, filename, 0);
        if (ino == 0) {
            return -(ret = ENOENT);
        }
    } else {
        ino = EXT2_ROOT_INO;
        base_name = filename;
    }

    ret = ext2fs_unlink(fs, ino, base_name, 0, 0);
    if (ret) return parse_extfs_error(fs, ino, ret);
    return update_xtime(fs, ino, nullptr, EXT_CTIME | EXT_MTIME);
}

static int remove_inode(ext2_filsys fs, ext2_ino_t ino) {
    errcode_t ret_err = 0;
    errcode_t err = 0;

    DEFER(LOG_DEBUG("remove ", VALUE(ino), VALUE(ret_err)));

    struct ext2_inode_large inode;
    memset(&inode, 0, sizeof(inode));
    ret_err = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (ret_err) return parse_extfs_error(fs, ino, ret_err);

    if (inode.i_links_count == 0) return 0;
    inode.i_links_count--;
    if (inode.i_links_count == 0) {
#ifndef NO_TIMESTAMP
        inode.i_dtime = time(0);
#else
        inode.i_dtime = 0;
#endif
    }

    ret_err = update_xtime(fs, ino, (struct ext2_inode *)&inode, EXT_CTIME);
    if (ret_err) return ret_err;

    if (inode.i_links_count) goto write_out;

    /* Nobody holds this file; free its blocks! */
    err = ext2fs_free_ext_attr(fs, ino, &inode);
    if (err) {
        ret_err = parse_extfs_error(fs, ino, err);
        LOG_ERROR("ext2fs_free_ext_attr failed, ret_err: `", ret_err);
        goto write_out;
    }
    if (ext2fs_inode_has_valid_blocks2(fs, (struct ext2_inode *)&inode)) {
        err = ext2fs_punch(fs, ino, (struct ext2_inode *)&inode, NULL, 0, ~0ULL);
        if (err) {
            ret_err = parse_extfs_error(fs, ino, err);
            LOG_ERROR("ext2fs_punch failed, ret_err: `", ret_err);
            goto write_out;
        }
    }
    ext2fs_inode_alloc_stats2(fs, ino, -1, LINUX_S_ISDIR(inode.i_mode));

write_out:
    err = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (err) ret_err = parse_extfs_error(fs, ino, err);
    return ret_err;
}

struct rd_struct {
    ext2_ino_t parent;
    int empty;
};

static int rmdir_proc(
    ext2_ino_t dir,
    int entry,
    struct ext2_dir_entry *dirent,
    int offset,
    int blocksize,
    char *buf,
    void *priv_data) {

    struct rd_struct *rds = (struct rd_struct *)priv_data;

    if (dirent->inode == 0)
        return 0;
    if (((dirent->name_len & 0xFF) == 1) && (dirent->name[0] == '.'))
        return 0;
    if (((dirent->name_len & 0xFF) == 2) && (dirent->name[0] == '.') &&
        (dirent->name[1] == '.')) {
        rds->parent = dirent->inode;
        return 0;
    }
    rds->empty = 0;
    return 0;
}

static int __parse_extfs_error(ext2_filsys fs, errcode_t err, ext2_ino_t ino, std::string &err_msg) {
    if (err < EXT2_ET_BASE) return -err;

    switch (err) {
        case EXT2_ET_NO_MEMORY: case EXT2_ET_TDB_ERR_OOM:
            return -ENOMEM;

        case EXT2_ET_INVALID_ARGUMENT: case EXT2_ET_LLSEEK_FAILED:
            return -EINVAL;

        case EXT2_ET_NO_DIRECTORY:
            return -ENOTDIR;

        case EXT2_ET_FILE_NOT_FOUND:
            return -ENOENT;

        case EXT2_ET_TOOSMALL: case EXT2_ET_BLOCK_ALLOC_FAIL: case EXT2_ET_INODE_ALLOC_FAIL: case EXT2_ET_EA_NO_SPACE:
            return -ENOSPC;

        case EXT2_ET_SYMLINK_LOOP:
            return -EMLINK;

        case EXT2_ET_FILE_TOO_BIG:
            return -EFBIG;

        case EXT2_ET_TDB_ERR_EXISTS: case EXT2_ET_FILE_EXISTS: case EXT2_ET_DIR_EXISTS:
            return -EEXIST;

        case EXT2_ET_MMP_FAILED: case EXT2_ET_MMP_FSCK_ON:
            return -EBUSY;

        case EXT2_ET_EA_KEY_NOT_FOUND:
#ifdef ENODATA
            return -ENODATA;
#else
            return -ENOENT;
#endif
            break;
        /* Sometimes fuse returns a garbage file handle pointer to us... */
        case EXT2_ET_MAGIC_EXT2_FILE:
            return -EFAULT;

        case EXT2_ET_UNIMPLEMENTED:
            return -EOPNOTSUPP;

        case EXT2_ET_FILE_RO:
            return -EACCES;
    }

    // unclassified extfs error
    if (fs) {
        ext2fs_mark_super_dirty(fs);
        ext2fs_flush(fs);
    }
    // decode error message
    switch (err) {
        case EXT2_ET_DIR_NO_SPACE:
            err_msg = "EXT2_ET_DIR_NO_SPACE";
            return -ENOSPC;

        case EXT2_ET_BAD_MAGIC:
            err_msg = "EXT2_ET_BAD_MAGIC";
            return -EIO;

        case EXT2_ET_DIR_CORRUPTED:
            err_msg = "EXT2_ET_DIR_CORRUPTED";
            return -EIO;

        case EXT2_ET_UNEXPECTED_BLOCK_SIZE:
            err_msg = "EXT2_ET_UNEXPECTED_BLOCK_SIZE";
            return -EIO;

        default:
            err_msg = "to be decode";
            return -EIO;
    }
}

struct update_dotdot {
    ext2_ino_t new_dotdot;
};

static int update_dotdot_helper(
    ext2_ino_t dir,
    int entry,
    struct ext2_dir_entry *dirent,
    int offset,
    int blocksize,
    char *buf,
    void *priv_data) {
    struct update_dotdot *ud = (struct update_dotdot *)priv_data;

    if (ext2fs_dirent_name_len(dirent) == 2 &&
        dirent->name[0] == '.' && dirent->name[1] == '.') {
        dirent->inode = ud->new_dotdot;
        return DIRENT_CHANGED | DIRENT_ABORT;
    }

    return 0;
}

static ext2_ino_t get_parent_dir_ino(ext2_filsys fs, const char *path) {
    char *last_slash = strrchr((char *)path, '/');
    if (last_slash == 0) {
        return 0;
    }
    unsigned int parent_len = last_slash - path;
    if (parent_len == 0) {
        return EXT2_ROOT_INO;
    }
    char *parent_path = strndup(path, parent_len);
    ext2_ino_t parent_ino = string_to_inode(fs, parent_path, 0);
    // LOG_DEBUG(VALUE(path), VALUE(parent_path), VALUE(parent_ino));
    free(parent_path);
    return parent_ino;
}

static char *get_filename(const char *path) {
    char *last_slash = strrchr((char *)path, (int)'/');
    if (last_slash == nullptr) {
        return nullptr;
    }
    char *filename = last_slash + 1;
    if (strlen(filename) == 0) {
        return nullptr;
    }
    return filename;
}

static int create_file(ext2_filsys fs, const char *path, unsigned int mode, ext2_ino_t *ino) {
    ext2_ino_t parent;
    errcode_t ret = 0;

    DEFER(LOG_DEBUG("create ", VALUE(path), VALUE(parent), VALUE(*ino), VALUE(ret)));
    parent = get_parent_dir_ino(fs, path);
    if (parent == 0) {
        return -(ret = ENOTDIR);
    }
    ret = ext2fs_new_inode(fs, parent, mode, 0, ino);
    if (ret) {
        return parse_extfs_error(fs, parent, ret);
    }
    char *filename = get_filename(path);
    if (filename == nullptr) {
        // This should never happen.
        return -(ret = EISDIR);
    }
    ret = ext2fs_link(fs, parent, filename, *ino, EXT2_FT_REG_FILE);
    if (ret == EXT2_ET_DIR_NO_SPACE) {
        ret = ext2fs_expand_dir(fs, parent);
        if (ret) return parse_extfs_error(fs, parent, ret);
        ret = ext2fs_link(fs, parent, filename, *ino, EXT2_FT_REG_FILE);
    }
    if (ret) return parse_extfs_error(fs, parent, ret);
    if (ext2fs_test_inode_bitmap2(fs->inode_map, *ino)) {
        LOG_WARN("inode already set ", VALUE(*ino));
    }
    ext2fs_inode_alloc_stats2(fs, *ino, +1, 0);

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = (mode & ~LINUX_S_IFMT) | LINUX_S_IFREG;
#ifndef NO_TIMESTAMP
    inode.i_atime = inode.i_ctime = inode.i_mtime = time(0);
#endif
    inode.i_links_count = 1;
    ret = ext2fs_inode_size_set(fs, &inode, 0);  // TODO: update size? also on write?
    if (ret) return parse_extfs_error(fs, 0, ret);
    if (ext2fs_has_feature_inline_data(fs->super)) {
        inode.i_flags |= EXT4_INLINE_DATA_FL;
    } else if (ext2fs_has_feature_extents(fs->super)) {
        ext2_extent_handle_t handle;
        inode.i_flags &= ~EXT4_EXTENTS_FL;
        ret = ext2fs_extent_open2(fs, *ino, &inode, &handle);
        if (ret) return parse_extfs_error(fs, 0, ret);
        ext2fs_extent_free(handle);
    }
    ret = ext2fs_write_new_inode(fs, *ino, &inode);
    if (ret) return parse_extfs_error(fs, 0, ret);
    if (inode.i_flags & EXT4_INLINE_DATA_FL) {
        ret = ext2fs_inline_data_init(fs, *ino);
        if (ret) return parse_extfs_error(fs, 0, ret);
    }
    return 0;
}

static unsigned char ext2_file_type_to_d_type(int type) {
    switch (type) {
        case EXT2_FT_UNKNOWN:
            return DT_UNKNOWN;
        case EXT2_FT_REG_FILE:
            return DT_REG;
        case EXT2_FT_DIR:
            return DT_DIR;
        case EXT2_FT_CHRDEV:
            return DT_CHR;
        case EXT2_FT_BLKDEV:
            return DT_BLK;
        case EXT2_FT_FIFO:
            return DT_FIFO;
        case EXT2_FT_SOCK:
            return DT_SOCK;
        case EXT2_FT_SYMLINK:
            return DT_LNK;
        default:
            return DT_UNKNOWN;
    }
}

static int array_push_dirent(std::vector<dirent> *dirs, struct ext2_dir_entry *dir, size_t len) {
    struct dirent tmpdir;
    tmpdir.d_ino = (ino_t)dir->inode;
    tmpdir.d_off = 0;  // ?
    tmpdir.d_reclen = dir->rec_len;
    tmpdir.d_type = ext2_file_type_to_d_type(ext2fs_dirent_file_type(dir));
    memset(tmpdir.d_name, 0, sizeof(tmpdir.d_name));
    memcpy(tmpdir.d_name, dir->name, len);
    dirs->emplace_back(tmpdir);
    LOG_DEBUG(VALUE(tmpdir.d_ino), VALUE(tmpdir.d_reclen), VALUE(tmpdir.d_type), VALUE(tmpdir.d_name), VALUE(len));
    return 0;
}

static int copy_dirent_to_result(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *priv_data) {
    size_t len = ext2fs_dirent_name_len(dirent);
    if ((strncmp(dirent->name, ".", len) != 0) &&
        (strncmp(dirent->name, "..", len) != 0)) {
        array_push_dirent((std::vector<::dirent> *)priv_data, dirent, len);
    }
    return 0;
}

static blkcnt_t blocks_from_inode(ext2_filsys fs, struct ext2_inode *inode) {
    blk64_t	ret = inode->i_blocks;

	if (ext2fs_has_feature_huge_file(fs->super)) {
		ret += ((long long) inode->osd2.linux2.l_i_blocks_hi) << 32;
		if (inode->i_flags & EXT4_HUGE_FILE_FL)
			ret *= (fs->blocksize / 512);
	}
	return ret;
}

static int is_fast_symlink(struct ext2_inode *inode) {
	return LINUX_S_ISLNK(inode->i_mode) && EXT2_I_SIZE(inode) &&
	       EXT2_I_SIZE(inode) < sizeof(inode->i_block);
}