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
#include "base_socket.h"

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
// POSIX standard
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
            Timeout timeout) {
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
                if (unlikely(ret < 0)) return -1;
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

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen,
           Timeout timeout) {
#ifdef __APPLE__
    auto ret = DOIO_ONCE(::accept(fd, addr, addrlen),
                    wait_for_fd_readable(fd, timeout));
    if (ret > 0) {
        set_fd_nonblocking(ret);
        int val = 1;
        ::setsockopt(ret, SOL_SOCKET, SO_NOSIGPIPE, (void*)&val, sizeof(val));
    }
    return ret;
#else
    return DOIO_ONCE(::accept4(fd, addr, addrlen, SOCK_NONBLOCK),
                 wait_for_fd_readable(fd, timeout));
#endif
}

ssize_t read(int fd, void *buf, size_t count, Timeout timeout) {
    return DOIO_ONCE(::read(fd, buf, count), wait_for_fd_readable(fd, timeout));
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt, Timeout timeout) {
    if (unlikely(iovcnt <= 0)) {
        errno = EINVAL;
        return -1;
    }
    return (iovcnt == 1) ? read(fd, iov->iov_base, iov->iov_len, timeout) :
        DOIO_ONCE(::readv(fd, iov, iovcnt), wait_for_fd_readable(fd, timeout));
}

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count,
                 Timeout timeout) {
#ifdef __APPLE__
    off_t len = count;
    ssize_t ret = DOIO_ONCE(::sendfile(out_fd, in_fd, *offset, &len, nullptr, 0),
                  wait_for_fd_writable(out_fd, timeout));
    return (ret == 0) ? len : (int)ret;
#else
    return DOIO_ONCE(::sendfile(out_fd, in_fd, offset, count),
           wait_for_fd_writable(out_fd, timeout));
#endif
}

ssize_t read_n(int fd, void *buf, size_t count, Timeout timeout) {
    return DOIO_LOOP(read(fd, buf, count, timeout), BufStep(buf, count));
}

ssize_t sendfile_n(int out_fd, int in_fd, off_t *offset, size_t count,
                   Timeout timeout) {
    return DOIO_LOOP(sendfile(out_fd, in_fd, offset, count, timeout), BufStep(count));
}

ssize_t readv_n(int fd, struct iovec *iov, int iovcnt, Timeout timeout) {
    iovector_view v(iov, iovcnt);
    return DOIO_LOOP(readv(fd, v.iov, v.iovcnt, timeout), BufStepV(v));
}

ssize_t send(int fd, const void *buf, size_t count, int flags, Timeout timeout) {
    return DOIO_ONCE(::send(fd, buf, count, flags), wait_for_fd_writable(fd, timeout));
}

ssize_t sendmsg(int fd, const struct msghdr* msg, int flags, Timeout timeout) {
    return DOIO_ONCE(::sendmsg(fd, msg, flags), wait_for_fd_writable(fd, timeout));
}

ssize_t recv(int fd, void* buf, size_t count, int flags, Timeout timeout) {
    return DOIO_ONCE(::recv(fd, buf, count, flags), wait_for_fd_readable(fd, timeout));
}

ssize_t recvmsg(int fd, struct msghdr* msg, int flags, Timeout timeout) {
    return DOIO_ONCE(::recvmsg(fd, msg, flags), wait_for_fd_readable(fd, timeout));
}

ssize_t sendv(int fd, const struct iovec *iov, int iovcnt, int flag, Timeout timeout) {
    msghdr msg = {};
    msg.msg_iov = (struct iovec*)iov;
    msg.msg_iovlen = iovcnt;
    return DOIO_ONCE(::sendmsg(fd, &msg, flag | MSG_NOSIGNAL),
          wait_for_fd_writable(fd, timeout));
}

ssize_t send_n(int fd, const void *buf, size_t count, int flag, Timeout timeout) {
    return DOIO_LOOP(send(fd, buf, count, flag, timeout), BufStep((void*&)buf, count));
}

ssize_t sendv_n(int fd, struct iovec *iov, int iovcnt, int flag, Timeout timeout) {
    iovector_view v(iov, iovcnt);
    return DOIO_LOOP(sendv(fd, v.iov, v.iovcnt, flag, timeout), BufStepV(v));
}

ssize_t write(int fd, const void *buf, size_t count, Timeout timeout) {
    return send(fd, buf, count, MSG_NOSIGNAL, timeout);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt, Timeout timeout) {
    return sendv(fd, iov, iovcnt, 0, timeout);
}

ssize_t write_n(int fd, const void *buf, size_t count, Timeout timeout) {
    return send_n(fd, buf, count, 0, timeout);
}

ssize_t writev_n(int fd, struct iovec *iov, int iovcnt, Timeout timeout) {
    return sendv_n(fd, iov, iovcnt, 0, timeout);
}

ssize_t sendfile_n(ISocketStream* out_stream,
            int in_fd, off_t offset, size_t count, Timeout timeout) {
    char buf[64 * 1024];
    auto func = [&]() -> ssize_t {
        size_t s = sizeof(buf);
        if (s > count) s = count;
        ssize_t n_read = ::pread(in_fd, buf, s, offset);
        if (n_read != (ssize_t) s)
            LOG_ERRNO_RETURN(0, (ssize_t)-1, "failed to read fd ", in_fd);
        offset += n_read;
        ssize_t n_write = out_stream->write(buf, s);
        if (n_write != (ssize_t) s)
            LOG_ERRNO_RETURN(0, (ssize_t)-1, "failed to write to stream ", out_stream);
        return n_write;
    };
    return doio_loop(func, BufStep(count));
}

bool ISocketStream::skip_read(size_t count) {
    static char buf[1024];
    return DOIO_LOOP(read(buf, std::min(count, sizeof(buf))), BufStep(count));
}

ssize_t ISocketStream::recv_at_least(void* buf, size_t count, size_t least, int flags) {
    return DOIO_LOOP_LAMBDA(recv(buf, count, flags), {
        (char*&)buf += ret;
        count -= ret;
        return n < least;
    });
}

ssize_t ISocketStream::recv_at_least_mutable(struct iovec *iov, int iovcnt,
                                             size_t least, int flags /*=0*/) {
    iovector_view v(iov, iovcnt);
    return DOIO_LOOP_LAMBDA(recv(v.iov, v.iovcnt, flags), {
        auto r = v.extract_front(ret);
        assert(r == ret); (void)r;
        return n < least;
    });
}

int do_get_name(int fd, Getter getter, EndPoint& addr) {
    sockaddr_storage storage;
    socklen_t len = storage.get_max_socklen();
    int ret = getter(fd, storage.get_sockaddr(), &len);
    if (ret < 0 || len > storage.get_max_socklen()) return -1;
    addr = storage.to_endpoint();
    return 0;
}

int do_get_name(int fd, Getter getter, char* path, size_t count) {
    struct sockaddr_un addr_un;
    socklen_t len = sizeof(addr_un);
    int ret = getter(fd, (struct sockaddr*) &addr_un, &len);
    // if len larger than size of addr_un, or less than prefix in addr_un
    if (ret < 0 || len > sizeof(addr_un) || len <= sizeof(addr_un.sun_family))
        return -1;
    strncpy(path, addr_un.sun_path, count);
    return 0;
}

int fill_uds_path(struct sockaddr_un& name, const char* path, size_t count) {
    const int LEN = sizeof(name.sun_path) - 1;
    if (count == 0) count = strlen(path);
    if (count > LEN)
        LOG_ERROR_RETURN(ENAMETOOLONG, -1, "pathname is too long (`>`)", count, LEN);

    memset(&name, 0, sizeof(name));
    memcpy(name.sun_path, path, count + 1);
#ifndef __linux__
    name.sun_len = 0;
#endif
    name.sun_family = AF_UNIX;
    return 0;
}

#ifdef __linux__
static int recv_errqueue(int fd, uint32_t &ret_counter) {
    char control[128];
    msghdr msg = {};
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    auto ret = ::recvmsg(fd, &msg, MSG_ERRQUEUE);
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

inline bool is_counter_less_than(uint32_t left, uint32_t right) {
    const uint32_t mid = UINT32_MAX / 2;
    return (left < right) || (left > right && left > mid && right - left < mid);
}

int zerocopy_confirm(int fd, uint32_t num_calls, Timeout timeout) {
    uint32_t counter = 0;
    do {
        auto ret = DOIO_ONCE(recv_errqueue(fd, counter),
                         wait_for_fd_error(fd, timeout));
        if (ret < 0) return ret;
    } while (is_counter_less_than(counter, num_calls));
    return 0;
}
#endif // __linux__

}  // namespace net
}  // namespace photon
