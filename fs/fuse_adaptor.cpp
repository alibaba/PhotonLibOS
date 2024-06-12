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

#include <thread>
#include <photon/common/alog.h>
#include "exportfs.h"
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include "fuse_adaptor.h"

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 29
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_LIBULOCKMGR
#include <ulockmgr.h>
#endif

#ifdef HAVE_SETXATTR
#undef HAVE_SETXATTR
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif
#include <sys/file.h> /* flock(2) */

#include "filesystem.h"
#include <photon/common/utility.h>
#include <photon/io/fuse-adaptor.h>
#include <photon/common/perf_counter.h>

REGISTER_PERF(xmp_getattr, TOTAL)
REGISTER_PERF(xmp_open, TOTAL)
REGISTER_PERF(xmp_read, TOTAL)
REGISTER_PERF(xmp_write, TOTAL)
REGISTER_PERF(xmp_statfs, TOTAL)
REGISTER_PERF(xmp_flush, TOTAL)
REGISTER_PERF(xmp_release, TOTAL)
REGISTER_PERF(xmp_fsync, TOTAL)
REGISTER_PERF(xmp_opendir, TOTAL)
REGISTER_PERF(xmp_readdir, TOTAL)
REGISTER_PERF(xmp_releasedir, TOTAL)
REGISTER_PERF(xmp_init, TOTAL)
REGISTER_PERF(xmp_destroy, TOTAL)
REGISTER_PERF(xmp_access, TOTAL)
REGISTER_PERF(xmp_create, TOTAL)

namespace photon {
namespace fs{

static IFileSystem* fs = nullptr;
static uid_t cuid = 0, cgid = 0;

#define CHECK_FS() if (!fs) return -EFAULT;

static void* xmp_init(struct fuse_conn_info *conn)
{
    REPORT_PERF(xmp_init, 1)
    cuid  = geteuid();
    cgid  = getegid();
    if ((unsigned int)conn->capable & FUSE_CAP_ATOMIC_O_TRUNC) {
        conn->want |= FUSE_CAP_ATOMIC_O_TRUNC;
    }
    if ((unsigned int)conn->capable & FUSE_CAP_BIG_WRITES) {
        conn->want |= FUSE_CAP_BIG_WRITES;
    }
    return NULL;
}

static void xmp_destroy(void *handle)
{
    REPORT_PERF(xmp_destroy, 1)
}

static int xmp_getattr(const char *path, struct stat *stbuf)
{
    REPORT_PERF(xmp_getattr, 1)
    LOG_DEBUG(VALUE(path));
    CHECK_FS();
    errno = 0;
    int res = fs->lstat(path, stbuf);
    if(res) return -errno;
    stbuf->st_blocks = stbuf->st_size / 512 + 1;  // du relies on this
    stbuf->st_blksize = 4096;
    // use current user/group when backendfs doesn't support uid/gid
    if (stbuf->st_uid == 0) stbuf->st_uid = cuid;
    if (stbuf->st_gid == 0) stbuf->st_gid = cgid;
    if (stbuf->st_nlink == 0) stbuf->st_nlink = 1;
    return 0;
}

static int xmp_access(const char *path, int mask)
{
    REPORT_PERF(xmp_access, 1)
    LOG_DEBUG(VALUE(path), VALUE(mask));
    CHECK_FS();
    int res = fs->access(path, mask);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(mask));
    return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
    LOG_DEBUG(VALUE(path), VALUE(size));
    CHECK_FS();
    int res = fs->readlink(path, buf, size - 1);
    if(res < 0) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(size));
    buf[res] = '\0';
    return 0;
}

//struct xmp_dirp {
//    DIR *dp;
//    struct dirent *entry;
//    off_t offset;
//};

static int xmp_opendir(const char *path, struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_opendir, 1)
    LOG_DEBUG(VALUE(path));
    CHECK_FS();
    auto dirp = fs->opendir(path);
    fi->fh = (unsigned long) dirp;
    if(!dirp) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path));
    return 0;
}

static inline fs::DIR* get_dirp(struct fuse_file_info *fi)
{
    auto dirp = (fs::DIR*)fi->fh;
    LOG_DEBUG(VALUE(dirp));
    return dirp;
}

static inline fs::IFile* get_file(struct fuse_file_info *fi)
{
    auto file = (fs::IFile*)fi->fh;
    LOG_DEBUG(VALUE(file));
    return file;
}

static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_readdir, 1)
    LOG_DEBUG(VALUE(path), VALUE(offset));
    CHECK_FS();
    auto d = get_dirp(fi);
    if (!d) \
        LOG_ERROR_RETURN(0, -1, VALUE(path));
    for (auto& x: FileList(d))
    {
        struct stat st;
        memset(&st, 0, sizeof(st));
        auto& dirp = x;
        st.st_ino = dirp.d_ino;
        st.st_mode = dirp.d_type << 12;
        LOG_DEBUG("dirp.d_name: `", dirp.d_name);
        if (filler(buf, dirp.d_name, &st, 0))
        {
            LOG_DEBUG("xmp_readdir break");
            break;
        }
    }
    return 0;
}

static int xmp_releasedir(const char *path, struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_releasedir, 1)
    LOG_DEBUG(VALUE(path));
    CHECK_FS();
    auto d = get_dirp(fi);
    d->closedir();
    delete d;
    return 0;
}

static int xmp_fsyncdir(const char *path, int flag, struct fuse_file_info *fi)
{
    CHECK_FS();
    return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
    LOG_DEBUG(VALUE(path));
    CHECK_FS();
    int res = fs->mkdir(path, mode);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(mode));
    return 0;
}

static int xmp_unlink(const char *path)
{
    LOG_DEBUG(VALUE(path));
    CHECK_FS();
    int res = fs->unlink(path);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path));
    return 0;
}

static int xmp_rmdir(const char *path)
{
    LOG_DEBUG(VALUE(path));
    CHECK_FS();
    int res = fs->rmdir(path);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path));
    return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
    LOG_DEBUG(VALUE(from), VALUE(to));
    CHECK_FS();
    int res = fs->symlink(from, to);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(from), VALUE(to));
    return 0;
}

static int xmp_rename(const char *from, const char *to)
{
    LOG_DEBUG(VALUE(from), VALUE(to));
    CHECK_FS();
    int res = fs->rename(from, to);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(from), VALUE(to));
    return 0;
}

static int xmp_link(const char *from, const char *to)
{
    LOG_DEBUG(VALUE(from), VALUE(to));
    CHECK_FS();
    int res = fs->link(from, to);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(from), VALUE(to));
    return 0;
}

static int xmp_chmod(const char *path, mode_t mode)
{
    LOG_DEBUG(VALUE(path), VALUE(mode));
    CHECK_FS();
    int res = fs->chmod(path, mode);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(mode));
    return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid)
{
    LOG_DEBUG(VALUE(path), VALUE(uid), VALUE(gid));
    CHECK_FS();
    int res = fs->chown(path, uid, gid);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(uid), VALUE(gid));
    return 0;
}

static int xmp_truncate(const char *path, off_t size)
{
    LOG_DEBUG(VALUE(path), VALUE(size));
    CHECK_FS();
    int res = fs->truncate(path, size);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(size));
    return 0;
}

#if 0
static int xmp_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    CHECK_FS();
    int res = geti_file(fi)->ftruncate(size);
    return (res == 0) ? 0 : -errno;
}
#endif

static int xmp_utimens(const char *path, const struct timespec ts[2])
{
    CHECK_FS();
//    int res;

    /* don't use utime/utimes since they follow symlinks */
/*
    if (fi)
        res = get_file(fi)->futimens(ts);
    else
    {
        auto fh = fs->open(path, O_WRONLY);
        res = fh->futimens(ts);
        fh->close();
        delete fh;
    }
    return (res == 0) ? 0 : -errno;
*/
    return -ENOSYS;
}

static int xmp_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_create, 1)
    LOG_DEBUG(VALUE(path), VALUE(fi->flags), VALUE(mode));
    CHECK_FS();
    auto fh = fs->open(path, fi->flags, mode);
    fi->fh = (uint64_t)fh;
    if(!fh) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(fi->flags), VALUE(mode));
    return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_open, 1)
    LOG_DEBUG(VALUE(path), VALUE(fi->flags));
    CHECK_FS();
    auto fh = fs->open(path, fi->flags);
    fi->fh = (uint64_t)fh;
    if(!fh) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(fi->flags));
    return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_read, 1)
    LOG_DEBUG(VALUE(path), VALUE(offset), VALUE(size));
    CHECK_FS();
    int res = get_file(fi)->pread(buf, size, offset);
    if(res < 0) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(size), VALUE(offset));
    return res;
}

#if 0
static int xmp_read_buf(const char *path, struct fuse_bufvec **bufp,
                        size_t size, off_t offset, struct fuse_file_info *fi)
{
    DLOG(DEBUG) << "xmp_read_buf ";

    struct fuse_bufvec *src;
    (void) path;

    src = (struct fuse_bufvec*)malloc(sizeof(struct fuse_bufvec));
    if (src == NULL)
        return -ENOMEM;

    *src = FUSE_BUFVEC_INIT(size);

    src->buf[0].flags = (enum fuse_buf_flags)(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
    src->buf[0].fd = fi->fh;
    src->buf[0].pos = offset;

    *bufp = src;

    return 0;
}
#endif

static int xmp_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_write, 1)
    LOG_DEBUG(VALUE(path), VALUE(offset), VALUE(size));
    CHECK_FS();
    int res = get_file(fi)->pwrite(buf, size, offset);
    if(res < 0) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(size), VALUE(offset));
    return res;
}
/*
static int xmp_write_buf(const char *path, struct fuse_bufvec *buf,
                         off_t offset, struct fuse_file_info *fi)
{
#if 0
    struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));

    (void) path;

    dst.buf[0].flags = (enum fuse_buf_flags)(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
    dst.buf[0].fd = fi->fh;
    dst.buf[0].pos = offset;

    return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
#else
    return 0;
#endif
}
*/

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
    REPORT_PERF(xmp_statfs, 1)
    LOG_DEBUG(VALUE(path));
    CHECK_FS();
    int res = fs->statvfs(path, stbuf);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path));
    return 0;
}

static int xmp_flush(const char *path, struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_flush, 1)
    LOG_DEBUG(VALUE(path));
    CHECK_FS();

    /* quote from FUSE doc:
     "BIG NOTE: This is not equivalent to fsync(). It's not a request to sync dirty data.

     Flush is called on each close() of a file descriptor. So if a filesystem wants to
     return write errors in close() and the file has cached dirty data, this is a good
     place to write back data and return any errors. Since many applications ignore
     close() errors this is not always useful."

     But it looks like a fsync() ...
     */

    int res = get_file(fi)->fsync();
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path));
    return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_release, 1)
    LOG_DEBUG(VALUE(path));
    CHECK_FS();
    auto file = get_file(fi);
    int res = file->close();
    DEFER(delete file);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path));
    return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
                     struct fuse_file_info *fi)
{
    REPORT_PERF(xmp_fsync, 1)
    LOG_DEBUG(VALUE(path), VALUE(isdatasync));
    CHECK_FS();
    auto fh = get_file(fi);
    int res = isdatasync ? fh->fdatasync() : fh->fsync();
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(isdatasync));
    return 0;
}

static int xmp_fallocate(const char *path, int mode,
                         off_t offset, off_t length, struct fuse_file_info *fi)
{
    LOG_DEBUG(VALUE(path), VALUE(mode));
    auto fh = get_file(fi);
    int res = fh->fallocate(mode, offset, length);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(mode));
    return 0;
}


static int xmp_setxattr(const char *path, const char *name, const char *value,
                        size_t size, int flags)
{
    LOG_DEBUG(VALUE(path));
    return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
                        size_t size)
{
    LOG_DEBUG(VALUE(path));
    return 0;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
    LOG_DEBUG(VALUE(path));
    return 0;
}

static int xmp_removexattr(const char *path, const char *name)
{
    LOG_DEBUG(VALUE(path));
    return 0;
}

static int xmp_flock(const char *path, struct fuse_file_info *fi, int op)
{
    LOG_DEBUG(VALUE(path));
/*
    res = flock(fi->fh, op);
    if (res == -1)
        return -errno;

    return 0;
*/
    return -ENOSYS;
}

static int xmp_getdir(const char* path, fuse_dirh_t dir, fuse_dirfil_t dirf)
{
    LOG_DEBUG(VALUE(path));
    return -ENOSYS;
}

static int xmp_mknod(const char* path,  mode_t mode, dev_t dev)
{
    LOG_DEBUG(VALUE(path));
    return -ENOSYS;
}

static int xmp_utime(const char* path, struct utimbuf* tb)
{
    LOG_DEBUG(VALUE(path));
    return -ENOSYS;
}

#if 0
static int xmp_fgetattr(const char *path, struct stat *buf, struct fuse_file_info *fi)
{
    LOG_DEBUG(VALUE(path));
    CHECK_FS();

    int res = fs->stat(path, buf);
    return (res == 0) ? 0 : -errno;
}
#endif

static int xmp_lock(const char *path, struct fuse_file_info *fi, int cmd, struct flock *lock)
{
    LOG_DEBUG(VALUE(path));
    return -ENOSYS;
}

static int xmp_bmap(const char *path, size_t blocksize, uint64_t *idx)
{
    LOG_DEBUG(VALUE(path));
    return -ENOSYS;
}

static int xmp_ioctl(const char *path, int cmd, void *arg,
                      struct fuse_file_info *fi, unsigned int flags, void *data)
{
    LOG_DEBUG(VALUE(path));
    return -ENOSYS;
}

static int xmp_poll(const char *path, struct fuse_file_info *fi,
                     struct fuse_pollhandle *ph, unsigned *reventsp)
{
    LOG_DEBUG(VALUE(path));
//   __unused(path, fi, ph, reventsp);
    return -ENOSYS;
}

static struct fuse_operations xmp_oper = {
    .getattr     = xmp_getattr,
    .readlink    = xmp_readlink,
    .getdir      = xmp_getdir,
    .mknod       = xmp_mknod,
    .mkdir       = xmp_mkdir,
    .unlink      = xmp_unlink,
    .rmdir       = xmp_rmdir,
    .symlink     = xmp_symlink,
    .rename      = xmp_rename,
    .link        = xmp_link,
    .chmod       = xmp_chmod,
    .chown       = xmp_chown,
    .truncate    = xmp_truncate,
    .utime       = xmp_utime,
    .open        = xmp_open,
    .read        = xmp_read,
    .write       = xmp_write,
    .statfs      = xmp_statfs,
    .flush       = xmp_flush,
    .release     = xmp_release,
    .fsync       = xmp_fsync,
    .setxattr    = xmp_setxattr,
    .getxattr    = xmp_getxattr,
    .listxattr   = xmp_listxattr,
    .removexattr = xmp_removexattr,
    .opendir     = xmp_opendir,
    .readdir     = xmp_readdir,
    .releasedir  = xmp_releasedir,
    .fsyncdir    = xmp_fsyncdir,
    .init        = xmp_init,
    .destroy     = xmp_destroy,
    .access      = xmp_access,
    .create      = xmp_create,
    .ftruncate   = nullptr,  /* The fgetattr and ftruncate handlers have become obsolete and have been removed */
    .fgetattr    = nullptr,  /* The fgetattr and ftruncate handlers have become obsolete and have been removed */
    .lock        = xmp_lock,
    .utimens     = xmp_utimens,
    .bmap        = xmp_bmap,
    flag_nullpath_ok:1,
    flag_nopath:1,
    flag_utime_omit_ok:1,
    flag_reserved:29,
    .ioctl       = xmp_ioctl,
    .poll        = xmp_poll,
    .write_buf   = nullptr, //xmp_write_buf,
    .read_buf    = nullptr, //xmp_read_buf,
    .flock       = xmp_flock,
    .fallocate   = xmp_fallocate
};
/*
static void fuse_logger(enum fuse_log_level level, const char *fmt, va_list ap)
{
    const int LEN = 4096;
    char buf[LEN + 1];
    int ret = vsnprintf(buf, LEN, fmt, ap);
    if (ret < 0) return;
    if (ret > LEN) ret = LEN;

    buf[LEN] = '\n';
    log_output(buf, buf + LEN + 1);
}
*/
int fuser_go(IFileSystem* fs_, int argc, char* argv[])
{
    if (!fs_)
        return 0;

    // fuse_set_log_func(&fuse_logger);

    umask(0);
    fs = fs_;
    return photon::run_fuse(argc, argv, &xmp_oper, NULL);
}

int fuser_go_exportfs(IFileSystem *fs_, int argc, char *argv[]) {
    if (!fs_) return 0;
    if (photon::init(INIT_EVENT_DEFAULT, INIT_IO_DEFAULT | INIT_IO_EXPORTFS))
        return -1;
    DEFER(photon::fini());

    auto efs = export_as_sync_fs(fs_);

    // fuse_set_log_func(&fuse_logger);

    umask(0);
    fs = efs;
    photon::semaphore exit_sem(0);
    std::thread([&] {
        fuse_main(argc, argv, &xmp_oper, NULL);
        exit_sem.signal(1);
    }).detach();
    exit_sem.wait(1);
    return 0;
}

void set_fuse_fs(fs::IFileSystem *fs_) { fs = fs_; }

fuse_operations *get_fuse_xmp_oper() { return &xmp_oper; }

}
}
