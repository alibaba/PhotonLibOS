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

#include "etsocket.h"

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <photon/common/alog-stdstring.h>
#include <photon/common/event-loop.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include "abstract_socket.h"
#include "basic_socket.h"
#include <photon/thread/thread.h>
#include "socket.h"

namespace photon {
namespace net {

static const int EOK = ENXIO;
enum class State {
    READABLE = 1,
    WRITABLE = 2,
    ERROR = 4
};
static photon::EventsMap<(int)State::READABLE, (int)State::WRITABLE, (int)State::ERROR> et_evmap(EPOLLIN | EPOLLRDHUP, EPOLLOUT, EPOLLERR);

struct NotifyContext {
    photon::thread *waiter[3] = {nullptr, nullptr, nullptr};

    int wait_for_state(State state, uint64_t timeout) {
        waiter[(int)state >> 1] = photon::CURRENT;
        auto ret = photon::thread_usleep(timeout);
        waiter[(int)state >> 1] = nullptr;
        auto eno = &errno;
        if (ret == 0)
        {
            *eno = ETIMEDOUT;
            return -1;
        }
        if (*eno != EOK)
        {
            LOG_DEBUG("failed when wait for events: ", ERRNO(*eno));
            return -1;
        }
        return 0;
    }

    void on_event(int ep_event) {
        auto ev = et_evmap.translate_bitwisely(ep_event);
        for (int i=0; i<3;i++) {
            if (waiter[i] && (ev & (1<<i))) {
                photon::thread_interrupt(waiter[i], EOK);
            }
        }
    }

    int wait_for_readable(uint64_t timeout) {
        return wait_for_state(State::READABLE, timeout);
    }

    int wait_for_writable(uint64_t timeout) {
        return wait_for_state(State::WRITABLE, timeout);
    }
};


struct ETPoller {
    int epfd = 0;
    EventLoop* loop = nullptr;

    int init() {
        epfd = epoll_create(1);
        loop = new_event_loop({this, &ETPoller::waiter},
                              {this, &ETPoller::notify});
        loop->async_run();
        return epfd;
    }
    int fini() {
        loop->stop();
        if (epfd > 0) {
            close(epfd);
            epfd = 0;
        }
        delete loop;
        loop = nullptr;
        return 0;
    }
    int waiter(EventLoop*) {
        auto ret = photon::wait_for_fd_readable(epfd, 1 * 1000UL);
        // LOG_DEBUG("WAITER ",VALUE(ret));
        auto err = errno;
        if (ret == 0)
            return 1;
        else if (err == ETIMEDOUT)
            return 0;
        else
            return -1;
    }
    int notify(EventLoop*) {
        struct epoll_event evs[1024];
        auto ret = epoll_wait(epfd, evs, 1024, 0);
        for (int i = 0; i < ret; i++) {
            auto c = (NotifyContext*)evs[i].data.ptr;
            c->on_event(evs[i].events);
        }
        return 0;
    }
    int register_notifier(int fd, NotifyContext* context) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
        ev.data.ptr = context;
        return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
    int unregister_notifier(int fd) {
        return epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    }
};

thread_local ETPoller etpoller;

int et_poller_init() { return etpoller.init(); }

int et_poller_fini() { return etpoller.fini(); }

static int et_fill_path(struct sockaddr_un& name, const char* path,
                        size_t count) {
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

class ETKernelSocket : public SocketBase, public NotifyContext {
public:
    int fd;
    bool m_autoremove = false;
    explicit ETKernelSocket(int fd) : fd(fd) {
        etpoller.register_notifier(fd, this);
    }
    ETKernelSocket(int socket_family, bool autoremove)
        : m_autoremove(autoremove) {
        fd = net::socket(socket_family, SOCK_STREAM, 0);
        if (fd > 0 && socket_family == AF_INET) {
            int val = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
        }
        etpoller.register_notifier(fd, this);
    }
    virtual ~ETKernelSocket() {
        if (fd <= 0) return;
        etpoller.unregister_notifier(fd);
        if (m_autoremove) {
            char filename[PATH_MAX];
            if (0 == getsockname(filename, PATH_MAX)) {
                unlink(filename);
            }
        }
        close();
    }
    virtual int close() override {
        auto ret = ::close(fd);
        fd = -1;
        return ret;
    }
    virtual Object* get_underlay_object(int) override { return (Object*)(uint64_t)fd; }
    typedef int (*Getter)(int sockfd, struct sockaddr* addr,
                          socklen_t* addrlen);

    int do_getname(net::EndPoint& addr, Getter getter) {
        struct sockaddr_in addr_in;
        socklen_t len = sizeof(addr_in);
        int ret = getter(fd, (struct sockaddr*)&addr_in, &len);
        if (ret < 0 || len != sizeof(addr_in)) return -1;
        addr.from_sockaddr_in(addr_in);
        return 0;
    }
    virtual int getsockname(net::EndPoint& addr) override {
        return do_getname(addr, &::getsockname);
    }
    virtual int getpeername(net::EndPoint& addr) override {
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

#define LAMBDA(expr) [&]() __INLINE__ { return expr; }
#define LAMBDA_TIMEOUT(expr)              \
    [&]() __INLINE__ {                    \
        Timeout __tmo(timeout);           \
        DEFER(timeout = __tmo.timeout()); \
        return expr;                      \
    }

template <typename IOCB>
__FORCE_INLINE__ ssize_t etdoio_n(void*& buf, size_t& count, IOCB iocb) {
    auto count0 = count;
    while (count > 0) {
        ssize_t ret = iocb();
        if (ret <= 0) return ret;
        (char*&)buf += ret;
        count -= ret;
    }
    return count0;
}

template <typename IOCB>
__FORCE_INLINE__ ssize_t etdoiov_n(iovector_view& v, IOCB iocb) {
    ssize_t count = 0;
    while (v.iovcnt > 0) {
        ssize_t ret = iocb();
        if (ret <= 0) return ret;
        count += ret;

        uint64_t bytes = ret;
        auto extracted = v.extract_front(bytes);
        assert(extracted == bytes);
        _unused(extracted);
    }
    return count;
}

template <typename IOCB, typename WAITCB>
static __FORCE_INLINE__ ssize_t etdoio(IOCB iocb, WAITCB waitcb) {
    while (true) {
        ssize_t ret = iocb();
        if (ret < 0) {
            auto e = errno;  // errno is usually a macro that expands to a
                             // function call
            if (e == EINTR) continue;
            if (e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS) {
                if (waitcb())  // non-zero result means timeout or
                               // interrupt, need to return
                    return ret;
                continue;
            }
        }
        return ret;
    }
}

class ETKernelSocketStream : public ETKernelSocket {
public:
    uint64_t m_timeout = -1;

    using ETKernelSocket::ETKernelSocket;
    virtual ~ETKernelSocketStream() {
        if (fd > 0) shutdown(ShutdownHow::ReadWrite);
    }
    virtual Object* get_underlay_object(int) override { return (Object*)(uint64_t)fd; }
    virtual ssize_t read(void* buf, size_t count) override {
        auto b = buf;
        return etdoio_n(b, count, LAMBDA(recv(b, count)));
    }
    virtual ssize_t write(const void* buf, size_t count) override {
        auto b = (void*)buf;
        return etdoio_n(b, count, LAMBDA(send(b, count)));
    }
    virtual ssize_t readv(const struct iovec* iov, int iovcnt) override {
        SmartCloneIOV<32> ciov(iov, iovcnt);
        return readv_mutable(ciov.ptr, iovcnt);
    }
    virtual ssize_t readv_mutable(struct iovec* iov, int iovcnt) override {
        iovector_view v(iov, iovcnt);
        return etdoiov_n(v, LAMBDA(recv((const struct iovec*)v.iov, v.iovcnt)));
    }
    virtual ssize_t writev(const struct iovec* iov, int iovcnt) override {
        SmartCloneIOV<32> ciov(iov, iovcnt);
        return writev_mutable(ciov.ptr, iovcnt);
    }
    virtual ssize_t writev_mutable(struct iovec* iov, int iovcnt) override {
        iovector_view v(iov, iovcnt);
        return etdoiov_n(v, LAMBDA(send((const struct iovec*)v.iov, v.iovcnt)));
    }
    virtual ssize_t recv(void* buf, size_t count) override {
        auto timeout = m_timeout;
        return etdoio(
            LAMBDA(::read(fd, buf, count)),
            LAMBDA_TIMEOUT(wait_for_readable(timeout)));
    }
    virtual ssize_t recv(const struct iovec* iov, int iovcnt) override {
        auto timeout = m_timeout;
        return etdoio(
            LAMBDA(::readv(fd, iov, iovcnt)),
            LAMBDA_TIMEOUT(wait_for_readable(timeout)));
    }
    ssize_t do_send(const void* buf, size_t count, int flags = 0) {
        auto timeout = m_timeout;
        return etdoio(
            LAMBDA(::send(fd, buf, count, MSG_NOSIGNAL | flags)),
            LAMBDA_TIMEOUT(wait_for_writable(timeout)));
    }
    virtual ssize_t send(const void* buf, size_t count) override {
        return do_send(buf, count);
    }
    ssize_t do_send(const struct iovec* iov, int iovcnt, int flags = 0) {
        auto timeout = m_timeout;
        return etdoio(
            [&] {
                struct msghdr msg {
                    0
                };
                msg.msg_iov = (iovec*)iov;
                msg.msg_iovlen = iovcnt;
                auto ret = ::sendmsg(fd, &msg, MSG_NOSIGNAL | flags);
                return ret;
            },
            LAMBDA_TIMEOUT(wait_for_writable(timeout)));
    }
    virtual ssize_t send(const struct iovec* iov, int iovcnt) override {
        return do_send(iov, iovcnt, 0);
    }
    virtual uint64_t timeout() override { return m_timeout; }
    virtual void timeout(uint64_t tm) override { m_timeout = tm; }
    virtual int shutdown(ShutdownHow how) override {
        // shutdown how defined as 0 for RD, 1 for WR and 2 for RDWR
        // in sys/socket.h, cast ShutdownHow into int just fits
        return ::shutdown(fd, static_cast<int>(how));
    }

    virtual ssize_t send2(const void* buf, size_t count, int flag) override {
        auto b = (void*)buf;
        return etdoio_n(b, count, LAMBDA(do_send(b, count, flag)));
    }
    virtual ssize_t send2(const struct iovec* iov, int iovcnt, int flag) override {
        iovector_view v((struct iovec*)iov, iovcnt);
        return etdoiov_n(v, LAMBDA(do_send((const struct iovec*)v.iov, v.iovcnt, flag)));
    }

    ssize_t do_sendfile(int in_fd, off_t offset, size_t count) {
        auto timeout = m_timeout;
        return etdoio(LAMBDA(::sendfile(this->fd, in_fd, &offset, count)),
                      LAMBDA_TIMEOUT(wait_for_writable(timeout)));
    }

    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        void* buf_unused = nullptr;
        return etdoio_n(buf_unused, count,
            LAMBDA(do_sendfile(in_fd, offset, count)));
    }
};

// factory class
class ETKernelSocketClient : public SocketBase {
public:
    int m_socket_family;
    bool m_autoremove = false;
    SockOptBuffer opts;
    uint64_t m_timeout = -1;

    ETKernelSocketClient(int socket_family, bool autoremove)
        : m_socket_family(socket_family), m_autoremove(autoremove) {}

    virtual int setsockopt(int level, int option_name, const void* option_value,
                           socklen_t option_len) override {
        return opts.put_opt(level, option_name, option_value, option_len);
    }
    virtual Object* get_underlay_object(int) override { return (Object*)-1UL; }
    ETKernelSocket* create_socket() {
        auto sock = new ETKernelSocketStream(m_socket_family, m_autoremove);
        if (sock->fd < 0)
            LOG_ERROR_RETURN(0, nullptr, "Failed to create socket fd");
        for (auto& opt : opts) {
            auto ret = sock->setsockopt(opt.level, opt.opt_name, opt.opt_val,
                                        opt.opt_len);
            if (ret < 0) {
                delete sock;
                LOG_ERROR_RETURN(EINVAL, nullptr, "Failed to setsockopt ",
                                 VALUE(opt.level), VALUE(opt.opt_name));
            }
        }
        sock->timeout(m_timeout);
        return sock;
    }

    int do_connect(ETKernelSocket* sock, const struct sockaddr* addr,
                   socklen_t addrlen, uint64_t timeout) {
        int err = 0;
        while (true) {
            int ret = ::connect(sock->fd, addr, addrlen);
            if (ret < 0) {
                auto e = errno;  // errno is usually a macro that expands to a
                                 // function call
                if (e == EINTR) {
                    err = 1;
                    continue;
                }
                if (e == EINPROGRESS || (e == EADDRINUSE && err == 1)) {
                    sock->wait_for_writable(timeout);
                    socklen_t n = sizeof(err);
                    ret =
                        ::getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &err, &n);
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

    virtual ISocketStream* connect(const EndPoint& ep) override {
        auto addr_in = ep.to_sockaddr_in();
        auto sock = create_socket();
        if (!sock) return nullptr;
        auto ret = do_connect(sock, (struct sockaddr*)&addr_in, sizeof(addr_in),
                              m_timeout);
        if (ret < 0) {
            delete sock;
            LOG_ERRNO_RETURN(0, nullptr, "Failed to connect to ", ep);
        }
        return sock;
    }
    virtual ISocketStream* connect(const char* path, size_t count) override {
        struct sockaddr_un addr_un;
        int ret = et_fill_path(addr_un, path, count);
        if (ret < 0) {
            LOG_ERROR_RETURN(0, nullptr, "Failed to fill uds addr");
        }
        auto sock = create_socket();
        if (!sock) return nullptr;
        ret = do_connect(sock, (struct sockaddr*)&addr_un, sizeof(addr_un),
                         m_timeout);
        if (ret < 0) return nullptr;
        return sock;
    }
    virtual uint64_t timeout() override { return m_timeout; }
    virtual void timeout(uint64_t tm) override { m_timeout = tm; }
};

class ETKernelSocketServer : public ETKernelSocket {
protected:
    Handler m_handler;

    photon::thread* workth;
    bool waiting;
    uint64_t m_timeout = -1UL;

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
                photon::thread_create11(&ETKernelSocketServer::handler, m_handler,
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
    explicit ETKernelSocketServer(int fd)
        : ETKernelSocket(fd), workth(nullptr) {}
    ETKernelSocketServer(int socket_family, bool autoremove)
        : ETKernelSocket(socket_family, autoremove), workth(nullptr) {}
    virtual ~ETKernelSocketServer() { terminate(); }

    virtual uint64_t timeout() override { return m_timeout; }
    virtual void timeout(uint64_t tm) override { m_timeout = tm; }
    virtual Object* get_underlay_object(int) override { return (Object*)(uint64_t)fd; }

    virtual int start_loop(bool block) override {
        if (workth) LOG_ERROR_RETURN(EALREADY, -1, "Already listening");
        if (block) return accept_loop();
        auto th =
            photon::thread_create11(&ETKernelSocketServer::accept_loop, this);
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

    bool is_socket(char* path) const {
        struct stat statbuf;
        if (0 == stat(path, &statbuf)) return S_ISSOCK(statbuf.st_mode) != 0;
        return false;
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
        int ret = et_fill_path(addr_un, path, count);
        if (ret < 0) return -1;
        return ::bind(fd, (struct sockaddr*)&addr_un, sizeof(addr_un));
    }
    virtual int listen(int backlog) override { return ::listen(fd, backlog); }

    int fdaccept(int fd, struct sockaddr* addr, socklen_t* addrlen,
                 uint64_t timeout) {
        return (int)etdoio(
            LAMBDA(::accept4(fd, addr, addrlen, SOCK_NONBLOCK)),
            LAMBDA_TIMEOUT(wait_for_readable(timeout)));
    }

    int do_accept() { return fdaccept(fd, nullptr, nullptr, m_timeout); }
    int do_accept(EndPoint& remote_endpoint) {
        struct sockaddr_in addr_in;
        socklen_t len = sizeof(addr_in);
        int cfd = fdaccept(fd, (struct sockaddr*)&addr_in, &len, m_timeout);
        if (cfd < 0 || len != sizeof(addr_in)) return -1;

        remote_endpoint.from_sockaddr_in(addr_in);
        return cfd;
    }
    virtual ISocketStream* accept(EndPoint* remote_endpoint) override {
        int cfd = remote_endpoint ? do_accept(*remote_endpoint) : do_accept();

        return cfd < 0 ? nullptr : new ETKernelSocketStream(cfd);
    }
    virtual ISocketStream* accept() override {
        int cfd = do_accept();
        return cfd < 0 ? nullptr : new ETKernelSocketStream(cfd);
    }
    bool is_socket(const char* path) const {
        struct stat statbuf;
        if (0 == stat(path, &statbuf)) return S_ISSOCK(statbuf.st_mode) != 0;
        return false;
    }
};

extern "C" ISocketClient* new_et_tcp_socket_client() {
    return new ETKernelSocketClient(AF_INET, false);
}
extern "C" ISocketServer* new_et_tcp_socket_server() {
    auto sock = new ETKernelSocketServer(AF_INET, false);
    if (sock->fd < 0) {
        delete sock;
        LOG_ERROR_RETURN(0, nullptr,
                         "Failed to create ET Socket Server socket");
    }
    return sock;
}
extern "C" ISocketStream* new_et_tcp_socket_stream(int fd) {
    if (fd <= 0) return nullptr;
    return new ETKernelSocketStream(fd);
}

}  // namespace net
}
