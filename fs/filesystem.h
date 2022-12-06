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
#include <cerrno>
#include <cstdarg>
#include <sys/uio.h>  // struct iovec
#include <sys/time.h> // struct timeval
#include <photon/common/stream.h>

#define UNIMPLEMENTED(func)  \
    virtual func             \
    {                        \
        errno = ENOSYS;      \
        return -1;           \
    }

#define UNIMPLEMENTED_POINTER(func)  \
    virtual func             \
    {                        \
        errno = ENOSYS;      \
        return nullptr;      \
    }

#define ECHECKSUM EUCLEAN

struct dirent;
struct stat;
struct statfs;
struct statvfs;
class iovector;
struct utimbuf;

namespace photon {
namespace fs
{
    const static size_t ALIGNMENT_4K = 4096;

    struct fiemap;
    class IFileSystem;
    class IFile : public IStream {
    public:
        virtual IFileSystem* filesystem()=0;
        virtual ssize_t pread(void *buf, size_t count, off_t offset)=0;
        virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset)=0;
        virtual ssize_t preadv_mutable(struct iovec *iov, int iovcnt, off_t offset)
        {
            return preadv(iov, iovcnt, offset);
        }
        virtual ssize_t preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            (void)flags;
            return preadv(iov, iovcnt, offset);
        }
        virtual ssize_t preadv2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            return preadv2(iov, iovcnt, offset, flags);
        }

        virtual ssize_t pwrite(const void *buf, size_t count, off_t offset)=0;
        virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset)=0;
        virtual ssize_t pwritev_mutable(struct iovec *iov, int iovcnt, off_t offset)
        {
            return pwritev(iov, iovcnt, offset);
        }
        virtual ssize_t pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            (void)flags;
            return pwritev(iov, iovcnt, offset);
        }
        virtual ssize_t pwritev2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags)
        {
            return pwritev2(iov, iovcnt, offset, flags);
        }

        virtual off_t lseek(off_t offset, int whence)=0;
        virtual int fsync()=0;
        virtual int fdatasync()=0;
        virtual int fchmod(mode_t mode)=0;
        virtual int fchown(uid_t owner, gid_t group)=0;
        virtual int fstat(struct stat *buf)=0;
        virtual int ftruncate(off_t length)=0;

        UNIMPLEMENTED(int fadvise(off_t offset, off_t len, int advice));
        UNIMPLEMENTED(int sync_file_range(off_t offset, off_t nbytes, unsigned int flags));
        UNIMPLEMENTED(int fallocate(int mode, off_t offset, off_t len));
        UNIMPLEMENTED(int fiemap(struct fiemap* map));   // query the extent map for

        // append write to the file
        // if `offset` is specified, it must be equal to current size of the
        // file; if `position` is specified, the latest file size will be stored
        // here upon return;
        UNIMPLEMENTED(ssize_t do_appendv(const struct iovec *iov, int iovcnt,
                                         off_t * /*IN*/ offset,
                                         off_t * /*OUT*/ position));
        ssize_t appendv(const struct iovec *iov, int iovcnt, off_t* offset, off_t* position) {
            return do_appendv(iov, iovcnt, offset, position);
        }
        template<typename=void> // in order to construction struct iovec without defination
        ssize_t append(const void *buf, size_t count, off_t* /*OUT*/ position = nullptr)
        {
            iovec v{(void*)buf, count};
            return do_appendv(&v, 1, nullptr, position);
        }
        ssize_t appendv(const struct iovec *iov, int iovcnt, off_t* /*OUT*/ position = nullptr)
        {
            return do_appendv(iov, iovcnt, nullptr, position);
        }
        template<typename=void> // in order to construction struct iovec without defination
        ssize_t pappend(const void *buf, size_t count, off_t offset, off_t* /*OUT*/ position = nullptr)
        {
            iovec v{(void*)buf, count};
            return do_appendv(&v, 1, &offset, position);
        }
        ssize_t pappendv(const struct iovec *iov, int iovcnt, off_t offset, off_t* /*OUT*/ position = nullptr)
        {
            return do_appendv(iov, iovcnt, &offset, position);
        }

                                                         // specified range. need fiemap.h.
        UNIMPLEMENTED(int vioctl(int request, va_list args));
        int ioctl(int request, ...)
        {
            va_list args;
            va_start(args, request);
            int ret = vioctl(request, args);
            va_end(args);
            return ret;
        }

        UNIMPLEMENTED_POINTER(Object* get_underlay_object(int i = 0));

        // De-allocate a range of space in the file.
        // Supported on at least the following filesystems:
        // *  XFS (since Linux 2.6.38)
        // *  ext4 (since Linux 3.0)
        // *  Btrfs (since Linux 3.7)
        // *  tmpfs(5) (since Linux 3.5)"
        int trim(off_t offset, off_t len);  // short-cuts for special mode of fallocate,
                                            // implementation placed in VirtualFile.cpp.

        // Clear a range of data in the file to all zero,
        // without freeing its space, as opposed to trim().
        // Supported natively on at least the following filesystems:
        // *  XFS (since Linux 3.15)
        // *  ext4, for extent-based files (since Linux 3.15)
        // *  SMB3 (since Linux 3.17)
        // *  Btrfs (since Linux 4.16)
        // If not natively supported, zero_range() will try to
        // trim() the range and then allocate space on that range.
        int zero_range(off_t offset, off_t len);    // short-cuts for special mode of fallocate,
                                                    // implementation placed in VirtualFile.cpp.

        // member function pointer to either pread() or pwrite()
        typedef ssize_t (IFile::*FuncPIO) (void *buf, size_t count, off_t offset);
        FuncPIO _and_pread()  { return &IFile::pread; }
        FuncPIO _and_pwrite() { return (FuncPIO)&IFile::pwrite; }
        bool is_readf(FuncPIO f) { return f == _and_pread(); }
        bool is_writef(FuncPIO f) { return f == _and_pwrite(); }

        // member function pointer to either preadv() or pwritev(), the non-const iovec* edition
        typedef ssize_t (IFile::*FuncPIOV_mutable) (struct iovec *iov, int iovcnt, off_t offset);
        FuncPIOV_mutable _and_preadv_mutable()  { return &IFile::preadv_mutable; }
        FuncPIOV_mutable _and_pwritev_mutable() { return &IFile::pwritev_mutable; }
        bool is_readf(FuncPIOV_mutable f) { return f == _and_preadv_mutable(); }
        bool is_writef(FuncPIOV_mutable f) { return f == _and_pwritev_mutable(); }

        // member function pointer to either preadv() or pwritev(), the const iovec* edition
        typedef ssize_t (IFile::*FuncPIOCV) (const struct iovec *iov, int iovcnt, off_t offset);
        FuncPIOCV _and_preadcv()  { return &IFile::preadv; }
        FuncPIOCV _and_pwritecv() { return &IFile::pwritev; }
        bool is_readf(FuncPIOCV f) { return f == _and_preadcv(); }
        bool is_writef(FuncPIOCV f) { return f == _and_pwritecv(); }

        // member function pointer to either preadv2() or pwritev2(), the non-const iovec* edition
        typedef ssize_t (IFile::*FuncPIOV2_mutable) (struct iovec *iov, int iovcnt, off_t offset, int flags);
        FuncPIOV2_mutable _and_preadv2_mutable()  { return &IFile::preadv2_mutable; }
        FuncPIOV2_mutable _and_pwritev2_mutable() { return &IFile::pwritev2_mutable; }
        bool is_readf(FuncPIOV2_mutable f) { return f == _and_preadv2_mutable(); }
        bool is_writef(FuncPIOV2_mutable f) { return f == _and_pwritev2_mutable(); }

        // member function pointer to either preadv2() or pwritev2(), the const iovec* edition
        typedef ssize_t (IFile::*FuncPIOCV2) (const struct iovec *iov, int iovcnt, off_t offset, int flags);
        FuncPIOCV2 _and_preadcv2()  { return &IFile::preadv2; }
        FuncPIOCV2 _and_pwritecv2() { return &IFile::pwritev2; }
        bool is_readf(FuncPIOCV2 f) { return f == _and_preadcv2(); }
        bool is_writef(FuncPIOCV2 f) { return f == _and_pwritecv2(); }
    };

    class IFileXAttr {
    public:
        virtual ~IFileXAttr() { }
        virtual ssize_t fgetxattr(const char *name, void *value, size_t size)=0;
        virtual ssize_t flistxattr(char *list, size_t size)=0;
        virtual int fsetxattr(const char *name, const void *value, size_t size, int flags)=0;
        virtual int fremovexattr(const char *name)=0;
    };

    class DIR : public Object
    {
    public:
        virtual int closedir()=0;
        virtual struct dirent* get()=0;    // return the current dirent, without moving to the next one
        virtual int next()=0;              // move to the next one
        virtual struct dirent* readdir()   // readdir() is the combination of get() and next()
        {
            auto rst = get();
            if (rst)
                next();
            return rst;
        }
        virtual void rewinddir()=0;
        virtual void seekdir(long loc)=0;
        virtual long telldir()=0;
        UNIMPLEMENTED_POINTER(Object* get_underlay_object(int i = 0));
    };

    class IFileSystem : public Object
    {
    public:
        virtual IFile* open(const char *pathname, int flags)=0;
        virtual IFile* open(const char *pathname, int flags, mode_t mode)=0;
        virtual IFile* creat(const char *pathname, mode_t mode)=0;
        virtual int mkdir(const char *pathname, mode_t mode)=0;
        virtual int rmdir(const char *pathname) = 0;
        virtual int symlink(const char *oldname, const char *newname)=0;
        virtual ssize_t readlink(const char *path, char *buf, size_t bufsiz)=0;
        virtual int link(const char *oldname, const char *newname)=0;
        virtual int rename(const char *oldname, const char *newname)=0;
        virtual int unlink(const char *filename)=0;
        virtual int chmod(const char *pathname, mode_t mode)=0;
        virtual int chown(const char *pathname, uid_t owner, gid_t group)=0;
        virtual int lchown(const char *pathname, uid_t owner, gid_t group)=0;
        virtual int statfs(const char *path, struct statfs *buf)=0;
        virtual int statvfs(const char *path, struct statvfs *buf)=0;
        virtual int stat(const char *path, struct stat *buf)=0;
        virtual int lstat(const char *path, struct stat *buf)=0;
        virtual int access(const char *pathname, int mode)=0;
        virtual int truncate(const char *path, off_t length)=0;
        virtual int utime(const char *path, const struct utimbuf *file_times)=0;
        virtual int utimes(const char *path, const struct timeval times[2])=0;
        virtual int lutimes(const char *path, const struct timeval times[2])=0;
        virtual int mknod(const char *path, mode_t mode, dev_t dev)=0;
        virtual int syncfs()=0;
        int sync() { return syncfs(); }

        UNIMPLEMENTED_POINTER(Object* get_underlay_object(int i = 0));

        virtual DIR* opendir(const char *name)=0;
        int closedir(DIR*& dirp)
        {
            int rst = dirp->closedir();
            delete dirp;
            dirp = nullptr; // to protect furhter deletion
            return rst;
        }
        struct dirent* readdir(DIR *dirp)
        {
            return dirp->readdir();
        }
        int readdir(DIR *dirp, struct dirent** dirpp)
        {
            *dirpp = dirp->readdir();
            return (*dirpp) ? 0 : -1;
        }
        void rewinddir(DIR *dirp)
        {
            dirp->rewinddir();
        }
        void seekdir(DIR *dirp, long loc)
        {
            dirp->seekdir(loc);
        }
        long telldir(DIR *dirp)
        {
            return dirp->telldir();
        }
    };

    class IFileSystemXAttr {
    public:
        virtual ~IFileSystemXAttr() { }
        virtual ssize_t getxattr(const char *path, const char *name, void *value, size_t size)=0;
        virtual ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size)=0;
        virtual ssize_t listxattr(const char *path, char *list, size_t size)=0;
        virtual ssize_t llistxattr(const char *path, char *list, size_t size)=0;
        virtual int setxattr(const char *path, const char *name, const void *value, size_t size, int flags)=0;
        virtual int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags)=0;
        virtual int removexattr(const char *path, const char *name)=0;
        virtual int lremovexattr(const char *path, const char *name)=0;
    };

    // intended use: for (auto& x: FileList(dirp)) { ... }
    struct FileList {
        DIR* dirp;
        bool autoDelete;
        FileList(DIR* dirp, bool autoDelete = false) :
            dirp(dirp), autoDelete(autoDelete) { }
        ~FileList() { if (autoDelete) delete dirp; }

        struct iterator
        {
            DIR* dirp;
            explicit iterator(DIR* dirp) : dirp(dirp) { }
            struct dirent& operator*() {
                return *dirp->get();
            }
            iterator& operator++() {
                dirp->next();
                return *this;
            }
            iterator operator++(int) {
                iterator rst = *this;
                ++(*this);
                return rst;
            }
            bool operator==(const iterator& rhs) const {
                if (!rhs.dirp && dirp) return !dirp->get();
                else if (rhs.dirp && !dirp) return !rhs.dirp->get();
                else return dirp == rhs.dirp;
            }
            bool operator!=(const iterator& rhs) const {
                return !(*this == rhs);
            }
        };

        iterator begin() {
//            dirp->rewinddir();    // not necessary iterate from the begining
            return iterator(dirp);
        }
        iterator end() {
            return iterator(nullptr);
        }
    };
}
}
