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
#include <cstdarg>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <photon/common/async_stream.h>

struct dirent;
class iovector;

namespace photon {
namespace fs
{
    struct fiemap;
    class IAsyncFileSystem;
    class IAsyncFile : public IAsyncStream
    {
    public:
        virtual IAsyncFileSystem* filesystem() = 0;
        DEFINE_ASYNC(ssize_t, pread, void *buf, size_t count, off_t offset);
        DEFINE_ASYNC(ssize_t, preadv, const struct iovec *iov, int iovcnt, off_t offset);
        EXPAND_FUNC(ssize_t, preadv_mutable, struct iovec *iov, int iovcnt, off_t offset)
        {
            preadv(iov, iovcnt, offset, done, timeout);
        }
        EXPAND_FUNC(ssize_t, preadv2, const struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            (void)flags;
            preadv(iov, iovcnt, offset, done, timeout);
        }
        EXPAND_FUNC(ssize_t, preadv2_mutable, struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            preadv2(iov, iovcnt, offset, flags, done, timeout);
        }

        DEFINE_ASYNC(ssize_t, pwrite, const void *buf, size_t count, off_t offset);
        DEFINE_ASYNC(ssize_t, pwritev, const struct iovec *iov, int iovcnt, off_t offset);
        EXPAND_FUNC(ssize_t, pwritev_mutable, struct iovec *iov, int iovcnt, off_t offset)
        {
            pwritev(iov, iovcnt, offset, done, timeout);
        }
        EXPAND_FUNC(ssize_t, pwritev2, const struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            (void)flags;
            pwritev(iov, iovcnt, offset, done, timeout);
        }
        EXPAND_FUNC(ssize_t, pwritev2_mutable, struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            pwritev2(iov, iovcnt, offset, flags, done, timeout);
        }

        DEFINE_ASYNC(off_t, lseek, off_t offset, int whence);
        DEFINE_ASYNC0(int, fsync);
        DEFINE_ASYNC0(int, fdatasync);
        DEFINE_ASYNC0(int, close);
        DEFINE_ASYNC(int, fchmod, mode_t mode);
        DEFINE_ASYNC(int, fchown, uid_t owner, gid_t group);
        DEFINE_ASYNC(int, fstat, struct stat *buf);
        DEFINE_ASYNC(int, ftruncate, off_t length);

        UNIMPLEMENTED_ASYNC(int, sync_file_range, off_t offset, off_t nbytes, unsigned int flags);
        // UNIMPLEMENTED_ASYNC(ssize_t, append, const void *buf, size_t count, off_t* position);
        UNIMPLEMENTED_ASYNC(ssize_t, do_appendv, const struct iovec *iov, int iovcnt, off_t* offset, off_t* position);
        UNIMPLEMENTED_ASYNC(int, fallocate, int mode, off_t offset, off_t len);
        UNIMPLEMENTED_ASYNC(int, trim, off_t offset, size_t len);
        UNIMPLEMENTED_ASYNC(int, fiemap, struct fiemap* map);   // query the extent map for
                                                                    // specified range. need fiemap.h.
        virtual int vioctl(int request, va_list args) { errno = ENOSYS; return -1; }
        int ioctl(int request, ...)
        {
            va_list args;
            va_start(args, request);
            int ret = vioctl(request, args);
            va_end(args);
            return ret;
        }

        const static uint32_t OPID_PREAD     = 5;
        const static uint32_t OPID_PREADV    = 6;
        const static uint32_t OPID_PWRITE    = 7;
        const static uint32_t OPID_PWRITEV   = 8;
        const static uint32_t OPID_LSEEK     = 9;
        const static uint32_t OPID_FSYNC     = 10;
        const static uint32_t OPID_FDATASYNC = 11;
        const static uint32_t OPID_CLOSE     = 12;
        const static uint32_t OPID_FCHMOD    = 13;
        const static uint32_t OPID_FCHOWN    = 14;
        const static uint32_t OPID_FSTAT     = 15;
        const static uint32_t OPID_FTRUNCATE = 16;
        const static uint32_t OPID_TRIM      = 17;
        const static uint32_t OPID_SYNC_FILE_RANGE = 18;
        const static uint32_t OPID_FALLOCATE = 19;
        const static uint32_t OPID_APPEND    = 20;
        const static uint32_t OPID_APPENDV   = 21;
        const static uint32_t OPID_VIOCTL    = 22;

        using FuncPIO = AsyncFunc<ssize_t, IAsyncFile, void*, size_t, off_t>;
        FuncPIO _and_pread()  { return &IAsyncFile::pread; }
        FuncPIO _and_pwrite() { return (FuncPIO)&IAsyncFile::pwrite; }
        bool is_readf(FuncPIO f) { return f == _and_pread(); }
        bool is_writef(FuncPIO f) { return f == _and_pwrite(); }

        using FuncPIOV_mutable = AsyncFunc<ssize_t, IAsyncFile, struct iovec*, int, off_t>;
        FuncPIOV_mutable _and_preadv_mutable()  { return &IAsyncFile::preadv_mutable; }
        FuncPIOV_mutable _and_pwritev_mutable() { return &IAsyncFile::pwritev_mutable; }
        bool is_readf(FuncPIOV_mutable f) { return f == _and_preadv_mutable(); }
        bool is_writef(FuncPIOV_mutable f) { return f == _and_pwritev_mutable(); }

        using FuncPIOCV = AsyncFunc<ssize_t, IAsyncFile, const struct iovec*, int, off_t>;
        FuncPIOCV _and_preadcv()  { return &IAsyncFile::preadv; }
        FuncPIOCV _and_pwritecv() { return &IAsyncFile::pwritev; }
        bool is_readf(FuncPIOCV f) { return f == _and_preadcv(); }
        bool is_writef(FuncPIOCV f) { return f == _and_pwritecv(); }

        using FuncPIOV2_mutable = AsyncFunc<ssize_t, IAsyncFile, struct iovec*, int, off_t, int>;
        FuncPIOV2_mutable _and_preadv2_mutable()  { return &IAsyncFile::preadv2_mutable; }
        FuncPIOV2_mutable _and_pwritev2_mutable() { return &IAsyncFile::pwritev2_mutable; }
        bool is_readf(FuncPIOV2_mutable f) { return f == _and_preadv2_mutable(); }
        bool is_writef(FuncPIOV2_mutable f) { return f == _and_pwritev2_mutable(); }

        using FuncPIOCV2 = AsyncFunc<ssize_t, IAsyncFile, const struct iovec*, int, off_t, int>;
        FuncPIOCV2 _and_preadcv2()  { return &IAsyncFile::preadv2; }
        FuncPIOCV2 _and_pwritecv2() { return &IAsyncFile::pwritev2; }
        bool is_readf(FuncPIOCV2 f) { return f == _and_preadcv2(); }
        bool is_writef(FuncPIOCV2 f) { return f == _and_pwritecv2(); }

        UNIMPLEMENTED_ASYNC(ssize_t, pread0c, off_t offset, size_t count, iovector* iov);
        UNIMPLEMENTED_ASYNC(ssize_t, pwrite0c, off_t offset, iovector* iov);
        const static uint32_t OPID_PREAD0C   = 23;
        const static uint32_t OPID_PWIRTE0C  = 24;
        const static uint32_t OPID_PREADV2   = 25;
        const static uint32_t OPID_PWRITEV2  = 26;
    };

    class IAsyncFileXAttr
    {
    public:
        virtual ~IAsyncFileXAttr() { }
        DEFINE_ASYNC(ssize_t, fgetxattr, const char *name, void *value, size_t size);
        DEFINE_ASYNC(ssize_t, flistxattr, char *list, size_t size);
        DEFINE_ASYNC(int, fsetxattr, const char *name, const void *value, size_t size, int flags);
        DEFINE_ASYNC(int, fremovexattr, const char *name);

        const static uint32_t OPID_FGETXATTR   = 30;
        const static uint32_t OPID_FLISTXATTR  = 31;
        const static uint32_t OPID_FSETXATTR   = 32;
        const static uint32_t OPID_FREMOVEXATTR  = 33;
    };

    class AsyncDIR : public Object
    {
    public:
        virtual ~AsyncDIR() { }
        DEFINE_ASYNC0(int, closedir);
        DEFINE_ASYNC0(dirent*, get);
        DEFINE_ASYNC0(int, next);
        DEFINE_ASYNC0(void, rewinddir);
        DEFINE_ASYNC(void, seekdir, long loc);
        DEFINE_ASYNC0(long, telldir);

        const static uint32_t OPID_CLOSEDIR     = 128;
        const static uint32_t OPID_GETDIR       = 129;
        const static uint32_t OPID_NEXTDIR      = 130;
        const static uint32_t OPID_REWINDDIR    = 131;
        const static uint32_t OPID_SEEKDIR      = 132;
        const static uint32_t OPID_TELLDIR      = 133;
    };

    class IAsyncFileSystem : public IAsyncBase
    {
    public:
        DEFINE_ASYNC(IAsyncFile*, open, const char *pathname, int flags);
        DEFINE_ASYNC(IAsyncFile*, open, const char *pathname, int flags, mode_t mode);
        DEFINE_ASYNC(IAsyncFile*, creat, const char *pathname, mode_t mode);
        DEFINE_ASYNC(int, mkdir, const char *pathname, mode_t mode);
        DEFINE_ASYNC(int, rmdir, const char *pathname);
        DEFINE_ASYNC(int, symlink, const char *oldname, const char *newname);
        DEFINE_ASYNC(ssize_t, readlink, const char *path, char *buf, size_t bufsiz);
        DEFINE_ASYNC(int, link, const char *oldname, const char *newname);
        DEFINE_ASYNC(int, rename, const char *oldname, const char *newname);
        DEFINE_ASYNC(int, unlink, const char *filename);
        DEFINE_ASYNC(int, chmod, const char *pathname, mode_t mode);
        DEFINE_ASYNC(int, chown, const char *pathname, uid_t owner, gid_t group);
        DEFINE_ASYNC(int, lchown, const char *pathname, uid_t owner, gid_t group);
        DEFINE_ASYNC(int, statfs, const char *path, struct statfs *buf);
        DEFINE_ASYNC(int, statvfs, const char *path, struct statvfs *buf);
        DEFINE_ASYNC(int, stat, const char *path, struct stat *buf);
        DEFINE_ASYNC(int, lstat, const char *path, struct stat *buf);
        DEFINE_ASYNC(int, access, const char *pathname, int mode);
        DEFINE_ASYNC(int, truncate, const char *path, off_t length);
        DEFINE_ASYNC(int, utime, const char *path, const struct utimbuf *file_times);
        DEFINE_ASYNC(int, utimes, const char *path, const struct timeval times[2]);
        DEFINE_ASYNC(int, lutimes, const char *path, const struct timeval times[2]);
        DEFINE_ASYNC(int, mknod, const char *path, mode_t mode, dev_t dev);
        DEFINE_ASYNC0(int, syncfs);

        virtual Object* get_underlay_object(int i = 0) { errno = ENOSYS; return nullptr; }

        DEFINE_ASYNC(AsyncDIR*, opendir, const char *name);

        const static uint32_t OPID_OPEN     = 64;
        const static uint32_t OPID_CREATE   = 65;
        const static uint32_t OPID_MKDIR    = 66;
        const static uint32_t OPID_RMDIR    = 67;
        const static uint32_t OPID_SYMLINK  = 68;
        const static uint32_t OPID_READLINK = 69;
        const static uint32_t OPID_LINK     = 70;
        const static uint32_t OPID_RENAME   = 71;
        const static uint32_t OPID_UNLINK   = 72;
        const static uint32_t OPID_CHMOD    = 73;
        const static uint32_t OPID_CHOWN    = 74;
        const static uint32_t OPID_LCHOWN   = 75;
        const static uint32_t OPID_STATFS   = 76;
        const static uint32_t OPID_STATVFS  = 77;
        const static uint32_t OPID_STAT     = 78;
        const static uint32_t OPID_LSTAT    = 79;
        const static uint32_t OPID_ACCESS   = 80;
        const static uint32_t OPID_TRUNCATE = 81;
        const static uint32_t OPID_SYNCFS   = 82;
        const static uint32_t OPID_OPENDIR  = 83;
        const static uint32_t OPID_UTIME    = 84;
        const static uint32_t OPID_UTIMES   = 85;
        const static uint32_t OPID_LUTIMES  = 86;
        const static uint32_t OPID_MKNOD    = 87;
    };

    class IAsyncFileSystemXAttr
    {
    public:
        virtual ~IAsyncFileSystemXAttr() { }

        DEFINE_ASYNC(ssize_t, getxattr, const char *path, const char *name, void *value, size_t size);
        DEFINE_ASYNC(ssize_t, lgetxattr, const char *path, const char *name, void *value, size_t size);
        DEFINE_ASYNC(ssize_t, listxattr, const char *path, char *list, size_t size);
        DEFINE_ASYNC(ssize_t, llistxattr, const char *path, char *list, size_t size);
        DEFINE_ASYNC(int, setxattr, const char *path, const char *name, const void *value, size_t size, int flags);
        DEFINE_ASYNC(int, lsetxattr, const char *path, const char *name, const void *value, size_t size, int flags);
        DEFINE_ASYNC(int, removexattr, const char *path, const char *name);
        DEFINE_ASYNC(int, lremovexattr, const char *path, const char *name);

        const static uint32_t OPID_GETXATTR   = 90;
        const static uint32_t OPID_LGETXATTR   = 91;
        const static uint32_t OPID_LISTXATTR  = 92;
        const static uint32_t OPID_LLISTXATTR  = 93;
        const static uint32_t OPID_SETXATTR   = 94;
        const static uint32_t OPID_LSETXATTR   = 95;
        const static uint32_t OPID_REMOVEXATTR  = 96;
        const static uint32_t OPID_LREMOVEXATTR  = 97;
    };

    class IFile;
    class IFileSystem;
    class DIR;
    extern "C"
    {
        // The adaptors wrap an async file/fs/dir object into a sync one, assuming
        // that the underlay file/fs/dir object will perform actions in other OS
        // thread(s) (not necessarily), and they will call back the adaptors from
        // their own thread, so the adaptors will act accordingly in a thread-safe manner.

        // Create adaptors for async file/fs/dir, obtaining their ownship, so deleting
        // the adaptor objects means deleting the underlay objects, too.
        IFile*       new_async_file_adaptor(IAsyncFile* afile, uint64_t timeout = -1);
        IFileSystem* new_async_fs_adaptor(IAsyncFileSystem* afs, uint64_t timeout = -1);
        DIR*         new_async_dir_adaptor(AsyncDIR* adir, uint64_t timeout = -1);

        //===================================================================================

        // The adaptors wrap an *synchronized* file/fs/dir object that blocks current
        // *kernel* thread when doing I/O, and return another *synchronized* file/fs/dir
        // object that blocks current photon thread only, by delegating the reuqests to
        // other kernel threads.

        // Note that, the underlay file/fs/dir must NOT interact with photon.

        // Create adaptors for sync file/fs/dir, obtaining their ownship, so deleting
        // the adaptor objects means deleting the underlay objects, too.
        IFile*       new_sync_file_adaptor(IFile* file);
        IFileSystem* new_sync_fs_adaptor(IFileSystem* fs);
        DIR*         new_sync_dir_adaptor(DIR* dir);
    }
}
}
