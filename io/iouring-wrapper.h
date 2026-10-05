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
#include <photon/common/timeout.h>

namespace photon {

class CascadingEventEngine;

static const uint64_t IouringFixedFileFlag = 1ULL<< 32;

// Every entry point below that takes a `ce` needs an io_uring ring: the one `ce`
// names, or else the current vcpu's master event engine. io_uring is that master
// engine only if photon::init() was asked for INIT_EVENT_IOURING *and* got it --
// init() walks a recommended order and keeps the first engine that initializes, so
// the mask a caller passed does not say what is installed. Each one therefore
// identifies its engine before using it, and answers -1 with errno ENOSYS when it is
// not an io_uring one, instead of reading another engine's object as a ring.

ssize_t iouring_splice(int fd_in, int64_t off_in, int fd_out, int64_t off_out,
                       unsigned int nbytes, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_pread(int fd, void* buf, size_t count, off_t offset, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_pwrite(int fd, const void* buf, size_t count, off_t offset, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_preadv(int fd, const iovec* iov, int iovcnt, off_t offset, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_pwritev(int fd, const iovec* iov, int iovcnt, off_t offset, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_send(int fd, const void* buf, size_t len, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_send_zc(int fd, const void* buf, size_t len, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_sendmsg(int fd, const msghdr* msg, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_sendmsg_zc(int fd, const msghdr* msg, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_recv(int fd, void* buf, size_t len, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

ssize_t iouring_recvmsg(int fd, msghdr* msg, uint64_t flags = 0, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

int iouring_connect(int fd, const sockaddr* addr, socklen_t addrlen, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

int iouring_accept(int fd, sockaddr* addr, socklen_t* addrlen, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

int iouring_fsync(int fd, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

int iouring_fdatasync(int fd, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

int iouring_open(const char* path, int flags, mode_t mode, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

int iouring_mkdir(const char* path, mode_t mode, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

int iouring_close(int fd, Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

/**
 * Issue IORING_OP_URING_CMD on `fd` and wait for its completion. The fd's
 * kernel handler defines the payload layout and the result's meaning: the
 * cqe's res is returned when >= 0; a negative res yields -1 with errno = -res.
 * `cmd` is copied into the sqe's cmd area -- at most 16 bytes, unless the ring
 * was created with iouring_args::setup_sqe128 (ublk's 32-byte control command
 * requires it). `ce` selects the ring: a standalone cascading engine, or the
 * current vcpu's master engine when null. A cascading engine needs a pump
 * coroutine calling wait_for_events() (which drives submission and reaping),
 * and the pump together with all submitters must run on the same vcpu -- the
 * submission queue is not thread-safe.
 */
int32_t iouring_uring_cmd(int fd, uint32_t cmd_op, const void* cmd, size_t cmd_len,
                          Timeout timeout = {}, CascadingEventEngine* ce = nullptr);

/**
 * Abandon every operation parked in async_io on `ce`, which must be a
 * standalone iouring cascading engine: a parked caller interrupted
 * (photon::thread_interrupt) afterwards returns -1 immediately instead of
 * waiting for its completion -- the teardown path for commands the kernel
 * cannot cancel on request (e.g. a parked ublk FETCH_REQ). Destroy the engine
 * after all abandoned callers have been interrupted and joined; their late
 * completions die unreaped with the ring.
 */
void iouring_abandon(CascadingEventEngine* ce);

bool iouring_register_files_enabled();

int iouring_register_files(int fd, CascadingEventEngine* ce = nullptr);

int iouring_unregister_files(int fd, CascadingEventEngine* ce = nullptr);

struct iouring
{
    static ssize_t pread(int fd, void *buf, size_t count, off_t offset, Timeout timeout = {}, CascadingEventEngine* ce = nullptr)
    {
        return iouring_pread(fd, buf, count, offset, 0, timeout, ce);
    }
    static ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset, Timeout timeout = {}, CascadingEventEngine* ce = nullptr)
    {
        return iouring_preadv(fd, iov, iovcnt, offset, 0, timeout, ce);
    }
    static ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset, Timeout timeout = {}, CascadingEventEngine* ce = nullptr)
    {
        return iouring_pwrite(fd, buf, count, offset, 0, timeout, ce);
    }
    static ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset, Timeout timeout = {}, CascadingEventEngine* ce = nullptr)
    {
        return iouring_pwritev(fd, iov, iovcnt, offset, 0, timeout, ce);
    }
    static int fsync(int fd, Timeout timeout = {}, CascadingEventEngine* ce = nullptr)
    {
        return iouring_fsync(fd, timeout, ce);
    }
    static int fdatasync(int fd, Timeout timeout = {}, CascadingEventEngine* ce = nullptr)
    {
        return iouring_fdatasync(fd, timeout, ce);
    }
    static int close(int fd, Timeout timeout = {}, CascadingEventEngine* ce = nullptr)
    {
        return iouring_close(fd, timeout, ce);
    }
};

}
