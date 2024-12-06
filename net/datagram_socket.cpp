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

#include "datagram_socket.h"

#include <sys/fcntl.h>
#include <sys/un.h>
#include <unistd.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/net/basic_socket.h>
#include <photon/net/socket.h>
#include "base_socket.h"

namespace photon {
namespace net {

constexpr static size_t MAX_UDP_MESSAGE_SIZE = 65507UL;
constexpr static size_t MAX_UDS_MESSAGE_SIZE = 207UL * 1024;

class DatagramSocketBase : public IDatagramSocket {
protected:
    uint64_t m_timeout = -1;
    int fd;
    const size_t m_max_msg_size;

    int set_fd_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        return (flags < 0) ? flags : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

public:
    DatagramSocketBase(int AF, size_t maxsize)
        : fd(AF), m_max_msg_size(maxsize) {}

    int init(int nfd) {
        auto AF = fd;
#ifndef __APPLE__
        constexpr static auto FLAG = SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
#else
        constexpr static auto FLAG = SOCK_DGRAM;
#endif
        fd = (nfd >= 0) ? nfd : (::socket(AF, FLAG, 0));
#ifdef __APPLE__
        set_fd_nonblocking(fd);
#endif
        return fd < 0;
    }

    ~DatagramSocketBase() override {
        if (fd != -1) ::close(fd);
    }

    virtual uint64_t flags() override {
        return 0;  // not reliable and not preserve orders
    }

    virtual uint64_t max_message_size() override { return m_max_msg_size; }

    int do_connect(struct sockaddr* addr, size_t addr_len) {
        return doio([&] { return ::connect(fd, addr, addr_len); },
                    [&] { return photon::wait_for_fd_writable(fd); });
    }

    int do_bind(struct sockaddr* addr, size_t addr_len) {
        return ::bind(fd, addr, addr_len);
    }
    ssize_t do_send(const iovec* iov, int iovcnt, sockaddr* addr,
                    size_t addrlen, int flags = 0) {
        flags |= MSG_NOSIGNAL;
        struct msghdr hdr {
            .msg_name = (void*)addr, .msg_namelen = (socklen_t)addrlen,
            .msg_iov = (iovec*)iov,
            .msg_iovlen = (decltype(msghdr::msg_iovlen))iovcnt,
            .msg_control = nullptr, .msg_controllen = 0, .msg_flags = flags,
        };
        return doio([&] { return ::sendmsg(fd, &hdr, MSG_DONTWAIT | flags); },
                    [&] { return photon::wait_for_fd_writable(fd); });
    }
    ssize_t do_recv(const iovec* iov, int iovcnt, sockaddr* addr,
                    size_t* addrlen, int flags) {
        struct msghdr hdr {
            .msg_name = (void*)addr,
            .msg_namelen = addrlen ? (socklen_t)*addrlen : 0,
            .msg_iov = (iovec*)iov,
            .msg_iovlen = (decltype(msghdr::msg_iovlen))iovcnt,
            .msg_control = nullptr, .msg_controllen = 0, .msg_flags = flags,
        };
        auto ret =
            doio([&] { return ::recvmsg(fd, &hdr, MSG_DONTWAIT | flags); },
                 [&] { return photon::wait_for_fd_readable(fd); });
        if (addrlen) *addrlen = hdr.msg_namelen;
        return ret;
    }
    virtual Object* get_underlay_object(uint64_t recursion) override {
        return (Object*)(uint64_t)fd;
    }
    virtual int setsockopt(int level, int option_name, const void* option_value,
                           socklen_t option_len) override {
        return ::setsockopt(fd, level, option_name, option_value, option_len);
    };
    virtual int getsockopt(int level, int option_name, void* option_value,
                           socklen_t* option_len) override {
        return ::getsockopt(fd, level, option_name, option_value, option_len);
    }
    // get/set timeout, in us, (default +∞)
    virtual uint64_t timeout() const override { return m_timeout; }
    virtual void timeout(uint64_t tm) override { m_timeout = tm; }
    virtual int getsockname(EndPoint& addr) override {
        return get_socket_name(fd, addr);
    }
    virtual int getpeername(EndPoint& addr) override {
        return get_peer_name(fd, addr);
    }
    virtual int getsockname(char* path, size_t count) override {
        return get_socket_name(fd, path, count);
    }
    virtual int getpeername(char* path, size_t count) override {
        return get_peer_name(fd, path, count);
    }
};

class UDP : public DatagramSocketBase {
public:
    using DatagramSocketBase::DatagramSocketBase;
    virtual int connect(const Addr* addr, size_t addr_len) override {
        auto ep = (EndPoint*)addr;
        assert(ep && addr_len == sizeof(*ep));
        sockaddr_storage s(*ep);
        return do_connect(s.get_sockaddr(), s.get_socklen());
    }
    virtual int bind(const Addr* addr, size_t addr_len) override {
        auto ep = (EndPoint*)addr;
        assert(ep && addr_len == sizeof(*ep));
        sockaddr_storage s(*ep);
        return do_bind(s.get_sockaddr(), s.get_socklen());
    }
    virtual ssize_t send(const struct iovec* iov, int iovcnt, const Addr* addr,
                         size_t addr_len, int flags = 0) override {
        auto ep = (EndPoint*)addr;
        if (likely(!ep) || unlikely(addr_len != sizeof(*ep)))
            return do_send(iov, iovcnt, nullptr, 0, flags);
        assert(addr_len == sizeof(*ep));
        sockaddr_storage s(*ep);
        return do_send(iov, iovcnt, s.get_sockaddr(), s.get_socklen(), flags);
    }
    virtual ssize_t recv(const struct iovec* iov, int iovcnt, Addr* addr,
                         size_t* addr_len, int flags) override {
        auto ep = (EndPoint*)addr;
        if (likely(!ep || !addr_len) || unlikely(*addr_len != sizeof(*ep))) {
            return do_recv(iov, iovcnt, nullptr, 0, flags);
        }

        sockaddr_storage s(*ep);
        size_t alen = s.get_socklen();
        auto ret = do_recv(iov, iovcnt, s.get_sockaddr(), &alen, flags);
        if (ret >= 0) {
            *ep = s.to_endpoint();
            *addr_len = alen;
        }
        return ret;
    }
};

// UNIX-domain socket for datagram
class UDS : public DatagramSocketBase {
public:
    using DatagramSocketBase::DatagramSocketBase;
    struct sockaddr_un to_addr_un(const void* addr, size_t addr_len) {
        struct sockaddr_un un;
        fill_uds_path(un, (char*)addr, addr_len);
        return un;
    }
    virtual int connect(const Addr* addr, size_t addr_len) override {
        auto un = to_addr_un(addr, addr_len);
        return do_connect((sockaddr*)&un, sizeof(un));
    }
    virtual int bind(const Addr* addr, size_t addr_len) override {
        auto un = to_addr_un(addr, addr_len);
        return do_bind((sockaddr*)&un, sizeof(un));
    }
    virtual ssize_t send(const struct iovec* iov, int iovcnt, const Addr* addr,
                         size_t addr_len, int flags = 0) override {
        if (likely(!addr)) return do_send(iov, iovcnt, nullptr, 0, flags);

        auto un = to_addr_un(addr, addr_len);
        return do_send(iov, iovcnt, (sockaddr*)&un, sizeof(un), flags);
    }
    virtual ssize_t recv(const struct iovec* iov, int iovcnt, Addr* addr,
                         size_t* addr_len, int flags) override {
        if (likely(!addr || !addr_len || !*addr_len))
            return do_recv(iov, iovcnt, nullptr, 0, flags);

        sockaddr_un un;
        size_t alen = sizeof(un);
        auto ret = do_recv(iov, iovcnt, (sockaddr*)&un, &alen, flags);
        if (ret >= 0) {
            if (un.sun_family != AF_UNIX) return -1;
            size_t len = strlen(un.sun_path) + 1;
            if (len <= *addr_len) {
                *addr_len = len;
            } else {
                auto t = len;
                len = *addr_len;
                *addr_len = t;
            }
            memcpy(addr, un.sun_path, len);
        }
        return ret;
    }
};

UDPSocket* new_udp_socket(int fd) {
    auto sock = NewObj<UDP>(AF_INET, MAX_UDP_MESSAGE_SIZE)->init(fd);
    return (UDPSocket*)sock;
}

UDS_DatagramSocket* new_uds_datagram_socket(int fd) {
    auto sock = NewObj<UDS>(AF_UNIX, MAX_UDS_MESSAGE_SIZE)->init(fd);
    return (UDS_DatagramSocket*)sock;
}

}  // namespace net
}  // namespace photon