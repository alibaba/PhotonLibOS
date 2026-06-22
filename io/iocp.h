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

#include <photon/common/timeout.h>
#include <photon/io/fd-events.h>
#include <sys/uio.h>

namespace photon {

// ---- Event engine factory ----

MasterEventEngine* new_iocp_master_engine();
CascadingEventEngine* new_iocp_cascading_engine();

// ---- Async I/O wrappers (similar to iouring-wrapper) ----
// Each function accepts an optional CascadingEventEngine*.  When nullptr
// (the default), the master engine of the current vCPU is used.

// File I/O

// Open a file with FILE_FLAG_OVERLAPPED so the returned CRT fd can be used
// with iocp_pread / iocp_pwrite / iocp_sendfile. Translates POSIX O_* flags
// to Win32 access/creation/disposition. Returns CRT fd (>=0) or -1 with errno set.
int iocp_open(const char* pathname, int flags, mode_t mode = 0);

inline int iocp_creat(const char* pathname, mode_t mode) {
    return iocp_open(pathname, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

ssize_t iocp_pread(int fd, void* buf, size_t count, off_t offset,
                   Timeout timeout = {},
                   CascadingEventEngine* engine = nullptr);
ssize_t iocp_pwrite(int fd, const void* buf, size_t count, off_t offset,
                    Timeout timeout = {},
                    CascadingEventEngine* engine = nullptr);
ssize_t iocp_preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset,
                    Timeout timeout = {},
                    CascadingEventEngine* engine = nullptr);
ssize_t iocp_pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset,
                     Timeout timeout = {},
                     CascadingEventEngine* engine = nullptr);
int iocp_fsync(int fd, Timeout timeout = {},
               CascadingEventEngine* engine = nullptr);
int iocp_fdatasync(int fd, Timeout timeout = {},
                   CascadingEventEngine* engine = nullptr);
int iocp_close(int fd, Timeout timeout = {},
               CascadingEventEngine* engine = nullptr);

// Socket I/O (overlapped)
ssize_t iocp_send(int fd, const void* buf, size_t count, int flags,
                  Timeout timeout = {},
                  CascadingEventEngine* engine = nullptr);
ssize_t iocp_recv(int fd, void* buf, size_t count, int flags,
                  Timeout timeout = {},
                  CascadingEventEngine* engine = nullptr);
ssize_t iocp_sendmsg(int fd, const struct msghdr* msg, int flags,
                     Timeout timeout = {},
                     CascadingEventEngine* engine = nullptr);
ssize_t iocp_recvmsg(int fd, struct msghdr* msg, int flags,
                     Timeout timeout = {},
                     CascadingEventEngine* engine = nullptr);
ssize_t iocp_sendfile(int out_fd, int in_fd, off_t* offset, size_t count,
                      Timeout timeout = {},
                      CascadingEventEngine* engine = nullptr);

// ---- Convenience struct (same pattern as struct iouring) ----
// Provides standard-named static methods with sensible defaults.

struct iocp {
    static ssize_t pread(int fd, void* buf, size_t count, off_t offset,
                         Timeout timeout = {},
                         CascadingEventEngine* ce = nullptr) {
        return iocp_pread(fd, buf, count, offset, timeout, ce);
    }
    static ssize_t preadv(int fd, const struct iovec* iov, int iovcnt,
                          off_t offset, Timeout timeout = {},
                          CascadingEventEngine* ce = nullptr) {
        return iocp_preadv(fd, iov, iovcnt, offset, timeout, ce);
    }
    static ssize_t pwrite(int fd, const void* buf, size_t count,
                          off_t offset, Timeout timeout = {},
                          CascadingEventEngine* ce = nullptr) {
        return iocp_pwrite(fd, buf, count, offset, timeout, ce);
    }
    static ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt,
                           off_t offset, Timeout timeout = {},
                           CascadingEventEngine* ce = nullptr) {
        return iocp_pwritev(fd, iov, iovcnt, offset, timeout, ce);
    }
    static int fsync(int fd, Timeout timeout = {},
                     CascadingEventEngine* ce = nullptr) {
        return iocp_fsync(fd, timeout, ce);
    }
    static int fdatasync(int fd, Timeout timeout = {},
                         CascadingEventEngine* ce = nullptr) {
        return iocp_fdatasync(fd, timeout, ce);
    }
    static int close(int fd, Timeout timeout = {},
                     CascadingEventEngine* ce = nullptr) {
        return iocp_close(fd, timeout, ce);
    }
    static ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count,
                            Timeout timeout = {},
                            CascadingEventEngine* ce = nullptr) {
        return iocp_sendfile(out_fd, in_fd, offset, count, timeout, ce);
    }
};

} // namespace photon
