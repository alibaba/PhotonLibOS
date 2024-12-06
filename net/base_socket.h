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

/***
Internal header provides abstract socket base class
***/

#include <netinet/in.h>
#include <vector>

#include <photon/net/socket.h>

#define __UNIMPLEMENTED__(method, ret)  \
    virtual method override {           \
        errno = ENOSYS;                 \
        return ret;                     \
    }

#define UNIMPLEMENTED(method)      __UNIMPLEMENTED__(method, -1)
#define UNIMPLEMENTED_PTR(method)  __UNIMPLEMENTED__(method, nullptr)
#define UNIMPLEMENTED_VOID(method) __UNIMPLEMENTED__(method, )

namespace photon {
namespace net {

// sockaddr_storage is the container for any socket address type
struct sockaddr_storage {
    sockaddr_storage() = default;
    explicit sockaddr_storage(const EndPoint& ep) {
        if (ep.is_ipv4()) {
            auto* in4 = (sockaddr_in*) &store;
            in4->sin_family = AF_INET;
            in4->sin_port = htons(ep.port);
            in4->sin_addr.s_addr = ep.addr.to_nl();
        } else {
            auto* in6 = (sockaddr_in6*) &store;
            in6->sin6_family = AF_INET6;
            in6->sin6_port = htons(ep.port);
            in6->sin6_addr = ep.addr.addr;
        }
    }
    explicit sockaddr_storage(const sockaddr_in& addr) {
        *((sockaddr_in*) &store) = addr;
    }
    explicit sockaddr_storage(const sockaddr_in6& addr) {
        *((sockaddr_in6*) &store) = addr;
    }
    explicit sockaddr_storage(const sockaddr& addr) {
        *((sockaddr*) &store) = addr;
    }
    EndPoint to_endpoint() const {
        EndPoint ep;
        if (store.ss_family == AF_INET6) {
            auto s6 = (sockaddr_in6*) &store;
            ep.addr = IPAddr(s6->sin6_addr);
            ep.port = ntohs(s6->sin6_port);
        } else if (store.ss_family == AF_INET) {
            auto s4 = (sockaddr_in*) &store;
            ep.addr = IPAddr(s4->sin_addr);
            ep.port = ntohs(s4->sin_port);
        }
        return ep;
    }
    sockaddr* get_sockaddr() const {
        return (sockaddr*) &store;
    }
    socklen_t get_socklen() const {
        switch (store.ss_family) {
            case AF_INET:
                return sizeof(sockaddr_in);
            case AF_INET6:
                return sizeof(sockaddr_in6);
            default:
                return 0;
        }
    }
    socklen_t get_max_socklen() const {
        return sizeof(store);
    }
    // store must be zero initialized
    ::sockaddr_storage store = {};
};

struct SocketOpt {
    int level;
    int opt_name;
    void* opt_val;
    socklen_t opt_len;
};

class SockOptBuffer : public std::vector<SocketOpt> {
protected:
    static constexpr uint64_t BUFFERSIZE = 4096;
    char buffer[BUFFERSIZE];
    char* ptr = buffer;

public:
    int put_opt(int level, int name, const void* val, socklen_t len) {
        if (ptr + len >= buffer + BUFFERSIZE) {
            return -1;
        }
        memcpy(ptr, val, len);
        push_back(SocketOpt{level, name, ptr, len});
        ptr += len;
        return 0;
    }

    int get_opt(int level, int name, void* val, socklen_t* len) {
        for (auto& x : *this)
            if (level == x.level && name == x.opt_name && *len >= x.opt_len)
                return memcpy(val, x.opt_val, *len = x.opt_len), 0;
        return -1;
    }

    virtual int setsockopt(int fd) {
        for (auto& opt : *this) {
            if (::setsockopt(fd, opt.level, opt.opt_name, opt.opt_val, opt.opt_len) != 0) {
                LOG_ERRNO_RETURN(EINVAL, -1, "Failed to setsockopt ",
                                 VALUE(opt.level), VALUE(opt.opt_name), VALUE(opt.opt_val));
            }
        }
        return 0;
    }
};

class SocketClientBase : public ISocketClient {
public:
    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        return m_opts.put_opt(level, option_name, option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        return m_opts.get_opt(level, option_name, option_value, option_len);
    }

    uint64_t timeout() const override { return m_timeout; }

    void timeout(uint64_t tm) override { m_timeout = tm; }

    UNIMPLEMENTED_PTR(Object* get_underlay_object(uint64_t recursion = 0))

protected:
    uint64_t m_timeout = -1;
    SockOptBuffer m_opts;
};

class SocketServerBase : public ISocketServer {
public:
    uint64_t timeout() const override { return m_timeout; }

    void timeout(uint64_t tm) override { m_timeout = tm; }

    UNIMPLEMENTED(int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len))
    UNIMPLEMENTED(int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len))
    UNIMPLEMENTED(int getsockname(EndPoint& addr))
    UNIMPLEMENTED(int getsockname(char* path, size_t count))
    UNIMPLEMENTED(int getpeername(EndPoint& addr))
    UNIMPLEMENTED(int getpeername(char* path, size_t count))
    UNIMPLEMENTED_PTR(Object* get_underlay_object(uint64_t recursion = 0))

protected:
    uint64_t m_timeout = -1;
    SockOptBuffer m_opts;
};

class ForwardSocketClient : public ISocketClient {
public:
    ForwardSocketClient(ISocketClient* underlay, bool ownership)
            : m_underlay(underlay), m_ownership(ownership) {}

    ~ForwardSocketClient() {
        if (m_ownership) {
            delete m_underlay;
        }
    }

    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        return m_underlay->setsockopt(level, option_name, option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        return m_underlay->getsockopt(level, option_name, option_value, option_len);
    }

    uint64_t timeout() const override { return m_underlay->timeout(); }

    void timeout(uint64_t tm) override { m_underlay->timeout(tm); }

    Object* get_underlay_object(uint64_t recursion = 0) override {
        return (recursion == 0) ? m_underlay : m_underlay->get_underlay_object(recursion - 1);
    }

protected:
    ISocketClient* m_underlay;
    bool m_ownership;
};

class SocketStreamBase : public ISocketStream {
public:
    UNIMPLEMENTED_PTR(Object* get_underlay_object(uint64_t recursion = 0));
    UNIMPLEMENTED(int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len))
    UNIMPLEMENTED(int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len))
    UNIMPLEMENTED(int getsockname(EndPoint& addr))
    UNIMPLEMENTED(int getpeername(EndPoint& addr))
    UNIMPLEMENTED(int getsockname(char* path, size_t count))
    UNIMPLEMENTED(int getpeername(char* path, size_t count))
    UNIMPLEMENTED(uint64_t timeout() const)
    UNIMPLEMENTED_VOID(void timeout(uint64_t tm))
    UNIMPLEMENTED(int close())
    UNIMPLEMENTED(ssize_t read(void* buf, size_t count))
    UNIMPLEMENTED(ssize_t readv(const struct iovec* iov, int iovcnt))
    UNIMPLEMENTED(ssize_t write(const void* buf, size_t count))
    UNIMPLEMENTED(ssize_t writev(const struct iovec* iov, int iovcnt))
    UNIMPLEMENTED(ssize_t recv(void* buf, size_t count, int flags = 0))
    UNIMPLEMENTED(ssize_t recv(const struct iovec* iov, int iovcnt, int flags = 0))
    UNIMPLEMENTED(ssize_t send(const void* buf, size_t count, int flags = 0))
    UNIMPLEMENTED(ssize_t send(const struct iovec* iov, int iovcnt, int flags = 0))
    UNIMPLEMENTED(ssize_t sendfile(int in_fd, off_t offset, size_t count))
};

class ForwardSocketServer : public ISocketServer {
public:
    ForwardSocketServer(ISocketServer* underlay, bool ownership)
            : m_underlay(underlay), m_ownership(ownership) {}

    ~ForwardSocketServer() {
        if (m_ownership) {
            delete m_underlay;
        }
    }

    Object* get_underlay_object(uint64_t recursion = 0) override {
        return (recursion == 0) ? m_underlay : m_underlay->get_underlay_object(recursion - 1);
    }

    int bind(uint16_t port, IPAddr addr) override {
        return m_underlay->bind(port, addr);
    }

    int bind(const char* path, size_t count) override {
        return m_underlay->bind(path, count);
    }

    int listen(int backlog = 1024) override {
        return m_underlay->listen(backlog);
    }

    int start_loop(bool block = false) override {
        return m_underlay->start_loop(block);
    }

    void terminate() override { return m_underlay->terminate(); }

    int getsockname(EndPoint& addr) override {
        return m_underlay->getsockname(addr);
    }

    int getpeername(EndPoint& addr) override {
        return m_underlay->getpeername(addr);
    }

    int getsockname(char* path, size_t count) override {
        return m_underlay->getsockname(path, count);
    }

    int getpeername(char* path, size_t count) override {
        return m_underlay->getpeername(path, count);
    }

    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        return m_underlay->setsockopt(level, option_name, option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        return m_underlay->getsockopt(level, option_name, option_value, option_len);
    }

    uint64_t timeout() const override { return m_underlay->timeout(); }

    void timeout(uint64_t tm) override { m_underlay->timeout(tm); }

protected:
    ISocketServer* m_underlay;
    bool m_ownership;
};

class ForwardSocketStream : public ISocketStream {
public:
    ForwardSocketStream(ISocketStream* underlay, bool ownership) :
            m_underlay(underlay), m_ownership(ownership) {
    }

    ~ForwardSocketStream() {
        if (m_ownership) {
            delete m_underlay;
        }
    }

    int getsockname(EndPoint& addr) override {
        return m_underlay->getsockname(addr);
    }

    int getpeername(EndPoint& addr) override {
        return m_underlay->getpeername(addr);
    }

    int getsockname(char* path, size_t count) override {
        return m_underlay->getsockname(path, count);
    }

    int getpeername(char* path, size_t count) override {
        return m_underlay->getpeername(path, count);
    }

    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        return m_underlay->setsockopt(level, option_name, option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        return m_underlay->getsockopt(level, option_name, option_value, option_len);
    }

    uint64_t timeout() const override { return m_underlay->timeout(); }

    void timeout(uint64_t tm) override { m_underlay->timeout(tm); }

    Object* get_underlay_object(uint64_t recursion = 0) override {
        return (recursion == 0) ? m_underlay : m_underlay->get_underlay_object(recursion - 1);
    }

protected:
    ISocketStream* m_underlay;
    bool m_ownership;
};

}  // namespace net
}  // namespace photon
