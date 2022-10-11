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

#include "basic_socket.h"

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#ifdef __linux__
#include <linux/errqueue.h>
#include <sys/sendfile.h>
#endif
#include <sys/uio.h>
#include <photon/io/fd-events.h>
#include <photon/net/socket.h>
#include <photon/thread/thread.h>
#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/common/utility.h>

#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY    0x4000000
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef MSG_ERRQUEUE
#define MSG_ERRQUEUE 0
#endif

#ifndef SO_EE_ORIGIN_ZEROCOPY
#define SO_EE_ORIGIN_ZEROCOPY		5
#endif

#ifndef SO_EE_CODE_ZEROCOPY_COPIED
#define SO_EE_CODE_ZEROCOPY_COPIED	1
#endif

namespace photon {
namespace net {
int set_fd_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return (flags < 0) ? flags : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
int set_socket_nonblocking(int fd) {
#ifdef __APPLE__
    return set_fd_nonblocking(fd);
#else
    int flags = 1;
    return ioctl(fd, FIONBIO, &flags);
#endif
}
int socket(int domain, int type, int protocol) {
#ifdef __APPLE__
    auto fd = ::socket(domain, type, protocol);
    set_fd_nonblocking(fd);
    int val = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&val, sizeof(val));
    return fd;
#else
    return ::socket(domain, type | SOCK_NONBLOCK, protocol);
#endif
}
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen,
            uint64_t timeout) {
    int err = 0;
    while (true) {
        int ret = ::connect(fd, addr, addrlen);
        if (ret < 0) {
            auto e = errno;  // errno is usually a macro that expands to a
                             // function call
            if (e == EINTR) {
                err = 1;
                continue;
            }
            if (e == EINPROGRESS || (e == EADDRINUSE && err == 1)) {
                ret = photon::wait_for_fd_writable(fd, timeout);
                if (ret < 0) return -1;
                socklen_t n = sizeof(err);
                ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n);
                if (ret < 0) return -1;
                if (err) {
                    errno = err;
                    return -1;
                }
                return 0;
            }
        }
        return ret;
    }
}
template <typename IOCB, typename WAITCB>
static __FORCE_INLINE__ ssize_t doio(IOCB iocb, WAITCB waitcb) {
    while (true) {
        ssize_t ret = iocb();
        if (ret < 0) {
            auto e = errno;  // errno is usually a macro that expands to a
                             // function call
            if (e == EINTR) continue;
            if (e == EAGAIN || e == EWOULDBLOCK) {
                if (waitcb())  // non-zero result means timeout or interrupt,
                               // need to return
                    return ret;
                continue;
            }
        }
        return ret;
    }
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen,
           uint64_t timeout) {
#ifdef __APPLE__
    auto ret = (int)doio(LAMBDA(::accept(fd, addr, addrlen)),
                  LAMBDA_TIMEOUT(photon::wait_for_fd_readable(fd, timeout)));
    if (ret > 0) {
        set_fd_nonblocking(ret);
        int val = 1;
        ::setsockopt(ret, SOL_SOCKET, SO_NOSIGPIPE, (void*)&val, sizeof(val));
    }
    return ret;
#else
    return (int)doio(LAMBDA(::accept4(fd, addr, addrlen, SOCK_NONBLOCK)),
                  LAMBDA_TIMEOUT(photon::wait_for_fd_readable(fd, timeout)));
#endif
}
ssize_t read(int fd, void *buf, size_t count, uint64_t timeout) {
    return doio(LAMBDA(::read(fd, buf, count)),
                LAMBDA_TIMEOUT(photon::wait_for_fd_readable(fd, timeout)));
}
ssize_t readv(int fd, const struct iovec *iov, int iovcnt, uint64_t timeout) {
    if (iovcnt <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (iovcnt == 1) return read(fd, iov->iov_base, iov->iov_len, timeout);
    return doio(LAMBDA(::readv(fd, iov, iovcnt)),
                LAMBDA_TIMEOUT(photon::wait_for_fd_readable(fd, timeout)));
}
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count,
                 uint64_t timeout) {
#ifdef __APPLE__
    off_t len = count;
    ssize_t ret =
        doio(LAMBDA(::sendfile(out_fd, in_fd, *offset, &len, nullptr, 0)),
             LAMBDA_TIMEOUT(wait_for_fd_writable(out_fd, timeout)));
    return (ret == 0) ? len : (int)ret;
#else
    return doio(LAMBDA(::sendfile(out_fd, in_fd, offset, count)),
                LAMBDA_TIMEOUT(photon::wait_for_fd_writable(out_fd, timeout)));
#endif
}

ssize_t sendmsg_zerocopy(int fd, iovec* iov, int iovcnt, uint32_t& num_calls, uint64_t timeout) {
    msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = iovcnt;
    ssize_t ret = doio(LAMBDA(::sendmsg(fd, &msg, MSG_ZEROCOPY)),
                       LAMBDA_TIMEOUT(photon::wait_for_fd_writable(fd, timeout)));
    num_calls++;
    return ret;
}

ssize_t read_n(int fd, void *buf, size_t count, uint64_t timeout) {
    return doio_n(buf, count, LAMBDA_TIMEOUT(read(fd, buf, count, timeout)));
}
ssize_t sendfile_n(int out_fd, int in_fd, off_t *offset, size_t count,
                   uint64_t timeout) {
    void* buf_unused = nullptr;
    return doio_n(buf_unused, count,
        LAMBDA_TIMEOUT(sendfile(out_fd, in_fd, offset, count, timeout)));
}

ssize_t readv_n(int fd, struct iovec *iov, int iovcnt, uint64_t timeout) {
    iovector_view v(iov, iovcnt);
    return doiov_n(v, LAMBDA_TIMEOUT(readv(fd, v.iov, v.iovcnt, timeout)));
}

ssize_t zerocopy_n(int fd, iovec* iov, int iovcnt, uint32_t& num_calls, uint64_t timeout) {
    iovector_view v(iov, iovcnt);
    return doiov_n(v, LAMBDA_TIMEOUT(sendmsg_zerocopy(fd, v.iov, v.iovcnt, num_calls, timeout)));
}

ssize_t send(int fd, const void *buf, size_t count, int flag, uint64_t timeout) {
    return doio(LAMBDA(::send(fd, buf, count, flag | MSG_NOSIGNAL)),
                    LAMBDA_TIMEOUT(photon::wait_for_fd_writable(fd, timeout)));
}

ssize_t sendv(int fd, const struct iovec *iov, int iovcnt, int flag, uint64_t timeout) {
    msghdr msg = {};
    msg.msg_iov = (struct iovec*)iov;
    msg.msg_iovlen = iovcnt;
    return doio(LAMBDA(::sendmsg(fd, &msg, flag | MSG_NOSIGNAL)),
                       LAMBDA_TIMEOUT(photon::wait_for_fd_writable(fd, timeout)));
}
ssize_t send_n(int fd, const void *buf, size_t count, int flag, uint64_t timeout) {
    return doio_n((void *&)buf, count,
                LAMBDA_TIMEOUT(send(fd, (const void*)buf, (size_t)count, flag, timeout)));
}

ssize_t sendv_n(int fd, struct iovec *iov, int iovcnt, int flag, uint64_t timeout) {
    iovector_view v(iov, iovcnt);
    return doiov_n(v, LAMBDA_TIMEOUT(sendv(fd, (struct iovec*)v.iov, (int)v.iovcnt, flag, timeout)));
}
ssize_t write(int fd, const void *buf, size_t count, uint64_t timeout) {
    return send(fd, buf, count, 0, timeout);
}
ssize_t writev(int fd, const struct iovec *iov, int iovcnt, uint64_t timeout) {
    return sendv(fd, iov, iovcnt, 0, timeout);
}
ssize_t write_n(int fd, const void *buf, size_t count, uint64_t timeout) {
    return send_n(fd, buf, count, 0, timeout);
}
ssize_t writev_n(int fd, struct iovec *iov, int iovcnt, uint64_t timeout) {
    return sendv_n(fd, iov, iovcnt, 0, timeout);
}

bool ISocketStream::skip_read(size_t count) {
    if (!count) return true;
    while(count) {
        static char buf[1024];
        size_t len = count < sizeof(buf) ? count : sizeof(buf);
        ssize_t ret = read(buf, len);
        if (ret < (ssize_t)len) return false;
        count -= len;
    }
    return true;
}

#ifdef __linux__
static ssize_t recv_errqueue(int fd, uint32_t &ret_counter) {
    char control[128];
    msghdr msg = {};
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    auto ret = recvmsg(fd, &msg, MSG_ERRQUEUE);
    if (ret < 0) return ret;
    cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    if (cm == nullptr) {
        LOG_ERROR_RETURN(0, -1, "Fail to get control message header");
    }
    auto serr = (sock_extended_err*) CMSG_DATA(cm);
    if (serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
        LOG_ERROR_RETURN(0, -1, "wrong origin");
    }
    if (serr->ee_errno != 0) {
        LOG_ERROR_RETURN(0, -1, "wrong error code ", serr->ee_errno);
    }
    if ((serr->ee_code & SO_EE_CODE_ZEROCOPY_COPIED)) {
        LOG_DEBUG("deferred copy occurs, might harm performance");
    }

    ret_counter = serr->ee_data;
    return 0;
}

static int64_t read_counter(int fd, uint64_t timeout) {
    uint32_t counter = 0;
    auto ret = doio(LAMBDA(recv_errqueue(fd, counter)),
                LAMBDA_TIMEOUT(photon::wait_for_fd_error(fd, timeout)));
    if (ret < 0) return ret;
    return counter;
}

inline bool is_counter_less_than(uint32_t left, uint32_t right) {
    const uint32_t mid = UINT32_MAX / 2;
    return (left < right) || (left > right && left > mid && right - left < mid);
}

ssize_t zerocopy_confirm(int fd, uint32_t num_calls, uint64_t timeout) {
    auto func = LAMBDA_TIMEOUT(read_counter(fd, timeout));
    uint32_t counter = 0;
    do {
        auto ret = func();
        if (ret < 0) return ret;
        counter = ret;
    } while (is_counter_less_than(counter, num_calls));
    return 0;
}
#endif
}  // namespace net
}
