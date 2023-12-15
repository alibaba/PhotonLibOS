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
#include <ext2fs/ext2fs.h>
#include <utime.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/estring.h>
#include <photon/common/expirecontainer.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/localfs.h>
#include <photon/fs/virtual-file.h>
#include <photon/fs/fiemap.h>
#include "extfs_utils.i"
#include "buffer_file.h"

// add for debug
static uint64_t total_read_cnt = 0;
static uint64_t total_write_cnt = 0;

static ext2_filsys do_ext2fs_open(io_manager extfs_manager) {
    ext2_filsys fs;
    errcode_t ret = ext2fs_open(
        "extfs",
        EXT2_FLAG_RW,         // flags
        0,                    // superblock
        DEFAULT_BLOCK_SIZE,   // block_size
        extfs_manager,        // io manager
        &fs                   // ret_fs
    );
    if (ret) {
        errno = -parse_extfs_error(nullptr, 0, ret);
        LOG_ERRNO_RETURN(0, nullptr, "failed ext2fs_open");
    }
    ret = ext2fs_read_bitmaps(fs);
    if (ret) {
        errno = -parse_extfs_error(fs, 0, ret);
        ext2fs_close(fs);
        LOG_ERRNO_RETURN(0, nullptr, "failed ext2fs_read_bitmaps");
    }
    LOG_INFO("ext2fs opened");
    return fs;
}

static ext2_file_t do_ext2fs_open_file(ext2_filsys fs, const char *path, unsigned int flags, unsigned int mode) {
    DEFER(LOG_DEBUG("open_file" , VALUE(path)));
    ext2_ino_t ino = string_to_inode(fs, path, !(flags & O_NOFOLLOW));
    errcode_t ret;
    if (ino == 0) {
        if (!(flags & O_CREAT)) {
            errno = ENOENT;
            return nullptr;
        }
        ret = create_file(fs, path, mode, &ino);
        if (ret) {
            LOG_ERROR_RETURN(-ret, nullptr, "failed to create file ", VALUE(ret), VALUE(path));
        }
    } else if (flags & O_EXCL) {
        errno = EEXIST;
        return nullptr;
    }
    if ((flags & O_DIRECTORY) && ext2fs_check_directory(fs, ino)) {
        errno = ENOTDIR;
        return nullptr;
    }
    ext2_file_t file;
    ret = ext2fs_file_open(fs, ino, extfs_open_flags(flags), &file);
    if (ret) {
        errno = -parse_extfs_error(fs, ino, ret);
        return nullptr;
    }
    if (flags & O_TRUNC) {
        ret = ext2fs_file_set_size2(file, 0);
        if (ret) {
            errno = -parse_extfs_error(fs, ino, ret);
            return nullptr;
        }
    }
    return file;
}

static int do_ext2fs_file_close(ext2_file_t file) {
    errcode_t ret = ext2fs_file_close(file);
    if (ret) return parse_extfs_error(nullptr, 0, ret);
    return 0;
}

static long do_ext2fs_read(
    ext2_file_t file,
    int flags,
    char *buffer,
    size_t count,  // requested count
    off_t offset  // offset in file, -1 for current offset
) {
    errcode_t ret = 0;
    if ((flags & O_WRONLY) != 0) {
        // Don't try to read write only files.
        return -EBADF;
    }
    if (offset != -1) {
        ret = ext2fs_file_llseek(file, offset, EXT2_SEEK_SET, nullptr);
        if (ret) return parse_extfs_error(nullptr, 0, ret);
    }
    unsigned int got;
    LOG_DEBUG("read ", VALUE(offset), VALUE(count));
    ret = ext2fs_file_read(file, buffer, count, &got);
    if (ret) return parse_extfs_error(nullptr, 0, ret);
    total_read_cnt += got;
    if ((flags & O_NOATIME) == 0) {
        ret = update_xtime(file, EXT_ATIME);
        if (ret) return ret;
    }
    return got;
}

static long do_ext2fs_write(
    ext2_file_t file,
    int flags,
    const char *buffer,
    size_t count,  // requested count
    off_t offset  // offset in file, -1 for current offset
) {
    if ((flags & (O_WRONLY | O_RDWR)) == 0) {
        // Don't try to write to readonly files.
        return -EBADF;
    }
    errcode_t ret = 0;
    if ((flags & O_APPEND) != 0) {
        // append mode: seek to the end before each write
        ret = ext2fs_file_llseek(file, 0, EXT2_SEEK_END, nullptr);
    } else if (offset != -1) {
        ret = ext2fs_file_llseek(file, offset, EXT2_SEEK_SET, nullptr);
    }

    if (ret) return parse_extfs_error(nullptr, 0, ret);
    unsigned int written;
    LOG_DEBUG("write ", VALUE(offset), VALUE(count));
    ret = ext2fs_file_write(file, buffer, count, &written);
    if (ret) return parse_extfs_error(nullptr, 0, ret);
    total_write_cnt += written;
    // update_mtime
    ret = update_xtime(file, EXT_CTIME | EXT_MTIME);
    if (ret) return ret;

    ret = ext2fs_file_flush(file);
    if (ret) {
        return parse_extfs_error(nullptr, 0, ret);
    }

    return written;
}

static int do_ext2fs_chmod(ext2_filsys fs, ext2_ino_t ino, int mode) {
    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));

    errcode_t ret = ext2fs_read_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    // keep only fmt (file or directory)
    inode.i_mode &= LINUX_S_IFMT;
    // apply new mode
    inode.i_mode |= (mode & ~LINUX_S_IFMT);
    increment_version(&inode);

    ret = ext2fs_write_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    return 0;
}

static int do_ext2fs_chmod(ext2_filsys fs, const char *path, int mode) {
    LOG_DEBUG(VALUE(path));
    ext2_ino_t ino = string_to_inode(fs, path, 0);
    if (!ino) return -ENOENT;

    return do_ext2fs_chmod(fs, ino, mode);
}

static int do_ext2fs_chown(ext2_filsys fs, ext2_ino_t ino, int uid, int gid) {
    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));

    // TODO handle 32 bit {u,g}ids
    errcode_t ret = ext2fs_read_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);
    // keep only the lower 16 bits
    inode.i_uid = uid & 0xFFFF;
    ext2fs_set_i_uid_high(inode, uid >> 16);
    inode.i_gid = gid & 0xFFFF;
    ext2fs_set_i_gid_high(inode, gid >> 16);
    increment_version(&inode);

    ret = ext2fs_write_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    return 0;
}

static int do_ext2fs_chown(ext2_filsys fs, const char *path, int uid, int gid, int follow) {
    LOG_DEBUG(VALUE(path));
    ext2_ino_t ino = string_to_inode(fs, path, follow);
    if (!ino) return -ENOENT;

    return do_ext2fs_chown(fs, ino, uid, gid);
}

static int do_ext2fs_utimes(ext2_filsys fs, ext2_ino_t ino, const struct timeval tv[2]) {
    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));

    errcode_t ret = ext2fs_read_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    inode.i_atime = tv[0].tv_sec;
    inode.i_mtime = tv[1].tv_sec;
    inode.i_ctime = tv[1].tv_sec;
    increment_version(&inode);

    ret = ext2fs_write_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    return 0;
}

static int do_ext2fs_utimes(ext2_filsys fs, const char *path, const struct timeval tv[2], int follow) {
    LOG_DEBUG(VALUE(path));
    ext2_ino_t ino = string_to_inode(fs, path, follow);
    if (!ino) return -ENOENT;

    return do_ext2fs_utimes(fs, ino, tv);
}

static int do_ext2fs_unlink(ext2_filsys fs, const char *path) {
    ext2_ino_t ino;
    errcode_t ret = 0;

    DEFER(LOG_DEBUG("unlink ", VALUE(path), VALUE(ino), VALUE(ret)));
    ino = string_to_inode(fs, path, 0, true);
    if (ino == 0) {
        return -(ret = ENOENT);
    }

    if (ext2fs_check_directory(fs, ino) == 0) {
        return -(ret = EISDIR);
    }

    ret = unlink_file_by_name(fs, path);
    if (ret) return ret;

    ret = remove_inode(fs, ino);
    if (ret) return ret;

    return 0;
}

static int do_ext2fs_mkdir(ext2_filsys fs, const char *path, int mode) {
    ext2_ino_t parent, ino;
    errcode_t ret = 0;

    DEFER(LOG_DEBUG("mkdir ", VALUE(path), VALUE(parent), VALUE(ino), VALUE(ret)));
    ino = string_to_inode(fs, path, 0);
    if (ino) {
        return -(ret = EEXIST);
    }
    parent = get_parent_dir_ino(fs, path);
    if (parent == 0) {
        return -(ret = ENOTDIR);
    }
    char *filename = get_filename(path);
    if (filename == nullptr) {
        // This should never happen.
        return -(ret = EISDIR);
    }

    ret = ext2fs_new_inode(fs, parent, LINUX_S_IFDIR, 0, &ino);
    if (ret) return parse_extfs_error(fs, 0, ret);
    ret = ext2fs_mkdir(fs, parent, ino, filename);
    if (ret == EXT2_ET_DIR_NO_SPACE) {
        ret = ext2fs_expand_dir(fs, parent);
        if (ret) return parse_extfs_error(fs, 0, ret);

        ret = ext2fs_mkdir(fs, parent, ino, filename);
    }
    if (ret) return parse_extfs_error(fs, 0, ret);

    struct ext2_inode_large inode;
    memset(&inode, 0, sizeof(inode));
    ret = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (ret) return parse_extfs_error(fs, 0, ret);
    inode.i_mode = (mode & ~LINUX_S_IFMT) | LINUX_S_IFDIR;
    ret = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (ret) return parse_extfs_error(fs, 0, ret);

    return 0;
}

static int do_ext2fs_rmdir(ext2_filsys fs, const char *path) {
    ext2_ino_t ino;
    errcode_t ret = 0;
    struct rd_struct rds;

    DEFER(LOG_DEBUG("rmdir ", VALUE(path), VALUE(ino), VALUE(ret)));
    ino = string_to_inode(fs, path, 0, true);
    if (ino == 0) {
        return -(ret = ENOENT);
    }

    rds.parent = 0;
    rds.empty = 1;

    ret = ext2fs_dir_iterate2(fs, ino, 0, 0, rmdir_proc, &rds);
    if (ret) return parse_extfs_error(fs, ino, ret);

    if (rds.empty == 0) {
        return -(ret = ENOTEMPTY);
    }

    ret = unlink_file_by_name(fs, path);
    if (ret) return ret;
    /* Directories have to be "removed" twice. */
    ret = remove_inode(fs, ino);
    if (ret) return ret;
    ret = remove_inode(fs, ino);
    if (ret) return ret;

    if (rds.parent) {
        struct ext2_inode_large inode;
        memset(&inode, 0, sizeof(inode));
        ret = ext2fs_read_inode_full(fs, rds.parent, (struct ext2_inode *)&inode, sizeof(inode));
        if (ret) return parse_extfs_error(fs, rds.parent, ret);

        if (inode.i_links_count > 1)
            inode.i_links_count--;
        // update_mtime
        ret = update_xtime(fs, rds.parent, (struct ext2_inode *)&inode, EXT_CTIME | EXT_MTIME);
        if (ret) return ret;
        ret = ext2fs_write_inode_full(fs, rds.parent, (struct ext2_inode *)&inode, sizeof(inode));
        if (ret) return parse_extfs_error(fs, rds.parent, ret);
    }

    return 0;
}

static int do_ext2fs_rename(ext2_filsys fs, const char *from, const char *to) {
    errcode_t ret = 0;
    ext2_ino_t from_ino, to_ino, to_dir_ino, from_dir_ino;
    struct ext2_inode inode;
    struct update_dotdot ud;

    DEFER(LOG_DEBUG("rename ", VALUE(from), VALUE(to), VALUE(from_ino), VALUE(to_ino), VALUE(ret)));

    from_ino = string_to_inode(fs, from, 0, true);
    if (from_ino == 0) {
        return -(ret = ENOENT);
    }
    to_ino = string_to_inode(fs, to, 0);
    if (to_ino == 0 && errno != ENOENT) {
        return -(ret = errno);
    }

    /* Already the same file? */
    if (to_ino != 0 && to_ino == from_ino)
        return 0;

    /* Find parent dir of the source and check write access */
    from_dir_ino = get_parent_dir_ino(fs, from);
    if (from_dir_ino == 0) {
        return -(ret = ENOTDIR);
    }

    /* Find parent dir of the destination and check write access */
    to_dir_ino = get_parent_dir_ino(fs, to);
    if (to_dir_ino == 0) {
        return -(ret = ENOTDIR);
    }
    char *filename = get_filename(to);
    if (filename == nullptr) {
        return -(ret = EISDIR);
    }

    /* If the target exists, unlink it first */
    if (to_ino != 0) {
        ret = ext2fs_read_inode(fs, to_ino, &inode);
        if (ret) return parse_extfs_error(fs, to_ino, ret);

        LOG_DEBUG("unlinking ` ino=`", LINUX_S_ISDIR(inode.i_mode) ? "dir" : "file", to_ino);
        if (LINUX_S_ISDIR(inode.i_mode))
            ret = do_ext2fs_rmdir(fs, to);
        else
            ret = do_ext2fs_unlink(fs, to);
        if (ret) return ret;
    }

    /* Get ready to do the move */
    ret = ext2fs_read_inode(fs, from_ino, &inode);
    if (ret) return parse_extfs_error(fs, from_ino, ret);

    /* Link in the new file */
    LOG_DEBUG("linking ino=`/path=` to dir=`", from_ino, filename, to_dir_ino);
    ret = ext2fs_link(fs, to_dir_ino, filename, from_ino, ext2_file_type(inode.i_mode));
    if (ret == EXT2_ET_DIR_NO_SPACE) {
        ret = ext2fs_expand_dir(fs, to_dir_ino);
        if (ret) return parse_extfs_error(fs, to_dir_ino, ret);

        ret = ext2fs_link(fs, to_dir_ino, filename, from_ino, ext2_file_type(inode.i_mode));
    }
    if (ret) return parse_extfs_error(fs, to_dir_ino, ret);

    /* Update '..' pointer if dir */
    ret = ext2fs_read_inode(fs, from_ino, &inode);
    if (ret) return parse_extfs_error(fs, from_ino, ret);

    if (LINUX_S_ISDIR(inode.i_mode)) {
        ud.new_dotdot = to_dir_ino;
        LOG_DEBUG("updating .. entry for dir=`", to_dir_ino);
        ret = ext2fs_dir_iterate2(fs, from_ino, 0, nullptr, update_dotdot_helper, &ud);
        if (ret) return parse_extfs_error(fs, from_ino, ret);

        /* Decrease from_dir_ino's links_count */
        LOG_DEBUG("moving linkcount from dir=` to dir=`", from_dir_ino, to_dir_ino);
        ret = ext2fs_read_inode(fs, from_dir_ino, &inode);
        if (ret) return parse_extfs_error(fs, from_dir_ino, ret);
        inode.i_links_count--;
        ret = ext2fs_write_inode(fs, from_dir_ino, &inode);
        if (ret) return parse_extfs_error(fs, from_dir_ino, ret);

        /* Increase to_dir_ino's links_count */
        ret = ext2fs_read_inode(fs, to_dir_ino, &inode);
        if (ret) return parse_extfs_error(fs, to_dir_ino, ret);
        inode.i_links_count++;
        ret = ext2fs_write_inode(fs, to_dir_ino, &inode);
        if (ret) return parse_extfs_error(fs, to_dir_ino, ret);
    }

    /* Update timestamps */
    // update_ctime
    ret = update_xtime(fs, from_ino, nullptr, EXT_CTIME);
    if (ret) return ret;
    // update_mtime
    ret = update_xtime(fs, to_dir_ino, nullptr, EXT_CTIME | EXT_MTIME);
    if (ret) return ret;

    /* Remove the old file */
    ret = unlink_file_by_name(fs, from);
    if (ret) return ret;

    /* Flush the whole mess out */
    ret = ext2fs_flush2(fs, 0);
    if (ret) return parse_extfs_error(fs, 0, ret);

    return 0;
}

static int do_ext2fs_link(ext2_filsys fs, const char *src, const char *dest) {
    errcode_t ret = 0;
    ext2_ino_t parent, ino;

    DEFER(LOG_DEBUG("link ", VALUE(src), VALUE(dest), VALUE(parent), VALUE(ino), VALUE(ret)));

    ino = string_to_inode(fs, dest, 0);
    if (ino) {
        return -(ret = EEXIST);
    }
    parent = get_parent_dir_ino(fs, dest);
    if (parent == 0) {
        return -(ret = ENOTDIR);
    }
    char *filename = get_filename(dest);
    if (filename == nullptr) {
        return -(ret = EISDIR);
    }
    ino = string_to_inode(fs, src, 0);
    if (ino == 0) {
        return -(ret = ENOENT);
    }

    struct ext2_inode_large inode;
    memset(&inode, 0, sizeof(inode));
    ret = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (ret) return parse_extfs_error(fs, ino, ret);

    inode.i_links_count++;
    // update_ctime
    ret = update_xtime(fs, ino, (struct ext2_inode *)&inode, EXT_CTIME);
    if (ret) return ret;

    ret = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (ret) return parse_extfs_error(fs, ino, ret);

    ret = ext2fs_link(fs, parent, filename, ino, ext2_file_type(inode.i_mode));
    if (ret == EXT2_ET_DIR_NO_SPACE) {
        ret = ext2fs_expand_dir(fs, parent);
        if (ret) return parse_extfs_error(fs, parent, ret);

        ret = ext2fs_link(fs, parent, filename, ino, ext2_file_type(inode.i_mode));
    }
    if (ret) return parse_extfs_error(fs, parent, ret);

    // update_mtime
    ret = update_xtime(fs, parent, nullptr, EXT_CTIME | EXT_MTIME);
    if (ret) return ret;

    return 0;
}

static int do_ext2fs_symlink(ext2_filsys fs, const char *src, const char *dest) {
    ext2_ino_t parent, ino;
    errcode_t ret = 0;

    DEFER(LOG_DEBUG("symlink ", VALUE(src), VALUE(dest), VALUE(parent), VALUE(ino), VALUE(ret)));

    ino = string_to_inode(fs, dest, 0);
    if (ino) {
        return -(ret = EEXIST);
    }
    parent = get_parent_dir_ino(fs, dest);
    if (parent == 0) {
        return -(ret = ENOTDIR);
    }
    char *filename = get_filename(dest);
    if (filename == nullptr) {
        return -(ret = EISDIR);
    }

    /* Create symlink */
    ret = ext2fs_symlink(fs, parent, 0, filename, src);
    if (ret == EXT2_ET_DIR_NO_SPACE) {
        ret = ext2fs_expand_dir(fs, parent);
        if (ret) return parse_extfs_error(fs, parent, ret);

        ret = ext2fs_symlink(fs, parent, 0, filename, src);
    }
    if (ret) return parse_extfs_error(fs, parent, ret);

    /* Update parent dir's mtime */
    ret = update_xtime(fs, parent, nullptr, EXT_CTIME | EXT_MTIME);
    if (ret) return ret;

    /* Still have to update the uid/gid of the symlink */
    ino = string_to_inode(fs, dest, 0);
    if (ino == 0) {
        return -(ret = ENOTDIR);
    }

    struct ext2_inode_large inode;
    memset(&inode, 0, sizeof(inode));
    ret = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (ret) return parse_extfs_error(fs, ino, ret);

    ret = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (ret) return parse_extfs_error(fs, ino, ret);

    return 0;
}

static int do_ext2fs_mknod(ext2_filsys fs, const char *path, unsigned int st_mode, unsigned int st_rdev) {
    ext2_ino_t parent, ino;
    errcode_t ret = 0;
    unsigned long devmajor, devminor;
    int filetype;

    DEFER(LOG_DEBUG("mknod ", VALUE(path), VALUE(parent), VALUE(ino), VALUE(ret)));
    ino = string_to_inode(fs, path, 0);
    if (ino) {
        return -(ret = EEXIST);
    }

    parent = get_parent_dir_ino(fs, path);
    if (parent == 0) {
        return -(ret = ENOTDIR);
    }

    char *filename = get_filename(path);
    if (filename == nullptr) {
        return -(ret = EISDIR);
    }

    switch (st_mode & S_IFMT) {
        case S_IFCHR:
            filetype = EXT2_FT_CHRDEV;
            break;
        case S_IFBLK:
            filetype = EXT2_FT_BLKDEV;
            break;
        case S_IFIFO:
            filetype = EXT2_FT_FIFO;
            break;
#ifndef _WIN32
        case S_IFSOCK:
            filetype = EXT2_FT_SOCK;
            break;
#endif
        default:
            return EXT2_ET_INVALID_ARGUMENT;
    }

    ret = ext2fs_new_inode(fs, parent, 010755, 0, &ino);
    if (ret) return parse_extfs_error(fs, 0, ret);

    ret = ext2fs_link(fs, parent, filename, ino, filetype);
    if (ret == EXT2_ET_DIR_NO_SPACE) {
        ret = ext2fs_expand_dir(fs, parent);
        if (ret) return parse_extfs_error(fs, parent, ret);

        ret = ext2fs_link(fs, parent, filename, ino, filetype);
    }
    if (ret) return parse_extfs_error(fs, parent, ret);

    if (ext2fs_test_inode_bitmap2(fs->inode_map, ino))
        LOG_WARN("Warning: inode already set");
    ext2fs_inode_alloc_stats2(fs, ino, +1, 0);

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = st_mode;
#ifndef NO_TIMESTAMP
    inode.i_atime = inode.i_ctime = inode.i_mtime =
        fs->now ? fs->now : time(0);
#endif

    if (filetype != S_IFIFO) {
        devmajor = major(st_rdev);
        devminor = minor(st_rdev);

        if ((devmajor < 256) && (devminor < 256)) {
            inode.i_block[0] = devmajor * 256 + devminor;
            inode.i_block[1] = 0;
        } else {
            inode.i_block[0] = 0;
            inode.i_block[1] = (devminor & 0xff) | (devmajor << 8) |
                               ((devminor & ~0xff) << 12);
        }
    }
    inode.i_links_count = 1;

    ret = ext2fs_write_new_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    return 0;
}

static int do_ext2fs_stat(ext2_filsys fs, ext2_ino_t ino, struct stat *statbuf) {
    dev_t fakedev = 0;
    errcode_t ret;

    struct ext2_inode_large inode;
    memset(&inode, 0, sizeof(inode));
    ret = ext2fs_read_inode_full(fs, ino, (struct ext2_inode *)&inode, sizeof(inode));
    if (ret) return parse_extfs_error(fs, ino, ret);

    memcpy(&fakedev, fs->super->s_uuid, sizeof(fakedev));
    statbuf->st_dev = fakedev;
    statbuf->st_ino = ino;
    statbuf->st_mode = inode.i_mode;
    statbuf->st_nlink = inode.i_links_count;
    statbuf->st_uid = inode_uid(inode);
    statbuf->st_gid = inode_gid(inode);
    statbuf->st_size = EXT2_I_SIZE(&inode);
    statbuf->st_blksize = fs->blocksize;
    statbuf->st_blocks = blocks_from_inode(fs, (struct ext2_inode *)&inode);
    statbuf->st_atime = inode.i_atime;
    statbuf->st_mtime = inode.i_mtime;
    statbuf->st_ctime = inode.i_ctime;
    if (LINUX_S_ISCHR(inode.i_mode) ||
        LINUX_S_ISBLK(inode.i_mode)) {
        if (inode.i_block[0])
            statbuf->st_rdev = inode.i_block[0];
        else
            statbuf->st_rdev = inode.i_block[1];
    }

    return 0;
}

static int do_ext2fs_stat(ext2_filsys fs, const char *path, struct stat *statbuf, int follow) {
    LOG_DEBUG(VALUE(path));
    ext2_ino_t ino = string_to_inode(fs, path, follow);
    if (!ino) return -ENOENT;

    return do_ext2fs_stat(fs, ino, statbuf);
}

static int do_ext2fs_readdir(ext2_filsys fs, const char *path, std::vector<::dirent> *dirs) {
    ext2_ino_t ino = string_to_inode(fs, path, 1);
    if (!ino) return -ENOENT;

    ext2_file_t file;
    errcode_t ret = ext2fs_file_open(
        fs,
        ino,  // inode,
        0,    // flags TODO
        &file);
    if (ret) return parse_extfs_error(fs, ino, ret);
    ret = ext2fs_check_directory(fs, ino);
    if (ret) return parse_extfs_error(fs, ino, ret);
    auto block_buf = (char *)malloc(fs->blocksize);
    ret = ext2fs_dir_iterate(
        fs,
        ino,
        0,  // flags
        block_buf,
        copy_dirent_to_result,
        (void *)dirs);
    free(block_buf);
    if (ret) return parse_extfs_error(fs, ino, ret);

    return 0;
}

static int do_ext2fs_truncate(ext2_filsys fs, const char *path, off_t length) {
    ext2_ino_t ino = string_to_inode(fs, path, 1);
    if (!ino) return -ENOENT;

    ext2_file_t file;
    errcode_t ret = ext2fs_file_open(fs, ino, EXT2_FILE_WRITE, &file);
    if (ret) return parse_extfs_error(fs, ino, ret);

    ret = ext2fs_file_set_size2(file, length);
    if (ret) {
        ext2fs_file_close(file);
        return parse_extfs_error(fs, ino, ret);
    }

    ret = ext2fs_file_close(file);
    if (ret) return parse_extfs_error(fs, ino, ret);
    ret = update_xtime(fs, ino, nullptr, EXT_CTIME | EXT_MTIME);
    if (ret) return ret;

    return 0;
}

static int do_ext2fs_ftruncate(ext2_file_t file, int flags, off_t length) {
    if ((flags & (O_WRONLY | O_RDWR)) == 0) {
        return -EBADF;
    }
    errcode_t ret = 0;
    ret = ext2fs_file_set_size2(file, length);
    if (ret) return parse_extfs_error(nullptr, 0, ret);
    ret = update_xtime(file, EXT_CTIME | EXT_MTIME);
    if (ret) return ret;
    ret = ext2fs_file_flush(file);
    if (ret) return parse_extfs_error(nullptr, 0, ret);

    return 0;
}

static int do_ext2fs_access(ext2_filsys fs, const char *path, int mode) {
    ext2_ino_t ino = string_to_inode(fs, path, 1);
    if (!ino) return -ENOENT;
    // existence check
    if (mode == 0) return 0;

    struct ext2_inode inode;
    errcode_t ret = ext2fs_read_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    mode_t perms = inode.i_mode & 0777;
    if ((mode & W_OK) && (inode.i_flags & EXT2_IMMUTABLE_FL))
        return -EACCES;
    if ((mode & perms) == (mode_t)mode)
        return 0;
    return -EACCES;
}

static int do_ext2fs_statvfs(ext2_filsys fs, const char *path, struct statvfs *buf) {
    auto dblk = fs->desc_blocks + (blk64_t)fs->group_desc_count * (fs->inode_blocks_per_group + 2);
    auto rblk = ext2fs_r_blocks_count(fs->super);
    if (!rblk) rblk = ext2fs_blocks_count(fs->super) / 10;
    auto fblk = ext2fs_free_blocks_count(fs->super);

    buf->f_bsize = fs->blocksize;
    buf->f_frsize = fs->blocksize;
    buf->f_blocks = ext2fs_blocks_count(fs->super) - dblk;
    buf->f_bfree = fblk;
    buf->f_bavail = (fblk < rblk) ? 0 : (fblk - rblk);
    buf->f_files = fs->super->s_inodes_count;
    buf->f_ffree = fs->super->s_free_inodes_count;
    buf->f_favail = fs->super->s_free_inodes_count;
    auto fptr = (uint64_t *)fs->super->s_uuid;
    buf->f_fsid = *(fptr+1) ^ *fptr;
    buf->f_flag = 0;
    if (!(fs->flags & EXT2_FLAG_RW)) buf->f_flag |= ST_RDONLY;
    buf->f_namemax = EXT2_NAME_LEN;

    return 0;
}

static int do_ext2fs_statfs(ext2_filsys fs, const char *path, struct statfs *buf) {
    auto dblk = fs->desc_blocks + (blk64_t)fs->group_desc_count * (fs->inode_blocks_per_group + 2);
    auto rblk = ext2fs_r_blocks_count(fs->super);
    if (!rblk) rblk = ext2fs_blocks_count(fs->super) / 10;
    auto fblk = ext2fs_free_blocks_count(fs->super);

    buf->f_type = fs->magic;
    buf->f_bsize = fs->blocksize;
    buf->f_frsize = fs->blocksize;
    buf->f_blocks = ext2fs_blocks_count(fs->super) - dblk;
    buf->f_bfree = fblk;
    buf->f_bavail = (fblk < rblk) ? 0 : (fblk - rblk);
    buf->f_files = fs->super->s_inodes_count;
    buf->f_ffree = fs->super->s_free_inodes_count;
    auto fptr = (uint64_t *)fs->super->s_uuid;
    auto fsid = *(fptr+1) ^ *fptr;
    memcpy(&(buf->f_fsid), &fsid, sizeof(buf->f_fsid));
    buf->f_flags = 0;
    if (!(fs->flags & EXT2_FLAG_RW)) buf->f_flags |= ST_RDONLY;
    buf->f_namelen = EXT2_NAME_LEN;

    return 0;
}

static ssize_t do_ext2fs_readlink(ext2_filsys fs, const char *path, char *buf, size_t bufsize) {
    ext2_ino_t ino = string_to_inode(fs, path, 0);
    if (!ino) return -ENOENT;

    struct ext2_inode inode;
    errcode_t ret = ext2fs_read_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    if (!LINUX_S_ISLNK(inode.i_mode))
        return -EINVAL;

    size_t len = bufsize - 1;
    if (inode.i_size < len)
        len = inode.i_size;

    if (is_fast_symlink(&inode)) {
        memcpy(buf, (char *)inode.i_block, len);
    } else {
        ext2_file_t file;
        unsigned int got;
        ret = ext2fs_file_open(fs, ino, 0, &file);
        if (ret) return parse_extfs_error(fs, ino, ret);
        ret = ext2fs_file_read(file, buf, len, &got);
        if (ret) {
            ext2fs_file_close(file);
            return parse_extfs_error(fs, ino, ret);
        }
        ret = ext2fs_file_close(file);
        if (ret) return parse_extfs_error(fs, ino, ret);
        if (got != len) return -EINVAL;
    }
    buf[len] = 0;

    return (len + 1);
}

static int do_ext2fs_fallocate(ext2_filsys fs, ext2_ino_t ino, off_t offset, off_t len) {
    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));

    errcode_t ret = ext2fs_read_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    LOG_DEBUG("do_ext2fs_fallocate ", VALUE(offset), VALUE(len));
    auto start = offset / fs->blocksize;
    auto end = (offset + len - 1) / fs->blocksize;
    LOG_DEBUG("ext2fs_fallocate blocks `", end - start + 1);

    ret = ext2fs_fallocate(fs, EXT2_FALLOCATE_FORCE_INIT, ino, &inode,
                           ~0ULL, start, end - start + 1);
    if (ret) return parse_extfs_error(fs, ino, ret);

    ret = ext2fs_inode_size_set(fs, (struct ext2_inode *)&inode, len);
    if (ret) return parse_extfs_error(fs, ino, ret);

    ret = ext2fs_write_inode(fs, ino, &inode);
    if (ret) return parse_extfs_error(fs, ino, ret);

    return 0;
}

static int ino_iter_extents(ext2_filsys fs, ext2_ino_t ino,
                            ext2_extent_handle_t extents,
                            std::vector<std::pair<off_t, size_t>> &blocks) {
    errcode_t retval;
    int op = EXT2_EXTENT_ROOT;
    struct ext2fs_extent extent;

    for (;;) {
        retval = ext2fs_extent_get(extents, op, &extent);
        if (retval)
            break;

        op = EXT2_EXTENT_NEXT;

        if ((extent.e_flags & EXT2_EXTENT_FLAGS_SECOND_VISIT) ||
            !(extent.e_flags & EXT2_EXTENT_FLAGS_LEAF))
            continue;
        LOG_DEBUG("found block ` `", extent.e_pblk, extent.e_len);
        blocks.push_back({extent.e_pblk, extent.e_len});
    }

    if (retval == EXT2_ET_EXTENT_NO_NEXT)
        retval = 0;
    if (retval) {
        LOG_WARN("getting extends of ino ", VALUE(retval), VALUE(ino));
    }
    return retval;
}

// TODO make ino_iter_blocks() compatible with ext2/3
static int ino_iter_blocks(ext2_filsys fs, ext2_ino_t ino,
                           std::vector<std::pair<off_t, size_t>> &blocks) {
    struct ext2_inode inode;
    ext2_extent_handle_t extents;

    auto retval = ext2fs_read_inode(fs, ino, &inode);
    if (retval) return retval;

    if (!ext2fs_inode_has_valid_blocks2(fs, &inode)) {
        LOG_DEBUG("ext2fs_inode_has_valid_blocks2");
        return 0;
    }

    retval = ext2fs_extent_open(fs, ino, &extents);
    if (retval == EXT2_ET_INODE_NOT_EXTENT) {
        LOG_DEBUG("EXT2_ET_INODE_NOT_EXTENT");
        return 0;
    }

    auto ret = ino_iter_extents(fs, ino, extents, blocks);
    ext2fs_extent_free(extents);
    return ret;
}

static int do_ext2fs_fiemap(ext2_filsys fs, ext2_ino_t ino, struct photon::fs::fiemap* map) {
    std::vector<std::pair<off_t, size_t>> blocks;
    errcode_t ret = ino_iter_blocks(fs, ino, blocks);
    if (ret) return parse_extfs_error(fs, ino, ret);
    map->fm_mapped_extents = blocks.size();
    photon::fs::fiemap_extent *ext_buf = &map->fm_extents[0];
    for (size_t i = 0; i < blocks.size(); i++) {
        LOG_DEBUG("find block ` `", blocks[i].first * fs->blocksize, blocks[i].second * fs->blocksize);
        ext_buf[i].fe_physical = blocks[i].first * fs->blocksize;
        ext_buf[i].fe_length = blocks[i].second * fs->blocksize;
    }
    return 0;
}

static ssize_t do_ext2fs_getxattr(ext2_filsys fs, ext2_ino_t ino, const char *name, void *value,
                                  size_t size) {
    struct ext2_xattr_handle *h;
    void *ptr;
    size_t plen;
    errcode_t ret = ext2fs_xattrs_open(fs, ino, &h);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    DEFER(ext2fs_xattrs_close(&h));

    ret = ext2fs_xattrs_read(h);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    // get xattr
    ret = ext2fs_xattr_get(h, name, &ptr, &plen);
    if (ret)
        return parse_extfs_error(fs, ino, ret);

    if (size == 0) {
        ret = plen;
    } else if (size < plen) {
        ret = -ERANGE;
    } else {
        memcpy(value, ptr, plen);
        ret = plen;
    }

    ext2fs_free_mem(&ptr);
    return ret;
}

static ssize_t do_ext2fs_getxattr(ext2_filsys fs, const char *path, const char *name,
                                  void *value, size_t size, int follow) {
    LOG_DEBUG(VALUE(path));
    ext2_ino_t ino = string_to_inode(fs, path, follow);
    if (!ino) return -ENOENT;

    return do_ext2fs_getxattr(fs, ino, name, value, size);
}

static int count_buffer_space(char *name, char *value EXT2FS_ATTR((unused)),
                              size_t value_len EXT2FS_ATTR((unused)), void *data) {
    auto cnt = (unsigned int *)data;
    *cnt = *cnt + strlen(name) + 1;
    return 0;
}

static int copy_names(char *name, char *value EXT2FS_ATTR((unused)),
                      size_t value_len EXT2FS_ATTR((unused)), void *data) {
    auto buf = (char **)data;
    size_t name_len = strlen(name);
    memcpy(*buf, name, name_len + 1);
    *buf = *buf + name_len + 1;
    return 0;
}

static ssize_t do_ext2fs_listxattr(ext2_filsys fs, ext2_ino_t ino, char *list, size_t size) {
    struct ext2_xattr_handle *h;
    errcode_t ret = ext2fs_xattrs_open(fs, ino, &h);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    DEFER(ext2fs_xattrs_close(&h));

    ret = ext2fs_xattrs_read(h);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    // count buffer space
    unsigned int buf_size = 0;
    ret = ext2fs_xattrs_iterate(h, count_buffer_space, &buf_size);
    if (ret)
        return parse_extfs_error(fs, ino, ret);

    if (size == 0) {
        return buf_size;
    } else if (size < buf_size) {
        return -ERANGE;
    }
    // copy to list
    memset(list, 0, size);
    ret = ext2fs_xattrs_iterate(h, copy_names, &list);
    if (ret)
        return parse_extfs_error(fs, ino, ret);

    return buf_size;
}

static ssize_t do_ext2fs_listxattr(ext2_filsys fs, const char *path, char *list, size_t size, int follow) {
    LOG_DEBUG(VALUE(path));
    ext2_ino_t ino = string_to_inode(fs, path, follow);
    if (!ino) return -ENOENT;

    return do_ext2fs_listxattr(fs, ino, list, size);
}

static bool check_namespace(const char *name) {
    static std::vector<std::string> valid_names = {
        "gnu.",
        "system.posix_acl_default",
        "system.posix_acl_access",
        "system.richacl",
        "security.",
        "trusted.",
        "system.",
        "user.",
    };

    for (auto &n : valid_names) {
        if (strncmp(name, n.c_str(), n.size()) == 0) {
            return true;
        }
    }
    return false;
}

static int do_ext2fs_setxattr(ext2_filsys fs, ext2_ino_t ino, const char *name,
                              const void *value, size_t size, int flags) {
    if (!check_namespace(name))
        LOG_ERROR_RETURN(ENOTSUP, -ENOTSUP, "the namespace prefix of '`' is not valid", name);

    struct ext2_xattr_handle *h;
    errcode_t ret = ext2fs_xattrs_open(fs, ino, &h);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    DEFER(ext2fs_xattrs_close(&h));

    ret = ext2fs_xattrs_read(h);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    // set xattr
    ret = ext2fs_xattr_set(h, name, value, size);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    // update_ctime
    ret = update_xtime(fs, ino, nullptr, EXT_CTIME);
    if (ret) return ret;

    return 0;
}

static int do_ext2fs_setxattr(ext2_filsys fs, const char *path, const char *name,
                              const void *value, size_t size, int flags, int follow) {
    LOG_DEBUG(VALUE(path));
    ext2_ino_t ino = string_to_inode(fs, path, follow);
    if (!ino) return -ENOENT;

    return do_ext2fs_setxattr(fs, ino, name, value, size, flags);
}

static int do_ext2fs_removexattr(ext2_filsys fs, ext2_ino_t ino, const char *name) {
    struct ext2_xattr_handle *h;
    errcode_t ret = ext2fs_xattrs_open(fs, ino, &h);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    DEFER(ext2fs_xattrs_close(&h));

    ret = ext2fs_xattrs_read(h);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    // remove xattr
    // no key found is treat as success
    ret = ext2fs_xattr_remove(h, name);
    if (ret)
        return parse_extfs_error(fs, ino, ret);
    // update_ctime
    ret = update_xtime(fs, ino, nullptr, EXT_CTIME);
    if (ret) return ret;

    return 0;
}

static int do_ext2fs_removexattr(ext2_filsys fs, const char *path, const char *name, int follow) {
    LOG_DEBUG(VALUE(path));
    ext2_ino_t ino = string_to_inode(fs, path, follow);
    if (!ino) return -ENOENT;

    return do_ext2fs_removexattr(fs, ino, name);
}

#define DO_EXT2FS(func) \
    auto ret = func;    \
    if (ret < 0) {      \
        errno = -ret;   \
        return -1;      \
    }                   \
    return ret;

namespace photon {
namespace fs {

extern io_manager new_io_manager(photon::fs::IFile *file);

class ExtFileSystem;
class ExtFile : public photon::fs::VirtualFile, public photon::fs::IFileXAttr {
public:
    ExtFile(ext2_file_t _file, int _flags, ExtFileSystem *_fs) :
        file(_file), flags(_flags), m_fs(_fs) {
        fs = ext2fs_file_get_fs(file);
        ino = ext2fs_file_get_inode_num(file);
    }

    ~ExtFile() {
        close();
    }

    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        DO_EXT2FS(do_ext2fs_read(file, flags, (char *)buf, count, offset))
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        DO_EXT2FS(do_ext2fs_write(file, flags, (const char *)buf, count, offset))
    }
    virtual int fchmod(mode_t mode) override {
        DO_EXT2FS(do_ext2fs_chmod(fs, ino, mode))
    }
    virtual int fchown(uid_t owner, gid_t group) override {
        DO_EXT2FS(do_ext2fs_chown(fs, ino, owner, group))
    }
    virtual int futimes(const struct timeval tv[2]) {
        DO_EXT2FS(do_ext2fs_utimes(fs, ino, tv))
    }
    virtual int fstat(struct stat *buf) override {
        DO_EXT2FS(do_ext2fs_stat(fs, ino, buf))
    }
    virtual int close() override {
        DO_EXT2FS(do_ext2fs_file_close(file))
    }
    virtual int fsync() override {
        if ((flags & (O_WRONLY | O_RDWR)) == 0) {
            errno = EBADF;
            return -1;
        }
        errcode_t ret = ext2fs_file_flush(file);
        if (ret) {
            errno = -parse_extfs_error(fs, ino, ret);
            return -1;
        }
        return flush_buffer();
    }
    virtual int fdatasync() override {
        return this->fsync();
    }
    virtual int ftruncate(off_t length) override {
        DO_EXT2FS(do_ext2fs_ftruncate(file, flags, length))
    }
    // mode is not used
    virtual int fallocate(int mode, off_t o_offset, off_t len) override {
        DO_EXT2FS(do_ext2fs_fallocate(fs, ino, o_offset, len));
    }
    virtual int fiemap(struct photon::fs::fiemap* map) override {
        DO_EXT2FS(do_ext2fs_fiemap(fs, ino, map));
    }
    virtual ssize_t fgetxattr(const char *name, void *value, size_t size) override {
        DO_EXT2FS(do_ext2fs_getxattr(fs, ino, name, value, size));
    }
    virtual ssize_t flistxattr(char *list, size_t size) override {
        DO_EXT2FS(do_ext2fs_listxattr(fs, ino, list, size));
    }
    virtual int fsetxattr(const char *name, const void *value, size_t size, int flags) override {
        DO_EXT2FS(do_ext2fs_setxattr(fs, ino, name, value, size, flags));
    }
    virtual int fremovexattr(const char *name) override {
        DO_EXT2FS(do_ext2fs_removexattr(fs, ino, name));
    }

    virtual photon::fs::IFileSystem *filesystem() override;

    int flush_buffer();

private:
    ext2_file_t file;
    ext2_filsys fs;
    ext2_ino_t ino;
    int flags;
    ExtFileSystem *m_fs;
};

class ExtDIR : public photon::fs::DIR {
public:
    std::vector<::dirent> m_dirs;
    ::dirent *direntp = nullptr;
    long loc;
    ExtDIR(std::vector<::dirent> &dirs) : loc(0) {
        m_dirs = std::move(dirs);
        next();
    }
    virtual ~ExtDIR() override {
        closedir();
    }
    virtual int closedir() override {
        if (!m_dirs.empty()) {
            m_dirs.clear();
        }
        return 0;
    }
    virtual dirent *get() override {
        return direntp;
    }
    virtual int next() override {
        if (!m_dirs.empty()) {
            if (loc < (long) m_dirs.size()) {
                direntp = &m_dirs[loc++];
            } else {
                direntp = nullptr;
            }
        }
        return direntp != nullptr ? 1 : 0;
    }
    virtual void rewinddir() override {
        loc = 0;
        next();
    }
    virtual void seekdir(long loc) override {
        this->loc = loc;
        next();
    }
    virtual long telldir() override {
        return loc;
    }
};

static const uint64_t kMinimalInoLife = 1L * 1000 * 1000; // ino lives at least 1s
class ExtFileSystem : public photon::fs::IFileSystem, public photon::fs::IFileSystemXAttr {
public:
    ext2_filsys fs;
    io_manager extfs_io_manager;
    ExtFileSystem(photon::fs::IFile *_image_file, bool buffer = true) : ino_cache(kMinimalInoLife) {
        ExtFileSystem::mutex.lock();
        DEFER(ExtFileSystem::mutex.unlock());
        if (buffer) {
            buffer_file = new_buffer_file(_image_file);
            extfs_io_manager = new_io_manager(buffer_file);
        } else {
            extfs_io_manager = new_io_manager(_image_file);
        }
        fs = do_ext2fs_open(extfs_io_manager);
        memset(fs->reserved, 0, sizeof(fs->reserved));
        auto reserved = reinterpret_cast<std::uintptr_t *>(fs->reserved);
        reserved[0] = reinterpret_cast<std::uintptr_t>(this);
    }
    ~ExtFileSystem() {
        if (fs) {
            ext2fs_flush(fs);
            ext2fs_close(fs);
            LOG_INFO("ext2fs flushed and closed");
        }
        delete buffer_file;
        LOG_INFO(VALUE(total_read_cnt), VALUE(total_write_cnt));
    }
    photon::fs::IFile *open(const char *path, int flags, mode_t mode) override {
        ext2_file_t file = do_ext2fs_open_file(fs, path, flags, mode);
        if (!file) {
            return nullptr;
        }
        return new ExtFile(file, flags, this);
    }
    photon::fs::IFile *open(const char *path, int flags) override {
        return open(path, flags, 0666);
    }
    photon::fs::IFile *creat(const char *path, mode_t mode) override {
        return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    }
    int mkdir(const char *path, mode_t mode) override {
        DO_EXT2FS(do_ext2fs_mkdir(fs, path, mode))
    }
    int rmdir(const char *path) override {
        DO_EXT2FS(do_ext2fs_rmdir(fs, path))
    }
    int symlink(const char *oldname, const char *newname) override {
        DO_EXT2FS(do_ext2fs_symlink(fs, oldname, newname))
    }
    int link(const char *oldname, const char *newname) override {
        DO_EXT2FS(do_ext2fs_link(fs, oldname, newname))
    }
    int rename(const char *oldname, const char *newname) override {
        DO_EXT2FS(do_ext2fs_rename(fs, oldname, newname))
    }
    int unlink(const char *path) override {
        DO_EXT2FS(do_ext2fs_unlink(fs, path))
    }
    int mknod(const char *path, mode_t mode, dev_t dev) override {
        DO_EXT2FS(do_ext2fs_mknod(fs, path, mode, dev))
    }
    int utime(const char *path, const struct utimbuf *file_times) override {
        struct timeval tv[2];
        tv[0].tv_sec = file_times->actime;
        tv[0].tv_usec = 0;
        tv[1].tv_sec = file_times->modtime;
        tv[1].tv_usec = 0;
        DO_EXT2FS(do_ext2fs_utimes(fs, path, tv, 1));
    }
    int utimes(const char *path, const struct timeval tv[2]) override {
        DO_EXT2FS(do_ext2fs_utimes(fs, path, tv, 1));
    }
    int lutimes(const char *path, const struct timeval tv[2]) override {
        DO_EXT2FS(do_ext2fs_utimes(fs, path, tv, 0));
    }
    int chown(const char *path, uid_t owner, gid_t group) override {
        DO_EXT2FS(do_ext2fs_chown(fs, path, owner, group, 1))
    }
    int lchown(const char *path, uid_t owner, gid_t group) override {
        DO_EXT2FS(do_ext2fs_chown(fs, path, owner, group, 0))
    }
    int chmod(const char *path, mode_t mode) override {
        DO_EXT2FS(do_ext2fs_chmod(fs, path, mode))
    }
    int stat(const char *path, struct stat *buf) override {
        DO_EXT2FS(do_ext2fs_stat(fs, path, buf, 1))
    }
    int lstat(const char *path, struct stat *buf) override {
        DO_EXT2FS(do_ext2fs_stat(fs, path, buf, 0))
    }
    ssize_t readlink(const char *path, char *buf, size_t bufsize) override {
        DO_EXT2FS(do_ext2fs_readlink(fs, path, buf, bufsize))
    }
    int statfs(const char *path, struct statfs *buf) override {
        DO_EXT2FS(do_ext2fs_statfs(fs, path, buf))
    }
    int statvfs(const char *path, struct statvfs *buf) override {
        DO_EXT2FS(do_ext2fs_statvfs(fs, path, buf))
    }
    int access(const char *path, int mode) override {
        DO_EXT2FS(do_ext2fs_access(fs, path, mode))
    }
    int truncate(const char *path, off_t length) override {
        DO_EXT2FS(do_ext2fs_truncate(fs, path, length))
    }
    int syncfs() override {
        errcode_t ret = ext2fs_flush(fs);
        if (ret) {
            errno = -parse_extfs_error(fs, 0, ret);
            return -1;
        }
        return flush_buffer();
    }
    ssize_t getxattr(const char *path, const char *name, void *value, size_t size) override {
        DO_EXT2FS(do_ext2fs_getxattr(fs, path, name, value, size, 1));
    }
    ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) override {
        DO_EXT2FS(do_ext2fs_getxattr(fs, path, name, value, size, 0));
    }
    ssize_t listxattr(const char *path, char *list, size_t size) override {
        DO_EXT2FS(do_ext2fs_listxattr(fs, path, list, size, 1));
    }
    ssize_t llistxattr(const char *path, char *list, size_t size) override {
        DO_EXT2FS(do_ext2fs_listxattr(fs, path, list, size, 0));
    }
    int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) override {
        DO_EXT2FS(do_ext2fs_setxattr(fs, path, name, value, size, flags, 1));
    }
    int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) override {
        DO_EXT2FS(do_ext2fs_setxattr(fs, path, name, value, size, flags, 0));
    }
    int removexattr(const char *path, const char *name) override {
        DO_EXT2FS(do_ext2fs_removexattr(fs, path, name, 1));
    }
    int lremovexattr(const char *path, const char *name) override {
        DO_EXT2FS(do_ext2fs_removexattr(fs, path, name, 0));
    }

    photon::fs::DIR *opendir(const char *path) override {
        std::vector<::dirent> dirs;
        auto ret = do_ext2fs_readdir(fs, path, &dirs);
        if (ret) {
            errno = -ret;
            return nullptr;
        }
        return new ExtDIR(dirs);
    }

    IFileSystem *filesystem() {
        return this;
    }

    ext2_ino_t get_inode(const char *str, int follow, bool release) {
        ext2_ino_t ino = 0;
        DEFER(LOG_DEBUG("get_inode ", VALUE(str), VALUE(follow), VALUE(release), VALUE(ino)));

        auto func = [&]() -> ext2_ino_t * {
            ext2_ino_t *i = new ext2_ino_t;
            errcode_t ret = 0;
            if (follow) {
                ret = ext2fs_namei_follow(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, str, i);
            } else {
                auto parent = get_parent_dir_ino(fs, str);
                if (parent) {
                    auto filename = get_filename(str);
                    if (filename)
                        ret = ext2fs_namei(fs, EXT2_ROOT_INO, parent, filename, i);
                    else
                        ret = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, str, i);
                } else {
                    ret = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, str, i);
                }
            }
            if (ret) {
                LOG_DEBUG("ext2fs_namei not found ", VALUE(str), VALUE(follow));
                errno = -parse_extfs_error(fs, 0, ret);
                delete i;
                return nullptr;
            }
            LOG_DEBUG("ext2fs_namei ", VALUE(str), VALUE(follow), VALUE(*i));
            return i;
        };

        if (follow) {
            auto b = func();
            if (b) ino = *b;

        } else {
            auto b = ino_cache.borrow(str, func);
            if (b) ino = *b;
        }

        if (release) {
            LOG_DEBUG("release ino_cache ", VALUE(str), VALUE(release));
            auto b = ino_cache.borrow(str);
            b.recycle(true);
        }

        return ino;
    }
    int flush_buffer() {
        return buffer_file ? buffer_file->fdatasync() : 0;
    }

private:
    ObjectCache<estring, ext2_ino_t *> ino_cache;
    photon::fs::IFile *buffer_file = nullptr;
    static photon::mutex mutex; // lock from init io_manager to ext2fs_open
};
// Initialize the static member.
photon::mutex ExtFileSystem::mutex;

photon::fs::IFileSystem *ExtFile::filesystem() {
    return m_fs;
}
int ExtFile::flush_buffer() {
    return m_fs->flush_buffer();
}

photon::fs::IFileSystem *new_extfs(photon::fs::IFile *file, bool buffer) {
    auto extfs = new ExtFileSystem(file, buffer);
    return extfs->fs ? extfs : nullptr;
}

}
}

static ext2_ino_t string_to_inode(ext2_filsys fs, const char *str, int follow, bool release) {
    auto reserved = reinterpret_cast<std::uintptr_t *>(fs->reserved);
    auto extfs = reinterpret_cast<photon::fs::ExtFileSystem *>(reserved[0]);
    // LOG_DEBUG("string_to_inode ", VALUE(str), VALUE(follow), VALUE(release));
    return extfs->get_inode(str, follow, release);
}
