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

#include <gmock/gmock.h>
#include <photon/fs/filesystem.h>

namespace PMock {
    using namespace photon::fs;
    using photon::fs::DIR;
    using photon::fs::fiemap;

    class MockNullFile : public IFile, public IFileXAttr {
    public:
        MOCK_METHOD0(filesystem, IFileSystem*());
        MOCK_METHOD3(pread, ssize_t(void*, size_t, off_t));
        MOCK_METHOD3(preadv, ssize_t(const struct iovec*, int, off_t));
        MOCK_METHOD3(pwrite, ssize_t(const void *buf, size_t, off_t));
        MOCK_METHOD3(pwritev, ssize_t(const struct iovec*, int, off_t));
        MOCK_METHOD2(lseek, off_t(off_t offset, int whence));
        MOCK_METHOD0(fsync, int());
        MOCK_METHOD0(fdatasync, int());
        MOCK_METHOD1(fchmod, int(mode_t mode));
        MOCK_METHOD2(fchown, int(uid_t owner, gid_t group));
        MOCK_METHOD1(fstat, int(struct stat *buf));
        MOCK_METHOD1(ftruncate, int(off_t length));
        MOCK_METHOD0(close, int());
        MOCK_METHOD2(read, ssize_t(void *buf, size_t count));
        MOCK_METHOD2(readv, ssize_t(const struct iovec *iov, int iovcnt));
        MOCK_METHOD2(write, ssize_t(const void *buf, size_t count));
        MOCK_METHOD2(writev, ssize_t(const struct iovec *iov, int iovcnt));
        MOCK_METHOD3(sync_file_range, int(off_t, off_t, unsigned int));
        MOCK_METHOD4(do_appendv, ssize_t(const struct iovec*, int, off_t*, off_t*));
        MOCK_METHOD3(fallocate, int(int, off_t, off_t));
        MOCK_METHOD2(trim, int(off_t, off_t));
        MOCK_METHOD1(fiemap, int(struct fiemap *p));
        MOCK_METHOD2(vioctl, int(int, va_list));
        MOCK_METHOD3(fgetxattr, ssize_t(const char*, void*, size_t));
        MOCK_METHOD2(flistxattr, ssize_t(char*, size_t));
        MOCK_METHOD4(fsetxattr, int(const char*, const void*, size_t, int));
        MOCK_METHOD1(fremovexattr, int(const char*));
    };

    class MockNullFileSystem : public IFileSystem, public IFileSystemXAttr{
    public:
        MOCK_METHOD2(open, IFile*(const char *pathname, int flags));
        MOCK_METHOD3(open, IFile*(const char *pathname, int flags, mode_t mode));
        MOCK_METHOD2(creat, IFile*(const char *pathname, mode_t mode));
        MOCK_METHOD2(mkdir, int(const char *pathname, mode_t mode));
        MOCK_METHOD1(rmdir, int(const char *pathname));
        MOCK_METHOD2(symlink, int(const char *oldname, const char *newname));
        MOCK_METHOD3(readlink, ssize_t(const char *path, char *buf, size_t bufsiz));
        MOCK_METHOD2(link, int(const char *oldname, const char *newname));
        MOCK_METHOD2(rename, int(const char *oldname, const char *newname));
        MOCK_METHOD1(unlink, int(const char *filename));
        MOCK_METHOD2(chmod, int(const char *pathname, mode_t mode));
        MOCK_METHOD3(chown, int(const char *pathname, uid_t owner, gid_t group));
        MOCK_METHOD3(lchown, int(const char *pathname, uid_t owner, gid_t group));
        MOCK_METHOD2(statfs, int(const char *path, struct ::statfs *buf));
        MOCK_METHOD2(statvfs, int(const char *path, struct ::statvfs *buf));
        MOCK_METHOD2(stat, int(const char *path, struct stat *buf));
        MOCK_METHOD2(lstat, int(const char *path, struct stat *buf));
        MOCK_METHOD2(access, int(const char *pathname, int mode));
        MOCK_METHOD2(truncate, int(const char *path, off_t length));
        MOCK_METHOD2(utime, int(const char *path, const struct utimbuf *file_times));
        MOCK_METHOD2(utimes, int(const char *path, const struct timeval times[2]));
        MOCK_METHOD2(lutimes, int(const char *path, const struct timeval times[2]));
        MOCK_METHOD3(mknod, int(const char *path, mode_t mode, dev_t dev));
        MOCK_METHOD0(syncfs, int());
        MOCK_METHOD1(opendir, DIR*(const char *name));
        MOCK_METHOD4(getxattr, ssize_t(const char*, const char*, void*, size_t));
        MOCK_METHOD4(lgetxattr, ssize_t(const char*, const char*, void*, size_t));
        MOCK_METHOD3(listxattr, ssize_t(const char*, char*, size_t));
        MOCK_METHOD3(llistxattr, ssize_t(const char*, char*, size_t));
        MOCK_METHOD5(setxattr, int(const char*, const char*, const void*, size_t, int));
        MOCK_METHOD5(lsetxattr, int(const char*, const char*, const void*, size_t, int));
        MOCK_METHOD2(removexattr, int(const char*, const char*));
        MOCK_METHOD2(lremovexattr, int(const char*, const char*));
    };

    class MockNullDIR : public DIR {
    public:
        MOCK_METHOD0(closedir, int());
        MOCK_METHOD0(next, int());
        MOCK_METHOD0(get, struct dirent*());
        MOCK_METHOD0(readdir, struct dirent*());
        MOCK_METHOD0(rewinddir, void());
        MOCK_METHOD1(seekdir, void(long loc));
        MOCK_METHOD0(telldir, long());
    };
}
