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
#include <unistd.h>

// aio wrapper depends on fd-events ( fd_events_epoll_init() )
namespace photon
{
    extern "C"
    {
        int libaio_wrapper_init(int iodepth = 32);
        int libaio_wrapper_fini();

        // `fd` must be opened with O_DIRECT, and the buffers must be aligned
        ssize_t libaio_pread(int fd, void *buf, size_t count, off_t offset);
        ssize_t libaio_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
        ssize_t libaio_pwrite(int fd, const void *buf, size_t count, off_t offset);
        ssize_t libaio_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);
        static int libaio_fsync(int fd) { return 0; }
        
        ssize_t posixaio_pread(int fd, void *buf, size_t count, off_t offset);
        ssize_t posixaio_pwrite(int fd, const void *buf, size_t count, off_t offset);
        int posixaio_fsync(int fd);
        int posixaio_fdatasync(int fd);
    }
    
    struct libaio
    {
        static ssize_t pread(int fd, void *buf, size_t count, off_t offset)
        {
            return libaio_pread(fd, buf, count, offset);
        }
        static ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
        {
            return libaio_preadv(fd, iov, iovcnt, offset);
        }
        static ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
        {
            return libaio_pwrite(fd, buf, count, offset);
        }
        static ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
        {
            return libaio_pwritev(fd, iov, iovcnt, offset);
        }
        static int fsync(int fd)
        {
            return libaio_fsync(fd);
        }
        static int fdatasync(int fd)
        {
            return libaio_fsync(fd);
        }
        static int close(int fd)
        {
            return ::close(fd);
        }
    };
    
    struct posixaio
    {
        static ssize_t pread(int fd, void *buf, size_t count, off_t offset)
        {
            return posixaio_pread(fd, buf, count, offset);
        }
        static ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
        static ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
        {
            return posixaio_pwrite(fd, buf, count, offset);
        }
        static ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);
        static int fsync(int fd)
        {
            return posixaio_fsync(fd);
        }
        static int fdatasync(int fd)
        {
            return posixaio_fdatasync(fd);
        }
        static int close(int fd)
        {
            return ::close(fd);
        }
    };
}

