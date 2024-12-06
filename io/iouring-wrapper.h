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

#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <poll.h>
#include <cstdint>
#include <cerrno>

namespace photon {

static const uint64_t IouringFixedFileFlag = 1UL << 32;

ssize_t iouring_pread(int fd, void* buf, size_t count, off_t offset, uint64_t flags = 0, uint64_t timeout = -1);

ssize_t iouring_pwrite(int fd, const void* buf, size_t count, off_t offset, uint64_t flags = 0, uint64_t timeout = -1);

ssize_t iouring_preadv(int fd, const iovec* iov, int iovcnt, off_t offset, uint64_t flags = 0, uint64_t timeout = -1);

ssize_t iouring_pwritev(int fd, const iovec* iov, int iovcnt, off_t offset, uint64_t flags = 0, uint64_t timeout = -1);

ssize_t iouring_send(int fd, const void* buf, size_t len, uint64_t flags = 0, uint64_t timeout = -1);

ssize_t iouring_send_zc(int fd, const void* buf, size_t len, uint64_t flags = 0, uint64_t timeout = -1);

ssize_t iouring_sendmsg(int fd, const msghdr* msg, uint64_t flags = 0, uint64_t timeout = -1);

ssize_t iouring_sendmsg_zc(int fd, const msghdr* msg, uint64_t flags = 0, uint64_t timeout = -1);

ssize_t iouring_recv(int fd, void* buf, size_t len, uint64_t flags = 0, uint64_t timeout = -1);

ssize_t iouring_recvmsg(int fd, msghdr* msg, uint64_t flags = 0, uint64_t timeout = -1);

int iouring_connect(int fd, const sockaddr* addr, socklen_t addrlen, uint64_t timeout = -1);

int iouring_accept(int fd, sockaddr* addr, socklen_t* addrlen, uint64_t timeout = -1);

int iouring_fsync(int fd);

int iouring_fdatasync(int fd);

int iouring_open(const char* path, int flags, mode_t mode);

int iouring_mkdir(const char* path, mode_t mode);

int iouring_close(int fd);

bool iouring_register_files_enabled();

int iouring_register_files(int fd);

int iouring_unregister_files(int fd);

struct iouring
{
    static ssize_t pread(int fd, void *buf, size_t count, off_t offset, uint64_t timeout = -1)
    {
        return iouring_pread(fd, buf, count, offset, 0, timeout);
    }
    static ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset, uint64_t timeout = -1)
    {
        return iouring_preadv(fd, iov, iovcnt, offset, 0, timeout);
    }
    static ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset, uint64_t timeout = -1)
    {
        return iouring_pwrite(fd, buf, count, offset, 0, timeout);
    }
    static ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset, uint64_t timeout = -1)
    {
        return iouring_pwritev(fd, iov, iovcnt, offset, 0, timeout);
    }
    static int fsync(int fd, uint64_t timeout = -1)
    {
        return iouring_fsync(fd);
    }
    static int fdatasync(int fd, uint64_t timeout = -1)
    {
        return iouring_fdatasync(fd);
    }
    static int close(int fd)
    {
        return iouring_close(fd);
    }
};

}
