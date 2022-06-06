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
    // this provides the basics for a implementing a "virtual" file
    // with self-managed offset, and default I/O routines.
    // users are supposed to derive from it, and implement positioned
    // I/O routines.
    class VirtualFile : public IFile
    {
    public:
        virtual off_t lseek(off_t offset, int whence) override;
        virtual ssize_t read(void *buf, size_t count) override;
        virtual ssize_t readv(const struct iovec *iov, int iovcnt) override;
        virtual ssize_t write(const void *buf, size_t count) override;
        virtual ssize_t writev(const struct iovec *iov, int iovcnt) override;
        virtual ssize_t pread(void *buf, size_t count, off_t offset) override;
        virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override;
        virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override;
        virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override;

    protected:
        off_t m_offset = 0;
        ssize_t piov_nocopy(FuncPIO f, const struct iovec *iov, int iovcnt, off_t offset);
        ssize_t piov_copy  (FuncPIO f, const struct iovec *iov, int iovcnt, off_t offset);
        ssize_t piov       (FuncPIO f, const struct iovec *iov, int iovcnt, off_t offset)
        {
            return piov_copy(f, iov, iovcnt, offset);
        }
        ssize_t buffered_piov(FuncPIO f, char *&buf, size_t count, const struct iovec *iov, int iovcnt, off_t offset);
    };

    // even pwrite / pwritev are not needed to implement
    class VirtualReadOnlyFile : public VirtualFile
    {
    public:
        UNIMPLEMENTED(ssize_t pwrite(const void *buf, size_t count, off_t offset) override);
        UNIMPLEMENTED(ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override);
        UNIMPLEMENTED(int fsync() override);
        UNIMPLEMENTED(int fdatasync() override);
        UNIMPLEMENTED(int close() override);
        UNIMPLEMENTED(int fchmod(mode_t mode) override);
        UNIMPLEMENTED(int fchown(uid_t owner, gid_t group) override);
        UNIMPLEMENTED(int ftruncate(off_t length) override);
    };

}
}
