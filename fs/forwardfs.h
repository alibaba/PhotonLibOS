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
#include <photon/fs/filesystem.h>

namespace photon {
namespace fs
{
    template<class IFileBase>
    class ForwardFileBase : public IFileBase
    {
    protected:
        IFileBase* m_file;
        ForwardFileBase(IFileBase* file)
        {
            m_file = file;
        }
        virtual int close() override
        {
            return m_file->close();
        }
        virtual ssize_t pread(void *buf, size_t count, off_t offset) override
        {
            return m_file->pread(buf, count, offset);
        }
        virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override
        {
            return m_file->pwrite(buf, count, offset);
        }
        virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            return m_file->preadv(iov, iovcnt, offset);
        }
        virtual ssize_t preadv_mutable(struct iovec *iov, int iovcnt, off_t offset) override
        {
            return m_file->preadv_mutable(iov, iovcnt, offset);
        }
        virtual ssize_t preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            return m_file->preadv2(iov, iovcnt, offset, flags);
        }
        virtual ssize_t preadv2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            return m_file->preadv2_mutable(iov, iovcnt, offset, flags);
        }
        virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            return m_file->pwritev(iov, iovcnt, offset);
        }
        virtual ssize_t pwritev_mutable(struct iovec *iov, int iovcnt, off_t offset) override
        {
            return m_file->pwritev_mutable(iov, iovcnt, offset);
        }
        virtual ssize_t pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            return m_file->pwritev2(iov, iovcnt, offset, flags);
        }
        virtual ssize_t pwritev2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            return m_file->pwritev2_mutable(iov, iovcnt, offset, flags);
        }
        virtual ssize_t read(void *buf, size_t count) override
        {
            return m_file->read(buf, count);
        }
        virtual ssize_t readv(const struct iovec *iov, int iovcnt) override
        {
            return m_file->readv(iov, iovcnt);
        }
        virtual ssize_t readv_mutable(struct iovec *iov, int iovcnt) override
        {
            return m_file->readv_mutable(iov, iovcnt);
        }
        virtual ssize_t write(const void *buf, size_t count) override
        {
            return m_file->write(buf, count);
        }
        virtual ssize_t writev(const struct iovec *iov, int iovcnt) override
        {
            return m_file->writev(iov, iovcnt);
        }
        virtual ssize_t writev_mutable(struct iovec *iov, int iovcnt) override
        {
            return m_file->writev_mutable(iov, iovcnt);
        }
        virtual IFileSystem* filesystem() override
        {
            return m_file->filesystem();
        }
        virtual off_t lseek(off_t offset, int whence) override
        {
            return m_file->lseek(offset, whence);
        }
        virtual int fsync() override
        {
            return m_file->fsync();
        }
        virtual int fdatasync() override
        {
            return m_file->fdatasync();
        }
        virtual int fchmod(mode_t mode) override
        {
            return m_file->fchmod(mode);
        }
        virtual int fchown(uid_t owner, gid_t group) override
        {
            return m_file->fchown(owner, group);
        }
        virtual int fstat(struct stat *buf) override
        {
            return m_file->fstat(buf);
        }
        virtual int ftruncate(off_t length) override
        {
            return m_file->ftruncate(length);
        }
        virtual int sync_file_range(off_t offset, off_t nbytes, unsigned int flags) override
        {
            return m_file->sync_file_range(offset, nbytes, flags);
        }
        virtual ssize_t do_appendv(const struct iovec *iov, int iovcnt,
                                      off_t*  /*IN*/ offset = nullptr,
                                      off_t* /*OUT*/ position = nullptr) override
        {
            return m_file->do_appendv(iov, iovcnt, offset, position);
        }
        virtual int fallocate(int mode, off_t offset, off_t len) override
        {
            return m_file->fallocate(mode, offset, len);
        }
        virtual int fadvise(off_t offset, off_t len, int advice) override
        {
            return m_file->fadvise(offset, len, advice);
        }
        virtual int fiemap(struct fiemap* map) override
        {
            return m_file->fiemap(map);
        }
        virtual int vioctl(int request, va_list args) override
        {
            return m_file->vioctl(request, args);
        }
    };
    template<class IFileBase>
    class ForwardFileBase_Ownership : public ForwardFileBase<IFileBase>
    {
    protected:
        bool m_ownership;
        ForwardFileBase_Ownership(IFileBase* file, bool ownership) : ForwardFileBase<IFileBase>(file)
        {
            m_ownership = ownership;
        }
        virtual ~ForwardFileBase_Ownership() override
        {
            if (m_ownership) delete this->m_file;
        }
        virtual int close() override
        {
            return m_ownership ? this->m_file->close() : 0;
        }
    };
    using ForwardFile = ForwardFileBase<IFile>;
    using ForwardFile_Ownership = ForwardFileBase_Ownership<IFile>;

    template<class IFileSystemBase>
    class ForwardFSBase : public IFileSystemBase
    {
    protected:
        IFileSystemBase* m_fs;
        ForwardFSBase(IFileSystemBase* fs)
        {
            m_fs = fs;
        }
        virtual IFile* open(const char *pathname, int flags) override
        {
            return m_fs->open(pathname, flags);
        }
        virtual IFile* open(const char *pathname, int flags, mode_t mode) override
        {
            return m_fs->open(pathname, flags, mode);
        }
        virtual IFile* creat(const char *pathname, mode_t mode) override
        {
            return m_fs->creat(pathname, mode);
        }
        virtual int mkdir (const char *pathname, mode_t mode) override
        {
            return m_fs->mkdir(pathname, mode);
        }
        virtual int rmdir(const char *pathname) override
        {
            return m_fs->rmdir(pathname);
        }
        virtual int symlink (const char *oldname, const char *newname) override
        {
            return m_fs->symlink(oldname, newname);
        }
        virtual ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) override
        {
            return m_fs->readlink(pathname, buf, bufsiz);
        }
        virtual int link(const char *oldname, const char *newname) override
        {
            return m_fs->link(oldname, newname);
        }
        virtual int rename(const char *oldname, const char *newname) override
        {
            return m_fs->rename(oldname, newname);
        }
        virtual int unlink (const char *pathname) override
        {
            return m_fs->unlink(pathname);
        }
        virtual int chmod(const char *pathname, mode_t mode) override
        {
            return m_fs->chmod(pathname, mode);
        }
        virtual int chown(const char *pathname, uid_t owner, gid_t group) override
        {
            return m_fs->chown(pathname, owner, group);
        }
        virtual int lchown(const char *pathname, uid_t owner, gid_t group) override
        {
            return m_fs->lchown(pathname, owner, group);
        }
        virtual DIR* opendir(const char *pathname) override
        {
            return m_fs->opendir(pathname);
        }
        virtual int stat(const char *path, struct stat *buf) override
        {
            return m_fs->stat(path, buf);
        }
        virtual int lstat(const char *path, struct stat *buf) override
        {
            return m_fs->lstat(path, buf);
        }
        virtual int access(const char *path, int mode) override
        {
            return m_fs->access(path, mode);
        }
        virtual int truncate(const char *path, off_t length) override
        {
            return m_fs->truncate(path, length);
        }
        virtual int syncfs() override
        {
            return m_fs->syncfs();
        }
        virtual int statfs(const char *path, struct statfs *buf) override
        {
            return m_fs->statfs(path, buf);
        }
        virtual int statvfs(const char *path, struct statvfs *buf) override
        {
            return m_fs->statvfs(path, buf);
        }
        virtual int utime(const char *path, const struct utimbuf *file_times) override
        {
            return m_fs->utime(path, file_times);
        }
        virtual int utimes(const char *path, const struct timeval times[2]) override
        {
            return m_fs->utimes(path, times);
        }
        virtual int lutimes(const char *path, const struct timeval times[2]) override
        {
            return m_fs->lutimes(path, times);
        }
        virtual int mknod(const char *path, mode_t mode, dev_t dev) override
        {
            return m_fs->mknod(path, mode, dev);
        }
        /*
        virtual ssize_t getxattr(const char *path, const char *name, void *value, size_t size) override
        {
            return m_fs->getxattr(path, name, value, size);
        }
        virtual ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) override
        {
            return m_fs->lgetxattr(path, name, value, size);
        }
        virtual ssize_t listxattr(const char *path, char *list, size_t size) override
        {
            return m_fs->listxattr(path, list, size);
        }
        virtual ssize_t llistxattr(const char *path, char *list, size_t size) override
        {
            return m_fs->llistxattr(path, list, size);
        }
        virtual int setxattr(const char *path, const char *name, const void *value, size_t size, int flags) override
        {
            return m_fs->setxattr(path, name, value, size, flags);
        }
        virtual int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) override
        {
            return m_fs->lsetxattr(path, name, value, size, flags);
        }
        virtual int removexattr(const char *path, const char *name) override
        {
            return m_fs->removexattr(path, name);
        }
        virtual int lremovexattr(const char *path, const char *name) override
        {
            return m_fs->lremovexattr(path, name);
        }
        */
    };
    template<class IFileSystemBase>
    class ForwardFSBase_Ownership : public ForwardFSBase<IFileSystemBase>
    {
    protected:
        bool m_ownership;
        ForwardFSBase_Ownership(IFileSystemBase* fs, bool ownership) : ForwardFSBase<IFileSystemBase>(fs)
        {
            m_ownership = ownership;
        }
        virtual ~ForwardFSBase_Ownership() override
        {
            if (m_ownership) delete this->m_fs;
        }
    };
    using ForwardFS = ForwardFSBase<IFileSystem>;
    using ForwardFS_Ownership = ForwardFSBase_Ownership<IFileSystem>;
}
}
