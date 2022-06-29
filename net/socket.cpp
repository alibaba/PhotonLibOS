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

#include "socket.h"

#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <memory>
#include <unordered_map>

#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/thread/list.h>
#include <photon/thread/timer.h>
#include <photon/common/utility.h>
#include <photon/net/abstract_socket.h>
#include <photon/net/basic_socket.h>
#include <photon/net/tlssocket.h>
#include <photon/net/utils.h>
#include <photon/net/zerocopy.h>

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

namespace photon {
namespace net {

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

static int fill_path(struct sockaddr_un& name, const char* path, size_t count) {
    const int LEN = sizeof(name.sun_path) - 1;
    if (count == 0) count = strlen(path);
    if (count > LEN)
        LOG_ERROR_RETURN(ENAMETOOLONG, -1, "pathname is too long (`>`)", count,
                         LEN);

    memset(&name, 0, sizeof(name));
    memcpy(name.sun_path, path, count + 1);
#ifndef __linux__
    name.sun_len = 0;
#endif
    name.sun_family = AF_UNIX;
    return 0;
}

class KernelSocket : public SocketBase {
public:
    uint64_t m_timeout = -1;
    int fd, m_socket_family;
    bool m_autoremove = false;

    explicit KernelSocket(int fd) : fd(fd), m_socket_family(-1) {}
    KernelSocket(int socket_family, bool autoremove, bool nonblocking = true)
        : m_socket_family(socket_family), m_autoremove(autoremove) {
        if (nonblocking) {
            fd = net::socket(socket_family, SOCK_STREAM, 0);
        } else {
            fd = ::socket(socket_family, SOCK_STREAM, 0);
        }
        if (fd > 0 && socket_family == AF_INET) {
            int val = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
        }
    }
    virtual ~KernelSocket() {
        if (fd <= 0) return;
        if (m_autoremove) {
            char filename[PATH_MAX];
            if (0 == getsockname(filename, PATH_MAX)) {
                unlink(filename);
            }
        }
        close();
    }
    virtual Object* get_underlay_object(int) override {
        return (Object*)(uint64_t)fd;
    }
    virtual int close() override {
        auto ret = ::close(fd);
        fd = -1;
        return ret;
    }
    typedef int (*Getter)(int sockfd, struct sockaddr* addr,
                          socklen_t* addrlen);
    int do_getname(EndPoint& addr, Getter getter) {
        struct sockaddr_in addr_in;
        socklen_t len = sizeof(addr_in);
        int ret = getter(fd, (struct sockaddr*)&addr_in, &len);
        if (ret < 0 || len != sizeof(addr_in)) return -1;
        addr.from_sockaddr_in(addr_in);
        return 0;
    }
    virtual int getsockname(EndPoint& addr) override {
        return do_getname(addr, &::getsockname);
    }
    virtual int getpeername(EndPoint& addr) override {
        return do_getname(addr, &::getpeername);
    }
    int do_getname(char* path, size_t count, Getter getter) {
        struct sockaddr_un addr_un;
        socklen_t len = sizeof(addr_un);
        int ret = getter(fd, (struct sockaddr*)&addr_un, &len);
        // if len larger than size of addr_un, or less than prefix in addr_un
        if (ret < 0 || len > sizeof(addr_un) ||
            len <= sizeof(addr_un.sun_family))
            return -1;

        strncpy(path, addr_un.sun_path, count);
        return 0;
    }
    virtual int getsockname(char* path, size_t count) override {
        return do_getname(path, count, &::getsockname);
    }
    virtual int getpeername(char* path, size_t count) override {
        return do_getname(path, count, &::getpeername);
    }
    virtual int setsockopt(int level, int option_name, const void* option_value,
                           socklen_t option_len) override {
        return ::setsockopt(fd, level, option_name, option_value, option_len);
    }
    virtual int getsockopt(int level, int option_name, void* option_value,
                           socklen_t* option_len) override {
        return ::getsockopt(fd, level, option_name, option_value, option_len);
    }
};

class KernelSocketStream : public KernelSocket {
public:
    photon::mutex m_rmutex, m_wmutex;
    using KernelSocket::KernelSocket;
    KernelSocketStream(int socket_family, bool autoremove)
        : KernelSocket(socket_family, autoremove, true) {}
    virtual ~KernelSocketStream() {
        if (fd > 0) shutdown(ShutdownHow::ReadWrite);
    }
    virtual ssize_t read(void* buf, size_t count) override {
        photon::scoped_lock lock(m_rmutex);
        return net::read_n(fd, buf, count, m_timeout);
    }
    virtual ssize_t write(const void* buf, size_t count) override {
        photon::scoped_lock lock(m_wmutex);
        return net::send2_n(fd, buf, count, 0, m_timeout);
    }
    virtual ssize_t readv(const struct iovec* iov, int iovcnt) override {
        SmartCloneIOV<32> ciov(iov, iovcnt);
        return readv_mutable(ciov.ptr, iovcnt);
    }
    virtual ssize_t readv_mutable(struct iovec* iov, int iovcnt) override {
        photon::scoped_lock lock(m_rmutex);
        return net::readv_n(fd, iov, iovcnt, m_timeout);
    }
    virtual ssize_t writev(const struct iovec* iov, int iovcnt) override {
        SmartCloneIOV<32> ciov(iov, iovcnt);
        return writev_mutable(ciov.ptr, iovcnt);
    }
    virtual ssize_t writev_mutable(struct iovec* iov, int iovcnt) override {
        photon::scoped_lock lock(m_wmutex);
        return net::sendv2_n(fd, iov, iovcnt, 0, m_timeout);
    }
    virtual ssize_t recv(void* buf, size_t count) override {
        photon::scoped_lock lock(m_rmutex);
        return net::read(fd, buf, count, m_timeout);
    }
    virtual ssize_t recv(const struct iovec* iov, int iovcnt) override {
        photon::scoped_lock lock(m_rmutex);
        return net::readv(fd, iov, iovcnt, m_timeout);
    }
    virtual ssize_t send(const void* buf, size_t count) override {
        photon::scoped_lock lock(m_wmutex);
        return net::send2(fd, buf, count, 0, m_timeout);
    }
    virtual ssize_t send(const struct iovec* iov, int iovcnt) override {
        photon::scoped_lock lock(m_wmutex);
        return net::sendv2(fd, iov, iovcnt, 0, m_timeout);
    }

    virtual ssize_t send2(const void* buf, size_t count, int flag) override {
        photon::scoped_lock lock(m_wmutex);
        return net::send2_n(fd, (void*)buf, (size_t)count, flag, m_timeout);
    }
    virtual ssize_t send2(const struct iovec* iov, int iovcnt, int flag) override {
        photon::scoped_lock lock(m_wmutex);
        return net::sendv2_n(fd, (struct iovec*)iov, (int)iovcnt, flag, m_timeout);
    }

    virtual ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        photon::scoped_lock lock(m_wmutex);
        return net::sendfile_n(fd, in_fd, &offset, count);
    }
    virtual uint64_t timeout() override { return m_timeout; }
    virtual void timeout(uint64_t tm) override { m_timeout = tm; }
    virtual int shutdown(ShutdownHow how) override {
        // shutdown how defined as 0 for RD, 1 for WR and 2 for RDWR
        // in sys/socket.h, cast ShutdownHow into int just fits
        return ::shutdown(fd, static_cast<int>(how));
    }
};

template<typename SOCKET, typename...Ts> static
KernelSocketStream* socket_ctor(Ts...xs) { return new SOCKET(xs...); }

class KernelSocketClient : public KernelSocket {
public:
    using KernelSocket::KernelSocket;

    int (*_do_connect)(int fd, const struct sockaddr*,
        socklen_t addrlen, uint64_t timeout) = &net::connect;

    KernelSocketStream* (*_ctor2)(int socket_family, bool autoremove) =
        &socket_ctor<KernelSocketStream>;

    SockOptBuffer opts;
    virtual int setsockopt(int level, int option_name,
            const void* option_value, socklen_t option_len) override
    {
        return opts.put_opt(level, option_name, option_value, option_len) ;
    }
    virtual int getsockopt(int level, int option_name,
            void* option_value, socklen_t* option_len) override
    {
        return opts.get_opt(level, option_name, option_value, option_len) ;
    }

    ISocketStream* connect(void* addr, size_t addr_sz)
    {
        auto s = _ctor2(m_socket_family, m_autoremove);
        std::unique_ptr<KernelSocketStream> sock(s);
        if (!sock || sock->fd < 0) {
            LOG_ERROR_RETURN(0, nullptr, "Failed to create socket fd");
        }
        for (auto& opt : opts) {
            auto ret = sock->setsockopt(opt.level, opt.opt_name, opt.opt_val,
                                        opt.opt_len);
            if (ret < 0) {
                LOG_ERROR_RETURN(EINVAL, nullptr, "Failed to setsockopt ",
                                 VALUE(opt.level), VALUE(opt.opt_name));
            }
        }
        sock->timeout(m_timeout);
        auto ret = _do_connect(sock->fd, (struct sockaddr*)addr,
                              addr_sz, m_timeout);
        if (ret < 0) {
            LOG_ERRNO_RETURN(0, nullptr, "Failed to connect socket");
        }
        return sock.release();
    }

    virtual ISocketStream* connect(const EndPoint& ep) override {
        auto addr_in = ep.to_sockaddr_in();
        return connect(&addr_in, sizeof(addr_in));
    }

    virtual ISocketStream* connect(const char* path, size_t count) override {
        struct sockaddr_un addr_un;
        int ret = fill_path(addr_un, path, count);
        if (ret < 0) {
            LOG_ERROR_RETURN(0, nullptr, "Failed to fill uds addr");
        }
        return connect(&addr_un, sizeof(addr_un));
    }

    virtual uint64_t timeout() override { return m_timeout; }

    virtual void timeout(uint64_t tm) override { m_timeout = tm; }
};

class ZeroCopySocketStream : public KernelSocketStream {
protected:
    net::ZerocopyEventEntry* m_event_entry;
    uint32_t m_num_calls;
    bool m_socket_error;

public:
    explicit ZeroCopySocketStream(int fd) : KernelSocketStream(fd) {
        m_num_calls = 0;
        m_socket_error = false;
        m_event_entry = new ZerocopyEventEntry(fd);
    }

    ~ZeroCopySocketStream() { delete m_event_entry; }

    ssize_t writev_mutable(iovec* iov, int iovcnt) override {
        ssize_t n_written;
        auto iov_view = iovector_view(iov, iovcnt);
        size_t sum = iov_view.sum();

        {
            photon::scoped_lock lock(m_wmutex);
            n_written =
                zerocopy_n(fd, iov_view.iov, iovcnt, m_num_calls, m_timeout);
            if (n_written != (ssize_t)sum) {
                LOG_ERRNO_RETURN(0, n_written, "zerocopy failed");
            }
        }

        int ret = m_event_entry->zerocopy_wait(m_num_calls - 1, m_timeout);
        if (ret == 0) {
            m_socket_error = true;
            LOG_ERRNO_RETURN(ETIMEDOUT, -1, "zerocopy wait timeout (active)")
        }
        if (m_socket_error) {
            LOG_ERRNO_RETURN(ETIMEDOUT, -1, "zerocopy wait timeout (passive)");
        }
        return n_written;
    }
};

class KernelSocketServer : public KernelSocket {
protected:
    Handler m_handler;
    photon::thread* workth = nullptr;
    bool waiting = false;

    int accept_loop() {
        if (workth) LOG_ERROR_RETURN(EALREADY, -1, "Already listening");
        workth = photon::CURRENT;
        DEFER(workth = nullptr);
        while (workth) {
            waiting = true;
            auto sess = accept();
            waiting = false;
            if (!workth) return 0;
            if (sess) {
                photon::thread_create11(&KernelSocketServer::handler, m_handler,
                                        sess);
            } else {
                photon::thread_usleep(1000);
            }
        }
        return 0;
    }

    static void handler(Handler m_handler, ISocketStream* sess) {
        m_handler(sess);
        delete sess;
    }

public:
    using KernelSocket::KernelSocket;
    int (*_do_accept)(int fd, struct sockaddr *addr,
        socklen_t *addrlen, uint64_t timeout) = &net::accept;

    KernelSocketStream* (*_ctor1)(int fd) =
        &socket_ctor<KernelSocketStream>;

    virtual ~KernelSocketServer() { terminate(); }
    virtual uint64_t timeout() override { return m_timeout; }
    virtual void timeout(uint64_t tm) override { m_timeout = tm; }

    virtual int start_loop(bool block) override {
        if (workth) LOG_ERROR_RETURN(EALREADY, -1, "Already listening");
        if (block) return accept_loop();
        auto th =
            photon::thread_create11(&KernelSocketServer::accept_loop, this);
        photon::thread_yield_to(th);
        return 0;
    }

    virtual void terminate() override {
        if (workth) {
            auto th = workth;
            workth = nullptr;
            if (waiting) {
                photon::thread_interrupt(th);
                photon::thread_yield_to(th);
            }
        }
    }

    virtual ISocketServer* set_handler(Handler handler) override {
        m_handler = handler;
        return this;
    }
    virtual int bind(uint16_t port, IPAddr addr) override {
        auto addr_in = EndPoint(addr, port).to_sockaddr_in();
        return ::bind(fd, (struct sockaddr*)&addr_in, sizeof(addr_in));
    }
    virtual int bind(const char* path, size_t count) override {
        if (m_autoremove && is_socket(path)) {
            unlink(path);
        }
        struct sockaddr_un addr_un;
        int ret = fill_path(addr_un, path, count);
        if (ret < 0) return -1;
        return ::bind(fd, (struct sockaddr*)&addr_un, sizeof(addr_un));
    }
    virtual int listen(int backlog) override { return ::listen(fd, backlog); }

    int do_accept() { return _do_accept(fd, nullptr, nullptr, m_timeout); }
    int do_accept(EndPoint& remote_endpoint) {
        struct sockaddr_in addr_in;
        socklen_t len = sizeof(addr_in);
        int cfd = _do_accept(fd, (struct sockaddr*)&addr_in, &len, m_timeout);
        if (cfd < 0 || len != sizeof(addr_in)) return -1;
        remote_endpoint.from_sockaddr_in(addr_in);
        return cfd;
    }
    virtual ISocketStream* accept(EndPoint* remote_endpoint) override {
        int cfd = remote_endpoint ? do_accept(*remote_endpoint) : do_accept();
        return cfd < 0 ? nullptr : _ctor1(cfd);
    }
    virtual ISocketStream* accept() override {
        int cfd = do_accept();
        return cfd < 0 ? nullptr : _ctor1(cfd);
    }
    bool is_socket(const char* path) const {
        struct stat statbuf;
        return (0 == stat(path, &statbuf)) ?
            S_ISSOCK(statbuf.st_mode) : false;
    }
};

class TcpSocketServer0c : public KernelSocketServer {
public:
    TcpSocketServer0c(int socket_family, bool autoremove) :
        KernelSocketServer(socket_family, autoremove, true)
    {
        int v = 1;
        _ctor1 = &socket_ctor<ZeroCopySocketStream>;
        KernelSocketServer::setsockopt(-SOL_SOCKET, SO_ZEROCOPY, &v, sizeof(v));
    }
};

class IouringSocketStream : public KernelSocketStream {
public:
    explicit IouringSocketStream(int fd) : KernelSocketStream(fd) {}

    IouringSocketStream(int socket_family, bool autoremove) :
        KernelSocketStream(socket_family, autoremove, false) {}

    ssize_t read(void* buf, size_t count) override {
        photon::scoped_lock lock(m_rmutex);
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(photon::iouring_pread(fd, buf, count, 0, timeout));
        return net::doio_n(buf, count, cb);
    }

    ssize_t write(const void* buf, size_t count) override {
        photon::scoped_lock lock(m_wmutex);
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(photon::iouring_pwrite(fd, buf, count, 0, timeout));
        return net::doio_n((void*&) buf, count, cb);
    }

    ssize_t readv(const iovec* iov, int iovcnt) override {
        photon::scoped_lock lock(m_rmutex);
        SmartCloneIOV<8> clone(iov, iovcnt);
        iovector_view view(clone.ptr, iovcnt);
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(photon::iouring_preadv(fd, view.iov, view.iovcnt, 0, timeout));
        return net::doiov_n(view, cb);
    }

    ssize_t writev(const iovec* iov, int iovcnt) override {
        photon::scoped_lock lock(m_wmutex);
        SmartCloneIOV<8> clone(iov, iovcnt);
        iovector_view view(clone.ptr, iovcnt);
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(photon::iouring_pwritev(fd, view.iov, view.iovcnt, 0, timeout));
        return net::doiov_n(view, cb);
    }

    ssize_t recv(void* buf, size_t count) override {
        photon::scoped_lock lock(m_rmutex);
        return photon::iouring_recv(fd, buf, count, 0, m_timeout);
    }

    ssize_t recv(const iovec* iov, int iovcnt) override {
        photon::scoped_lock lock(m_rmutex);
        return photon::iouring_preadv(fd, iov, iovcnt, 0, m_timeout);
    }

    ssize_t send(const void* buf, size_t count) override {
        photon::scoped_lock lock(m_wmutex);
        return photon::iouring_send(fd, buf, count, 0, m_timeout);
    }

    // fully send
    ssize_t send2(const void* buf, size_t count, int flag) override {
        photon::scoped_lock lock(m_wmutex);
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(photon::iouring_send(fd, buf, count, flag, timeout));
        return net::doio_n((void*&) buf, count, cb);
    }

    // fully sendmsg
    ssize_t send2(const struct iovec* iov, int iovcnt, int flag) override {
        photon::scoped_lock lock(m_wmutex);
        iovector_view view((iovec*) iov, iovcnt);
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(do_sendmsg(fd, view.iov, view.iovcnt, flag, timeout));
        return net::doiov_n(view, cb);
    }

private:
    static ssize_t do_sendmsg(int fd, iovec* iov, int iovcnt, int flag, uint64_t timeout) {
        msghdr msg = {};
        msg.msg_iov = iov;
        msg.msg_iovlen = iovcnt;
        return photon::iouring_sendmsg(fd, &msg, flag, timeout);
    }
};

class IouringSocketClient : public KernelSocketClient {
public:
    IouringSocketClient(int socket_family, bool autoremove)
        : KernelSocketClient(socket_family, autoremove)
    {
        _ctor2 = &socket_ctor<IouringSocketStream>;
        _do_connect = &photon::iouring_connect;
    }
};

class IouringSocketServer : public KernelSocketServer {
public:
    IouringSocketServer(int socket_family, bool autoremove) :
        KernelSocketServer(socket_family, autoremove, false)
    {
        _ctor1 = &socket_ctor<IouringSocketStream>;
        _do_accept = &photon::iouring_accept;
    }
};

LogBuffer& operator<<(LogBuffer& log, const IPAddr addr) {
    return log.printf(addr.d, '.', addr.c, '.', addr.b, '.', addr.a);
}

LogBuffer& operator<<(LogBuffer& log, const EndPoint ep) {
    return log << ep.addr << ':' << ep.port;
}

LogBuffer& operator<<(LogBuffer& log, const in_addr& iaddr) {
    return log << net::IPAddr(ntohl(iaddr.s_addr));
}

LogBuffer& operator<<(LogBuffer& log, const sockaddr_in& addr) {
    return log << net::EndPoint(addr);
}

LogBuffer& operator<<(LogBuffer& log, const sockaddr& addr) {
    if (addr.sa_family == AF_INET) {
        log << (const sockaddr_in&)addr;
    } else {
        log.printf("<sockaddr>");
    }
    return log;
}

template <typename SocketCS>
static SocketCS* new_socketcs(int socket_family, bool autoremove,
                                           ALogStringL socktype) {
    auto sock = new SocketCS(socket_family, autoremove);
    if (sock->fd < 0) {
        delete sock;
        LOG_ERROR_RETURN(0, nullptr, "Failed to create ` socket", socktype);
    }
    return sock;
}

extern "C" ISocketClient* new_tcp_socket_client() {
    return new_socketcs<KernelSocketClient>(AF_INET, false, "TCP client");
}
extern "C" ISocketServer* new_tcp_socket_server() {
    return new_socketcs<KernelSocketServer>(AF_INET, false, "TCP server");
}
extern "C" ISocketServer* new_tcp_socket_server_0c() {
    return new_socketcs<TcpSocketServer0c>(AF_INET, false, "TCP zero-copy server");
}
extern "C" ISocketClient* new_socket_client_iouring() {
    return new_socketcs<IouringSocketClient>(AF_INET, false, "iouring-based TCP client");
}
extern "C" ISocketServer* new_socket_server_iouring() {
    return new_socketcs<IouringSocketServer>(AF_INET, false, "iouring-based TCP server");
}
extern "C" ISocketClient* new_uds_client() {
    return new_socketcs<KernelSocketClient>(AF_UNIX, false, "UNIX domain socket client");
}
extern "C" ISocketServer* new_uds_server(bool autoremove) {
    return new_socketcs<KernelSocketServer>(AF_UNIX, autoremove, "UNIX domain socket server");
}

class TCPSocketPool;

class PooledTCPSocket : public ISocketStream {
public:
    TCPSocketPool* pool;
    ISocketStream* stream;
    EndPoint ep;
    bool drop;

    PooledTCPSocket(ISocketStream* stream, TCPSocketPool* pool,
                    const EndPoint& ep)
        : pool(pool), stream(stream), ep(ep), drop(false) {}
    // release socket back to pool when dtor
    ~PooledTCPSocket() override;
    // forwarding all actions
    int shutdown(ShutdownHow how) override {
        drop = true;
        return stream->shutdown(how);
    }

#define FORWARD_SOCK_ACT(how, action, count)                            \
    if (count == 0) return 0;                                           \
    auto ret = stream->action;                                          \
    if (ret < 0 || (ShutdownHow::Read == ShutdownHow::how && ret == 0)) \
        drop = true;                                                    \
    return ret

#define FORWARD_SOCK_OP(action) return stream->action;

    Object* get_underlay_object(int level) override {
        return level ? stream->get_underlay_object(level - 1) : nullptr;
    }
    int close() override {
        drop = true;
        FORWARD_SOCK_OP(close());
    }
    int getsockname(EndPoint& addr) override {
        FORWARD_SOCK_OP(getsockname(addr));
    }
    int getpeername(EndPoint& addr) override {
        FORWARD_SOCK_OP(getpeername(addr));
    }
    int getsockname(char* path, size_t count) override {
        FORWARD_SOCK_OP(getsockname(path, count));
    }
    int getpeername(char* path, size_t count) override {
        FORWARD_SOCK_OP(getpeername(path, count));
    }
    int setsockopt(int level, int option_name, const void* option_value,
                   socklen_t option_len) override {
        FORWARD_SOCK_OP(
            setsockopt(level, option_name, option_value, option_len));
    }
    int getsockopt(int level, int option_name, void* option_value,
                   socklen_t* option_len) override {
        FORWARD_SOCK_OP(
            getsockopt(level, option_name, option_value, option_len));
    }
    uint64_t timeout() override { FORWARD_SOCK_OP(timeout()); }
    void timeout(uint64_t tm) override { FORWARD_SOCK_OP(timeout(tm)); }

    ssize_t read(void* buf, size_t count) override {
        FORWARD_SOCK_ACT(Read, read(buf, count), count);
    }
    ssize_t write(const void* buf, size_t count) override {
        FORWARD_SOCK_ACT(Write, write(buf, count), count);
    }
    ssize_t readv(const struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Read, readv(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t readv_mutable(struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Read, readv_mutable(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t writev(const struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Write, writev(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t writev_mutable(struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Write, writev_mutable(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t recv(void* buf, size_t count) override {
        FORWARD_SOCK_ACT(Read, recv(buf, count), count);
    }
    ssize_t recv(const struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Read, recv(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t send(const void* buf, size_t count) override {
        FORWARD_SOCK_ACT(Write, send(buf, count), count);
    }
    ssize_t send(const struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Write, send(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t send2(const void* buf, size_t count, int flag) override {
        FORWARD_SOCK_ACT(Write, send2(buf, count, flag), count);
    }
    ssize_t send2(const struct iovec* iov, int iovcnt, int flag) override {
        FORWARD_SOCK_ACT(Write, send2(iov, iovcnt, flag),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        FORWARD_SOCK_ACT(Write, sendfile(in_fd, offset, count), count);
    }

#undef FORWARD_SOCK_ACT
#undef FORWARD_SOCK_OP
};

struct StreamListNode : public intrusive_list_node<StreamListNode> {
    EndPoint key;
    std::unique_ptr<ISocketStream> stream;
    Timeout expire;

    StreamListNode() : expire(0) {}
    StreamListNode(EndPoint key, ISocketStream* stream, uint64_t expire)
        : key(key), stream(stream), expire(expire) {}
};

class TCPSocketPool : public ISocketClient {
protected:
    CascadingEventEngine* ev;
    photon::thread* collector;
    std::unordered_multimap<EndPoint, StreamListNode*> fdmap;
    std::unordered_map<int, EndPoint> fdep;
    intrusive_list<StreamListNode> lru;
    ISocketClient* client;
    uint64_t expiration;
    photon::Timer timer;

public:
    TCPSocketPool(ISocketClient* client, uint64_t expiration)
        : ev(photon::new_epoll_cascading_engine()),
          client(client),
          expiration(expiration),
          timer(0, {this, &TCPSocketPool::evict}) {
        collector = (photon::thread*)photon::thread_enable_join(
            photon::thread_create11(&TCPSocketPool::collect, this));
    }

    ~TCPSocketPool() override {
        timer.stop();
        auto th = collector;
        collector = nullptr;
        photon::thread_interrupt((photon::thread*)th);
        photon::thread_join((photon::join_handle*)th);
        for (auto fe : fdep) {
            ev->rm_interest(
                {(int)fe.first, EVENT_READ, (void*)(uint64_t)fe.first});
        }
        while (!lru.empty()) {
            auto node = lru.pop_front();
            delete node;
        }
        delete ev;
        delete client;
    }

    void drop_from_pool(int fd) {
        // remove fd interest
        ev->rm_interest({(int)fd, EVENT_READ, (void*)(int64_t)fd});
        // find fdep & fdmap and remove
        auto it = fdep.find(fd);
        if (it != fdep.end()) {
            auto ep = it->second;
            for (auto map_it = fdmap.find(ep);
                 map_it != fdmap.end() && map_it->first == ep; map_it++) {
                if (map_it->second->stream->get_underlay_fd() == fd) {
                    auto node = map_it->second;
                    fdmap.erase(map_it);
                    lru.erase(node);
                    delete node;
                    break;
                }
            }
            fdep.erase(it);
        }
    }

    void drop_from_pool(StreamListNode* node) {
        // remove fd interest
        auto fd = node->stream->get_underlay_fd();
        if (fd >= 0) return drop_from_pool(fd);
        // or node have no record
        for (auto map_it = fdmap.find(node->key);
             map_it != fdmap.end() && map_it->first == node->key; map_it++) {
            if (map_it->second == node) {
                fdmap.erase(map_it);
                lru.erase(node);
                break;
            }
        }
        delete node;
    }

    uint64_t evict() {
        photon::thread_yield_to(photon::CURRENT);
        auto evict_cnt = fdmap.size() >> 1;
        auto cnt = 0;
        while (!lru.empty() && evict_cnt &&
               lru.front()->expire.expire() <= photon::now) {
            auto node = lru.front();
            drop_from_pool(node);
            evict_cnt--;
            cnt++;
        }
        if (lru.empty() || cnt) return expiration;
        return lru.front()->expire.timeout();
    }

    bool release(EndPoint ep, ISocketStream* stream) {
        auto fd = stream->get_underlay_fd();
        if (fd >= 0) {
            // able to fetch fd
            // check by epoll
            if (wait_for_fd_readable(fd, 0) == 0) {
                LOG_TEMP("refuse put in pool");
                return false;
            }
            ev->add_interest({fd, EVENT_READ, (void*)(uint64_t)fd});
            fdep.emplace(fd, ep);
        }
        // stream back to pool
        photon::thread_yield_to(photon::CURRENT);
        auto node = new StreamListNode(ep, stream, expiration);
        fdmap.emplace(ep, node);
        lru.push_back(node);
        return true;
    }

    void collect() {
        int64_t fds[16];
        while (collector) {
            auto ret = ev->wait_for_events((void**)fds, 16, -1UL);
            for (int i = 0; i < ret; i++) {
                // since destructed socket should never become readable before
                // it have been acquired again
                // if it is readable or RDHUP, both condition should treat as
                // socket shutdown
                auto fd = fds[i];
                drop_from_pool(fd);
            }
        }
    }

    template <typename SockCTOR>
    ISocketStream* do_connect(EndPoint key, SockCTOR ctor) {
    again:
        auto it = fdmap.find(key);
        if (it == fdmap.end()) {
            ISocketStream* sock = ctor();
            if (sock) {
                return new PooledTCPSocket(sock, this, key);
            }
            return nullptr;
        } else {
            auto fd = it->second->stream->get_underlay_fd();
            if (fd >= 0) {
                fdep.erase(fd);
                ev->rm_interest({fd, EVENT_READ, (void*)(uint64_t)fd});
            }
            auto node = it->second;
            fdmap.erase(it);
            lru.erase(node);
            if (fd >= 0 && wait_for_fd_readable(fd, 0) == 0) {
                LOG_TEMP("destruct node");
                delete node;
                goto again;
            }
            auto ret = new PooledTCPSocket(node->stream.release(), this, key);
            delete node;
            return ret;
        }
    }

    virtual ISocketStream* connect(const EndPoint& ep) override {
        return do_connect(ep, [&] { return client->connect(ep); });
    }

    virtual ISocketStream* connect(const char* path, size_t count) override {
        LOG_ERROR_RETURN(ENOSYS, nullptr,
                         "Socket pool supports TCP-like socket only");
    }

    virtual Object* get_underlay_object(int level) override {
        return level ? client->get_underlay_object(level - 1) : nullptr;
    }

#define FORWARD_CLIENT_OP(act) return client->act;

    int getsockname(EndPoint& addr) override {
        FORWARD_CLIENT_OP(getsockname(addr));
    }
    int getpeername(EndPoint& addr) override {
        FORWARD_CLIENT_OP(getpeername(addr));
    }
    int getsockname(char* path, size_t count) override {
        FORWARD_CLIENT_OP(getsockname(path, count));
    }
    int getpeername(char* path, size_t count) override {
        FORWARD_CLIENT_OP(getpeername(path, count));
    }
    int setsockopt(int level, int option_name, const void* option_value,
                   socklen_t option_len) override {
        FORWARD_CLIENT_OP(
            setsockopt(level, option_name, option_value, option_len));
    }
    int getsockopt(int level, int option_name, void* option_value,
                   socklen_t* option_len) override {
        FORWARD_CLIENT_OP(
            getsockopt(level, option_name, option_value, option_len));
    }
    uint64_t timeout() override { FORWARD_CLIENT_OP(timeout()); }
    void timeout(uint64_t tm) override { FORWARD_CLIENT_OP(timeout(tm)); }
#undef FORWARD_CLIENT_OP
};

PooledTCPSocket::~PooledTCPSocket() {
    if (drop || !pool->release(ep, stream)) {
        delete stream;
    }
}

extern "C" ISocketClient* new_tcp_socket_pool(ISocketClient* client, uint64_t expire) {
    return new TCPSocketPool(client, expire);
}

}  // namespace net
}
