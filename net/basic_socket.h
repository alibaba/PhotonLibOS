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
#include <cinttypes>
#include <sys/socket.h>
#include <sys/un.h>
#include <photon/common/iovector.h>
#include <photon/thread/thread.h>
#include <photon/common/timeout.h>
#include <photon/common/utility.h>

namespace photon {
namespace net {
int socket(int domain, int type, int protocol);

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen,
            Timeout timeout = {});

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen,
           Timeout timeout = {});

ssize_t send(int fd, const void* buf, size_t len, int flags, Timeout timeout = {});
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags, Timeout timeout = {});
ssize_t recv(int fd, void* buf, size_t count, int flags, Timeout timeout = {});
ssize_t recvmsg(int fd, struct msghdr* msg, int flags, Timeout timeout = {});

ssize_t read(int fd, void *buf, size_t count, Timeout timeout = {});

ssize_t readv(int fd, const struct iovec *iov, int iovcnt,
              Timeout timeout = {});

ssize_t write(int fd, const void *buf, size_t count, Timeout timeout = {});

ssize_t writev(int fd, const struct iovec *iov, int iovcnt,
               Timeout timeout = {});

ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count,
                 Timeout timeout = {});

ssize_t read_n(int fd, void *buf, size_t count, Timeout timeout = {});

ssize_t write_n(int fd, const void *buf, size_t count, Timeout timeout = {});

ssize_t readv_n(int fd, struct iovec *iov, int iovcnt, Timeout timeout = {});

ssize_t writev_n(int fd, struct iovec *iov, int iovcnt, Timeout timeout = {});

ssize_t sendfile_n(int out_fd, int in_fd, off_t *offset, size_t count,
                   Timeout timeout = {});

class ISocketStream;
ssize_t sendfile_n(ISocketStream* out_stream, int in_fd, off_t offset, size_t count, Timeout timeout = {});

[[deprecated("use sendfile_n() instead")]] inline
ssize_t sendfile_fallback(ISocketStream* out_stream, int in_fd, off_t offset, size_t count, Timeout timeout = {}) {
    return sendfile_n(out_stream, in_fd, offset, count, timeout);
}

int zerocopy_confirm(int fd, uint32_t num_calls, Timeout timeout = {});

ssize_t sendv(int fd, const struct iovec *iov, int iovcnt, int flag, Timeout timeout = {});

ssize_t send_n(int fd, const void *buf, size_t count, int flag, Timeout timeout = {});

ssize_t sendv_n(int fd, struct iovec *iov, int iovcnt, int flag, Timeout timeout = {});

// POSIX standard
int set_fd_nonblocking(int fd);

int set_socket_nonblocking(int fd);

#define LAMBDA(expr) [&]() __INLINE__ { return expr; }
#define LAMBDA_TIMEOUT(expr)  LAMBDA(expr)

template <typename IOCB, typename WAIT> __FORCE_INLINE__
int doio_once(IOCB iocb, WAIT waitcb) {
    while (true) {
        ssize_t ret = iocb();
        if (ret < 0) {
            auto e = errno;  // errno is usually a macro that expands to a
                             // function call
            if (e == EINTR) continue;
            if (e == EAGAIN || e == EWOULDBLOCK) {
                if (waitcb())  // non-zero result means timeout or
                               // interrupt, need to return
                    return ret;
                continue;
            }
        }
        return ret;
    }
}

#define DOIO_ONCE(iocb, waitcb) doio_once(LAMBDA(iocb), LAMBDA(waitcb))

template <typename IOCB, typename STEP> __FORCE_INLINE__
ssize_t doio_loop(IOCB iocb, STEP step) {
    ssize_t ret, n = 0;
    do {
        ret = iocb();
        if (ret <  0) return ret; // error
        if (ret == 0) break;  // EOF
        n += ret;
    } while (step(ret, n));
    return n;
}

struct BufStep {
    void*& buf;
    size_t& count;
    void* _dummy;
    BufStep(void*& buf, size_t& count) : buf(buf), count(count) { }
    BufStep(size_t& count) : buf(_dummy), count(count) { }
    bool operator()(size_t ret, size_t n) __INLINE__ {
        assert(ret <= count);
        (char*&)buf += ret;
        count -= ret;
        return count > 0;
    }
};

struct BufStepV {
    iovector_view& v;
    BufStepV(iovector_view& v) : v(v) { }
    bool operator()(size_t ret, size_t n) __INLINE__ {
        auto extracted = v.extract_front(ret);
        assert(extracted == ret);
        _unused(extracted);
        return v.iovcnt > 0;
    }
};

#define DOIO_LOOP(iocb, step)        doio_loop(LAMBDA(iocb), step)
#define DOIO_LOOP_LAMBDA(iocb, step) doio_loop(LAMBDA(iocb), \
    [&](size_t ret, size_t n) __INLINE__ { step; })

int fill_uds_path(struct sockaddr_un& name, const char* path, size_t count);

struct EndPoint;

using Getter = int (*)(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int do_get_name(int fd, Getter getter, char* path, size_t count);
int do_get_name(int fd, Getter getter, EndPoint& addr);

inline int get_socket_name(int fd, EndPoint& addr) {
    return do_get_name(fd, &::getsockname, addr);
}

inline int get_peer_name(int fd, EndPoint& addr) {
    return do_get_name(fd, &::getpeername, addr);
}

inline int get_socket_name(int fd, char* path, size_t count) {
    return do_get_name(fd, &::getsockname, path, count);
}

inline int get_peer_name(int fd, char* path, size_t count) {
    return do_get_name(fd, &::getpeername, path, count);
}


}  // namespace net
}
