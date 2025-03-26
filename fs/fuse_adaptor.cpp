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
#include "fuse_adaptor.h"

#if FUSE_USE_VERSION >= 30
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse_lowlevel.h>
#endif
#include <thread>
#include <vector>

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

#include <photon/common/alog.h>
#include <photon/common/event-loop.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/fs/exportfs.h>
#include <photon/fs/filesystem.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread-pool.h>
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

#define CHECK_FS() if (!fs) return -EFAULT;

#if FUSE_USE_VERSION >= 30
static void* xmp_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    REPORT_PERF(xmp_init, 1)
    conn->max_readahead = 128*1024*1024;
    conn->max_background = 128;
    conn->want &= ~FUSE_CAP_AUTO_INVAL_DATA;  // avoid per-read getattr in kernel 4.19
    (void) conn;
    (void) cfg;
    return NULL;
}
#else
static void* xmp_init(struct fuse_conn_info *conn)
{
    REPORT_PERF(xmp_init, 1)
    (void) conn;
    return NULL;
}
#endif

static void xmp_destroy(void *handle)
{
    REPORT_PERF(xmp_destroy, 1)
}

#if FUSE_USE_VERSION >= 30
static int xmp_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
#else
static int xmp_getattr(const char *path, struct stat *stbuf)
#endif
{
    REPORT_PERF(xmp_getattr, 1)
    LOG_DEBUG(VALUE(path));
    static uid_t cuid  = geteuid();
    static uid_t cgid  = getegid();
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

#if FUSE_USE_VERSION >= 30
static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags)
#else
static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
#endif
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
#if FUSE_USE_VERSION >= 30
        if (filler(buf, dirp.d_name, &st, 0, FUSE_FILL_DIR_PLUS))
#else
        if (filler(buf, dirp.d_name, &st, 0))
#endif
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

#if FUSE_USE_VERSION >= 30
static int xmp_rename(const char *from, const char *to, unsigned int flags)
#else
static int xmp_rename(const char *from, const char *to)
#endif
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

#if FUSE_USE_VERSION >= 30
static int xmp_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
#else
static int xmp_chmod(const char *path, mode_t mode)
#endif
{
    LOG_DEBUG(VALUE(path), VALUE(mode));
    CHECK_FS();
    int res = fs->chmod(path, mode);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(mode));
    return 0;
}

#if FUSE_USE_VERSION >= 30
static int xmp_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
#else
static int xmp_chown(const char *path, uid_t uid, gid_t gid)
#endif
{
    LOG_DEBUG(VALUE(path), VALUE(uid), VALUE(gid));
    CHECK_FS();
    int res = fs->chown(path, uid, gid);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(uid), VALUE(gid));
    return 0;
}

#if FUSE_USE_VERSION >= 30
static int xmp_truncate(const char *path, off_t size, struct fuse_file_info *fi)
#else
static int xmp_truncate(const char *path, off_t size)
#endif
{
    LOG_DEBUG(VALUE(path), VALUE(size));
    CHECK_FS();
    int res = fs->truncate(path, size);
    if(res) \
        LOG_ERROR_RETURN(0, -errno, VALUE(path), VALUE(size));
    return 0;
}

#if FUSE_USE_VERSION >= 30
static int xmp_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi)
#else
static int xmp_utimens(const char *path, const struct timespec ts[2])
#endif
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
    // REPORT_PERF(xmp_read, 1)
    // LOG_DEBUG(VALUE(path), VALUE(offset), VALUE(size));
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

#if FUSE_USE_VERSION < 30
static int xmp_getdir(const char* path, fuse_dirh_t dir, fuse_dirfil_t dirf)
{
    LOG_DEBUG(VALUE(path));
    return -ENOSYS;
}

static int xmp_utime(const char* path, struct utimbuf* tb)
{
    LOG_DEBUG(VALUE(path));
    return -ENOSYS;
}
#endif

static int xmp_mknod(const char* path,  mode_t mode, dev_t dev)
{
    LOG_DEBUG(VALUE(path));
    return -ENOSYS;
}

#if 0
static int xmp_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    CHECK_FS();
    int res = geti_file(fi)->ftruncate(size);
    return (res == 0) ? 0 : -errno;
}
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

#if FUSE_USE_VERSION < 35
static int xmp_ioctl(const char *path, int cmd, void *arg,
                      struct fuse_file_info *fi, unsigned int flags, void *data)
#else
static int xmp_ioctl(const char *path, unsigned int cmd, void *arg,
                      struct fuse_file_info *fi, unsigned int flags, void *data)
#endif
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
#if FUSE_USE_VERSION < 30
    .getdir      = xmp_getdir,
#endif
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
#if FUSE_USE_VERSION < 30
    .utime       = xmp_utime,
#endif
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
#if FUSE_USE_VERSION < 30
    .ftruncate   = nullptr,  /* The fgetattr and ftruncate handlers have become obsolete and have been removed */
    .fgetattr    = nullptr,  /* The fgetattr and ftruncate handlers have become obsolete and have been removed */
#endif
    .lock        = xmp_lock,
    .utimens     = xmp_utimens,
    .bmap        = xmp_bmap,
#if FUSE_USE_VERSION < 30
    flag_nullpath_ok:1,
    flag_nopath:1,
    flag_utime_omit_ok:1,
    flag_reserved:29,
#endif
    .ioctl       = xmp_ioctl,
    .poll        = xmp_poll,
    .write_buf   = nullptr, //xmp_write_buf,
    .read_buf    = nullptr, //xmp_read_buf,
    .flock       = xmp_flock,
    .fallocate   = xmp_fallocate,
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
    return run_fuse(argc, argv, &xmp_oper, NULL);
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

class FuseSessionLoop {
protected:
    struct fuse_session *se;
#if FUSE_USE_VERSION < 30
    struct fuse_chan *ch;
    IdentityPool<void, 32> bufpool;
    size_t bufsize = 0;
#endif
    ThreadPool<32> threadpool;
    EventLoop *loop;
    int ref = 1;
    condition_variable cond;

    int wait_for_readable(EventLoop *) {
        if (fuse_session_exited(se)) return -1;
#if FUSE_USE_VERSION < 30
        auto ret = wait_for_fd_readable(fuse_chan_fd(ch));
#else
        auto ret = wait_for_fd_readable(fuse_session_fd(se));
#endif
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                return 0;
            }
            return -1;
        }
        return 1;
    }

    static void *worker(void *args) {
        auto obj = (FuseSessionLoop *)args;
        struct fuse_buf fbuf;
        memset(&fbuf, 0, sizeof(fbuf));
#if FUSE_USE_VERSION < 30
        struct fuse_chan *tmpch = obj->ch;
        fbuf.mem = obj->bufpool.get();
        fbuf.size = obj->bufsize;
        DEFER({
            obj->bufpool.put(fbuf.mem);
        });
        auto res = fuse_session_receive_buf(obj->se, &fbuf, &tmpch);
#else
        auto res = fuse_session_receive_buf(obj->se, &fbuf);
#endif
        DEFER({
            if (--obj->ref == 0) obj->cond.notify_all();
        });
        if (res == -EINTR || res == -EAGAIN) return nullptr;
        if (res <= 0) {
            obj->loop->stop();
            return nullptr;
        }
#if FUSE_USE_VERSION < 30
        fuse_session_process_buf(obj->se, &fbuf, tmpch);
#else
        fuse_session_process_buf(obj->se, &fbuf);
#endif
        return nullptr;
    }

    int on_accept(EventLoop *) {
        ++ref;
        auto th = threadpool.thread_create(&FuseSessionLoop::worker, (void *)this);
        thread_yield_to(th);
        return 0;
    }

#if FUSE_USE_VERSION < 30
    int bufctor(void **buf) {
        *buf = malloc(bufsize);
        if (*buf == nullptr) return -1;
        return 0;
    }

    int bufdtor(void *buf) {
        free(buf);
        return 0;
    }
#endif

public:
    explicit FuseSessionLoop(struct fuse_session *session)
        : se(session),
#if FUSE_USE_VERSION < 30
          ch(fuse_session_next_chan(se, NULL)),
          bufpool({this, &FuseSessionLoop::bufctor},
                  {this, &FuseSessionLoop::bufdtor}),
          bufsize(fuse_chan_bufsize(ch)),
#endif
          threadpool(),
          loop(new_event_loop({this, &FuseSessionLoop::wait_for_readable},
                              {this, &FuseSessionLoop::on_accept})) {}

    ~FuseSessionLoop() {
        loop->stop();
        --ref;
        while (ref != 0) {
            cond.wait_no_lock();
        }

        delete loop;
    }

    void run() { loop->run(); }
};

static int fuse_session_loop_mpt(struct fuse_session *se) {
    FuseSessionLoop loop(se);
    loop.run();
    return 0;
}

#if FUSE_USE_VERSION >= 30
fuse* fuse3_setup(int argc, char *argv[], const struct ::fuse_operations *op,
           size_t op_size, char **mountpoint, int *multithreaded, void *user_data)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse *fuse;
    struct fuse_cmdline_opts opts;
    struct fuse_session * se;

    if (fuse_parse_cmdline(&args, &opts) != 0)
        return NULL;

    *mountpoint = opts.mountpoint;
    *multithreaded = !opts.singlethread;
    if (opts.show_version) {
        fuse_lowlevel_version();
        goto out1;
    }

    if (opts.show_help) {
        if(args.argv[0][0] != '\0')
            printf("usage: %s [options] <mountpoint>\n\n",
                   args.argv[0]);
        printf("FUSE options:\n");
        fuse_cmdline_help();
        fuse_lib_help(&args);
        goto out1;
    }

    if (!opts.show_help &&
        !opts.mountpoint) {
        fprintf(stderr, "error: no mountpoint specified\n");
        goto out1;
    }

    fuse = fuse_new(&args, op, op_size, user_data);
    fuse_opt_free_args(&args);
    if (fuse == NULL) {
        goto out1;
    }

    if (fuse_mount(fuse,opts.mountpoint) != 0) {
        goto out2;
    }

    if (fuse_daemonize(opts.foreground) != 0) {
        goto out3;
    }

    se = fuse_get_session(fuse);
    if (fuse_set_signal_handlers(se) != 0) {
        goto out3;
    }
    return fuse;

out3:
    fuse_unmount(fuse);
out2:
    fuse_destroy(fuse);
out1:
    free(opts.mountpoint);
    return NULL;
}
#endif

struct user_config {
    int threads;
};

#define USER_OPT(t, p, v) { t, offsetof(struct user_config, p), v }
struct fuse_opt user_opts[] = { USER_OPT("threads=%d", threads, 0), FUSE_OPT_END };

int run_fuse(int argc, char *argv[], const struct ::fuse_operations *op,
             void *user_data) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct user_config cfg{ .threads = 4, };
    fuse_opt_parse(&args, &cfg, user_opts, NULL);
    struct fuse *fuse;
    struct fuse_session* se;
    char *mountpoint;
    int multithreaded;
    int res;
    size_t op_size = sizeof(*(op));
#if FUSE_USE_VERSION < 30
    fuse = fuse_setup(args.argc, args.argv, op, op_size, &mountpoint, &multithreaded, user_data);
#else
    fuse = fuse3_setup(args.argc, args.argv, op, op_size, &mountpoint, &multithreaded, user_data);
#endif
    if (fuse == NULL) return 1;
    if (multithreaded) {
        if (cfg.threads < 1) cfg.threads = 1;
        if (cfg.threads > 64) cfg.threads = 64;
        se = fuse_get_session(fuse);
#if FUSE_USE_VERSION < 30
        auto ch = fuse_session_next_chan(se, NULL);
        auto fd = fuse_chan_fd(ch);
#else
        auto fd = fuse_session_fd(se);
#endif
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        std::vector<std::thread> ths;
        for (int i = 0; i < cfg.threads; ++i) {
          ths.emplace_back(std::thread([&]() {
              init(INIT_EVENT_EPOLL, INIT_IO_LIBAIO);
              DEFER(fini());
              if (fuse_session_loop_mpt(se) != 0) res = -1;
          }));
        }
        for (auto& th : ths) th.join();
        fuse_session_reset(se);
    } else {
        se = fuse_get_session(fuse);
        res = fuse_session_loop_mpt(se);
        fuse_session_reset(se);
    }
    fuse_remove_signal_handlers(fuse_get_session(fuse));
#if FUSE_USE_VERSION < 30
    fuse_unmount(mountpoint, fuse_session_next_chan(se, NULL));
#else
    fuse_unmount(fuse);
#endif
    fuse_destroy(fuse);
    free(mountpoint);
    fuse_opt_free_args(&args);
    if (res == -1) return 1;

    return 0;
}

}
}
