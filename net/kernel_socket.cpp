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
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/sendfile.h>
#endif
#include <unistd.h>
#include <memory>
#include <unordered_map>

#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/thread/list.h>
#include <photon/thread/timer.h>
#include <photon/common/estring.h>
#include <photon/common/utility.h>
#include <photon/common/event-loop.h>
#include <photon/net/basic_socket.h>
#include <photon/net/utils.h>
#ifdef PHOTON_URING
#include <photon/io/iouring-wrapper.h>
#endif
#ifdef ENABLE_FSTACK_DPDK
#include <photon/io/fstack-dpdk.h>
#endif

#include "base_socket.h"
#include "../io/events_map.h"

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif

#ifndef AF_SMC
#define AF_SMC 43
#endif

namespace photon {
namespace net {

static int socket(int family, int protocol = 0,
            bool nonblocking = true, bool nodelay = true) {
    int fd = ::socket(family, SOCK_STREAM, protocol);
    if (fd < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to create socket fd");
    if (nonblocking)
        set_fd_nonblocking(fd);
    if (nodelay) {
        int v = 1;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) < 0)
            LOG_WARN("failed to set TCP_NODELAY ", ERRNO());
    }
    return fd;
}

template<typename Stream> inline
Stream* new_stream(int family, int protocol = 0, bool nonblocking = true) {
    int fd = socket(family, protocol, nonblocking, family != AF_UNIX);
    return (fd < 0) ? nullptr : new Stream(fd);
}

class KernelSocketStream : public SocketStreamBase {
public:
    using ISocketStream::setsockopt;
    using ISocketStream::getsockopt;
    int fd = -1;
    explicit KernelSocketStream(int fd) : fd(fd) {}
    ~KernelSocketStream() override {
        if (fd < 0) return;
        shutdown(ShutdownHow::ReadWrite);
        close();
    }
    ssize_t read(void* buf, size_t count) override {
        Timeout timeout(m_timeout);
        return DOIO_LOOP(do_recv(fd, buf, count, 0, timeout), BufStep(buf, count));
    }
    ssize_t readv(const iovec* iov, int iovcnt) override {
        SmartCloneIOV<8> clone(iov, iovcnt);
        iovector_view view(clone.ptr, iovcnt);
        Timeout timeout(m_timeout);
        return DOIO_LOOP(do_recvmsg(fd, tmp_msg_hdr(view), 0, timeout), BufStepV(view));
    }
    ssize_t write(const void* buf, size_t count) override {
        Timeout timeout(m_timeout);
        return DOIO_LOOP(do_send(fd, buf, count, MSG_NOSIGNAL, timeout), BufStep((void*&)buf, count));
    }
    ssize_t writev(const iovec* iov, int iovcnt) override {
        SmartCloneIOV<8> clone(iov, iovcnt);
        iovector_view view(clone.ptr, iovcnt);
        Timeout timeout(m_timeout);
        return DOIO_LOOP(do_sendmsg(fd, tmp_msg_hdr(view), MSG_NOSIGNAL, timeout), BufStepV(view));
    }
    ssize_t recv(void* buf, size_t count, int flags = 0) override {
        return do_recv(fd, buf, count, flags, m_timeout);
    }
    ssize_t recv(const iovec* iov, int iovcnt, int flags = 0) override {
        return do_recvmsg(fd, tmp_msg_hdr(iov, iovcnt), flags, m_timeout);
    }
    ssize_t send(const void* buf, size_t count, int flags = 0) override {
        return do_send(fd, buf, count, flags | MSG_NOSIGNAL, m_timeout);
    }
    ssize_t send(const iovec* iov, int iovcnt, int flags = 0) override {
        return do_sendmsg(fd, tmp_msg_hdr(iov, iovcnt), flags | MSG_NOSIGNAL, m_timeout);
    }
    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        return net::sendfile_n(fd, in_fd, &offset, count);
    }
    int shutdown(ShutdownHow how) final {
        // shutdown how defined as 0 for RD, 1 for WR and 2 for RDWR
        // in sys/socket.h, cast ShutdownHow into int just fits
        return ::shutdown(fd, static_cast<int>(how));
    }
    Object* get_underlay_object(uint64_t recursion = 0) override {
        return (Object*) (uint64_t) fd;
    }
    int close() final {
        if (fd < 0) return 0;
        get_vcpu()->master_event_engine->wait_for_fd(fd, 0, -1UL);
        auto ret = ::close(fd);
        fd = -1;
        return ret;
    }
    int getsockname(EndPoint& addr) override {
        return get_socket_name(fd, addr);
    }
    int getpeername(EndPoint& addr) override {
        return get_peer_name(fd, addr);
    }
    int getsockname(char* path, size_t count) override {
        return get_socket_name(fd, path, count);
    }
    int getpeername(char* path, size_t count) override {
        return get_peer_name(fd, path, count);
    }
    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        return ::setsockopt(fd, level, option_name, option_value, option_len);
    }
    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        return ::getsockopt(fd, level, option_name, option_value, option_len);
    }
    uint64_t timeout() const override { return m_timeout; }
    void timeout(uint64_t tm) override { m_timeout = tm; }
protected:
    uint64_t m_timeout = -1;

    virtual ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, Timeout timeout) {
        return photon::net::send(sockfd, buf, count, flags, timeout);
    }
    virtual ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, Timeout timeout) {
        return photon::net::sendmsg(sockfd, message, flags, timeout);
    }
    virtual ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, Timeout timeout) {
        return photon::net::recv(sockfd, buf, count, flags, timeout);
    }
    virtual ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, Timeout timeout) {
        return photon::net::recvmsg(sockfd, message, flags, timeout);
    }

    struct tmp_msg_hdr : public ::msghdr {
        tmp_msg_hdr(const iovec* iov, int iovcnt) : ::msghdr{} {
            this->msg_iov = (iovec*) iov;
            this->msg_iovlen = iovcnt;
        }

        explicit tmp_msg_hdr(iovector_view& view) :
            tmp_msg_hdr(view.iov, view.iovcnt) { }

        operator ::msghdr*() { return this; }
    };
};

class KernelSocketClient : public SocketClientBase {
public:
    ISocketStream* connect(const char* path, size_t count) override {
        struct sockaddr_un addr_un;
        if (fill_uds_path(addr_un, path, count) != 0) return nullptr;
        sockaddr_storage r(addr_un);
        return do_connect(&r);
    }

    ISocketStream* connect(const EndPoint& remote, const EndPoint* local) override {
        sockaddr_storage r(remote);
        if (likely(!local || local->is_ipv4() != remote.is_ipv4())) {
            return do_connect(&r);
        }
        sockaddr_storage l(*local);
        return do_connect(&r, &l);
    }

    virtual KernelSocketStream* create_stream(int socket_family) {
        return new_stream<KernelSocketStream>(socket_family);
    }

    virtual int fd_connect(int fd, const sockaddr* remote, socklen_t addrlen) {
        return net::connect(fd, remote, addrlen, m_timeout);
    }

    ISocketStream* do_connect(const sockaddr_storage* remote,
                              const sockaddr_storage* local = nullptr) {
        auto stream = create_stream(remote->store.ss_family);
        auto deleter = [&](KernelSocketStream*) {
            auto errno_backup = errno;
            delete stream;
            errno = errno_backup;
        };
        std::unique_ptr<KernelSocketStream, decltype(deleter)> ptr(stream, deleter);
        if (!ptr || ptr->fd < 0) {
            LOG_ERROR_RETURN(0, nullptr, "Failed to create socket fd");
        }
        if (m_opts.setsockopt(ptr->fd) < 0) {
            return nullptr;
        }
        ptr->timeout(m_timeout);
        if (local != nullptr) {
            if (::bind(ptr->fd, local->get_sockaddr(), local->get_socklen()) != 0) {
                LOG_ERRNO_RETURN(0, nullptr, "fail to bind socket");
            }
        }
        auto ret = fd_connect(ptr->fd, remote->get_sockaddr(), remote->get_socklen());
        if (ret < 0) {
            LOG_ERRNO_RETURN(0, nullptr, "Failed to connect to ", remote->to_endpoint());
        }
        return ptr.release();
    }
};

class KernelSocketServer : public SocketServerBase {
public:
    using ISocketServer::setsockopt;
    using ISocketServer::getsockopt;

    explicit KernelSocketServer(bool autoremove = false) : m_autoremove(autoremove) { }

    int init() { return 0; }

    ~KernelSocketServer() {
        terminate();
        if (m_listen_fd < 0) {
            return;
        }
        ::shutdown(m_listen_fd, static_cast<int>(ShutdownHow::ReadWrite));
        if (m_autoremove) {
            char filename[PATH_MAX];
            if (0 == get_socket_name(m_listen_fd, filename, PATH_MAX)) {
                unlink(filename);
            }
        }
        ::close(m_listen_fd);
        m_listen_fd = -1;
    }

    int start_loop(bool block) override {
        if (workth) LOG_ERROR_RETURN(EALREADY, -1, "Already listening");
        m_block_serv = block;
        if (block) return accept_loop();
        auto loop = &KernelSocketServer::accept_loop;
        auto th = thread_create((thread_entry&)loop, this);
        thread_enable_join(th);
        thread_yield_to(th);
        return 0;
    }

    void terminate() final {
        if (!workth) return;
        auto th = workth;
        workth = nullptr;
        if (waiting) {
            thread_interrupt(th);
            if (!m_block_serv)
                thread_join((join_handle*)th);
        }
    }

    ISocketServer* set_handler(Handler handler) override {
        m_handler = handler;
        return this;
    }

    int bind(const EndPoint& ep) override {
        auto s = sockaddr_storage(ep);
        if (m_listen_fd < 0) {
            m_listen_fd = socket(s.get_sockaddr()->sa_family, 0, m_nonblocking, false);
            if (m_listen_fd < 0) return -1;
        }
        if (m_opts.setsockopt(m_listen_fd) != 0) {
            return -1;
        }
        int ret = ::bind(m_listen_fd, s.get_sockaddr(), s.get_socklen());
        if (ret < 0)
            LOG_ERRNO_RETURN(0, ret, "failed to bind to ", s.to_endpoint());
        return 0;
    }

    int bind(const char* path, size_t count) override {
        if (m_autoremove && is_socket(path)) {
            if (unlink(path) < 0)
                LOG_ERRNO_RETURN(0, -1, VALUE(path));
        }
        if (m_listen_fd < 0) {
            m_listen_fd = socket(AF_UNIX, 0, true, false);
            if (m_listen_fd < 0)
                LOG_ERRNO_RETURN(0, m_listen_fd, "failed to create UNIX domain socket at ", ALogString(path, count));
        }
        if (m_opts.setsockopt(m_listen_fd) != 0) {
            return -1;
        }
        struct sockaddr_un addr_un;
        int ret = fill_uds_path(addr_un, path, count);
        if (ret < 0) return -1;
        ret = ::bind(m_listen_fd, (struct sockaddr*) &addr_un, sizeof(addr_un));
        if (ret < 0)
            LOG_ERRNO_RETURN(0, ret, "failed to bind to '`' ", ALogString(path, count))
        return 0;
    }

    int listen(int backlog) override {
        return ::listen(m_listen_fd, backlog);
    }

    ISocketStream* accept(EndPoint* remote_endpoint = nullptr) override {
        int cfd = remote_endpoint ? do_accept(*remote_endpoint) : do_accept();
        if (cfd < 0) {
            return nullptr;
        }
        if (m_opts.setsockopt(cfd) != 0) {
            return nullptr;
        }
        return create_stream(cfd);
    }

    Object* get_underlay_object(uint64_t recursion = 0) override {
        return (Object*) (uint64_t) m_listen_fd;
    }

    int getsockname(EndPoint& addr) override {
        return get_socket_name(m_listen_fd, addr);
    }

    int getpeername(EndPoint& addr) override {
        return get_peer_name(m_listen_fd, addr);
    }

    int getsockname(char* path, size_t count) override {
        return get_socket_name(m_listen_fd, path, count);
    }

    int getpeername(char* path, size_t count) override {
        return get_peer_name(m_listen_fd, path, count);
    }

    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        if (m_listen_fd >= 0 && ::setsockopt(m_listen_fd, level, option_name, option_value, option_len) != 0)
            LOG_ERRNO_RETURN(0, -1, "failed to setsockopt for fd `", m_listen_fd);
        return m_opts.put_opt(level, option_name, option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        if (m_listen_fd >= 0 && ::getsockopt(m_listen_fd, level, option_name, option_value, option_len) == 0)
            return 0;
        return m_opts.get_opt(level, option_name, option_value, option_len);
    }

protected:
    bool m_autoremove;
    bool m_nonblocking = true;
    bool m_block_serv = false;
    bool waiting = false;
    Handler m_handler;
    photon::thread* workth = nullptr;
    int m_listen_fd = -1;

    virtual KernelSocketStream* create_stream(int fd) {
        return new KernelSocketStream(fd);
    }

    virtual int do_accept(struct sockaddr *addr, socklen_t *addrlen) {
        return net::accept(m_listen_fd, addr, addrlen);
    }

    int do_accept() { return do_accept(nullptr, nullptr); }

    int do_accept(EndPoint& remote_endpoint) {
        sockaddr_storage s;
        socklen_t len = s.get_max_socklen();
        int cfd = do_accept(s.get_sockaddr(), &len);
        if (cfd < 0 || len > s.get_max_socklen())
            return -1;
        remote_endpoint = s.to_endpoint();
        return cfd;
    }

    static bool is_socket(const char* path) {
        struct stat statbuf;
        return (0 == stat(path, &statbuf)) ? S_ISSOCK(statbuf.st_mode) : false;
    }

    int accept_loop() {
        if (workth) LOG_ERROR_RETURN(EALREADY, -1, "Already listening");
        workth = photon::CURRENT;
        DEFER(workth = nullptr);
        while (workth) {
            waiting = true;
            auto connection = accept();
            waiting = false;
            if (!workth) return 0;
            if (connection) {
                connection->timeout(m_timeout);
                photon::thread_create11(&KernelSocketServer::handler, m_handler, connection);
            } else {
                LOG_WARN("KernelSocketServer: failed to accept new connections: `", ERRNO());
                photon::thread_usleep(1000);
            }
        }
        return 0;
    }

    static void handler(Handler m_handler, ISocketStream* sess) {
        m_handler(sess);
        delete sess;
    }
};

class SMCSocketClient : public KernelSocketClient {
public:
    virtual KernelSocketStream* create_stream(int socket_family) {
        int ver = (socket_family == AF_INET6);
        return new_stream<KernelSocketStream>(AF_SMC, ver);
    }
};

class SMCSocketServer : public KernelSocketServer {
public:
    int bind(const EndPoint& ep) override {
        auto s = sockaddr_storage(ep);
        if (m_listen_fd < 0) {
            int ver = (s.get_sockaddr()->sa_family == AF_INET6);
            m_listen_fd = socket(AF_SMC, ver);
            if (m_listen_fd < 0) return -1;
        }
        int ret = ::bind(m_listen_fd, s.get_sockaddr(), s.get_socklen());
        if (ret < 0)
            LOG_ERRNO_RETURN(0, ret, "failed to bind to ", s.to_endpoint());
        return 0;
    }

    UNIMPLEMENTED(int bind(const char* path, size_t count));
};

#ifdef __linux__
class ZeroCopySocketStream : public KernelSocketStream {
public:
    explicit ZeroCopySocketStream(int fd) : KernelSocketStream(fd) {
        setsockopt<int>(SOL_SOCKET, SO_ZEROCOPY, 1);
    }

protected:
    uint32_t m_num_calls = 0;

    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, Timeout timeout) override {
        struct iovec iov{const_cast<void*>(buf), count};
        return do_sendmsg(sockfd, tmp_msg_hdr(&iov, 1), flags, timeout);
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, Timeout timeout) override {
        ssize_t n = photon::net::sendmsg(sockfd, message, flags | ZEROCOPY_FLAG, timeout);
        m_num_calls++;
        auto ret = zerocopy_confirm(sockfd, m_num_calls - 1, timeout);
        if (ret < 0)
            return ret;
        return n;
    }
};

class ZeroCopySocketServer : public KernelSocketServer {
public:
    using KernelSocketServer::KernelSocketServer;

    int init() {
        if (!net::zerocopy_available()) {
            LOG_ERROR_RETURN(0, -1, "zerocopy not available");
        }
        // if (KernelSocketServer::init() != 0) {
        //     return -1;
        // }
        return 0;
    }

protected:
    KernelSocketStream* create_stream(int fd) override {
        return new ZeroCopySocketStream(fd);
    }
};

class ZeroCopySocketClient : public KernelSocketClient {
public:
    using KernelSocketClient::KernelSocketClient;

protected:
    KernelSocketStream* create_stream(int socket_family) override {
        return new_stream<ZeroCopySocketStream>(socket_family);
    }
};

#ifdef PHOTON_URING

class IouringSocketStream : public KernelSocketStream {
public:
    using KernelSocketStream::KernelSocketStream;

    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, Timeout timeout) override {
        if (flags & ZEROCOPY_FLAG)
            return photon::iouring_send_zc(sockfd, buf, count, flags | MSG_WAITALL, timeout);
        else
            return photon::iouring_send(sockfd, buf, count, flags, timeout);
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, Timeout timeout) override {
        if (flags & ZEROCOPY_FLAG)
            return photon::iouring_sendmsg_zc(sockfd, message, flags | MSG_WAITALL, timeout);
        else
            return photon::iouring_sendmsg(sockfd, message, flags, timeout);
    }

    ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, Timeout timeout) override {
        return photon::iouring_recv(sockfd, buf, count, flags, timeout);
    }

    ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, Timeout timeout) override {
        return photon::iouring_recvmsg(sockfd, message, flags, timeout);
    }
};

class IouringSocketClient : public KernelSocketClient {
public:
    using KernelSocketClient::KernelSocketClient;

    KernelSocketStream* create_stream(int socket_family) override {
        return new_stream<IouringSocketStream>(socket_family, 0, false);
    }

    int fd_connect(int fd, const sockaddr* remote, socklen_t addrlen) override {
        return photon::iouring_connect(fd, remote, addrlen, m_timeout);
    }
};

class IouringSocketServer : public KernelSocketServer {
public:
    using KernelSocketServer::KernelSocketServer;

    int init() {
        m_nonblocking = false;
        return 0;
    }

    KernelSocketStream* create_stream(int fd) override {
        return new IouringSocketStream(fd);
    }

    int do_accept(struct sockaddr* addr, socklen_t* addrlen) override {
        return photon::iouring_accept(m_listen_fd, addr, addrlen, -1);
    }
};

class IouringFixedFileSocketStream : public IouringSocketStream {
public:
    explicit IouringFixedFileSocketStream(int fd) :
            IouringSocketStream(fd) {
        if (fd >= 0)
            photon::iouring_register_files(fd);
    }

    ~IouringFixedFileSocketStream() override {
        if (fd >= 0)
            photon::iouring_unregister_files(fd);
    }

private:
    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, Timeout timeout) override {
        if (flags & ZEROCOPY_FLAG)
            return photon::iouring_send_zc(sockfd, buf, count, IouringFixedFileFlag | flags | MSG_WAITALL, timeout);
        else
            return photon::iouring_send(sockfd, buf, count, IouringFixedFileFlag | flags, timeout);
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, Timeout timeout) override {
        if (flags & ZEROCOPY_FLAG)
            return photon::iouring_sendmsg_zc(sockfd, message, IouringFixedFileFlag | flags | MSG_WAITALL, timeout);
        else
            return photon::iouring_sendmsg(sockfd, message, IouringFixedFileFlag | flags, timeout);
    }

    ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, Timeout timeout) override {
        return photon::iouring_recv(sockfd, buf, count, IouringFixedFileFlag | flags, timeout);
    }

    ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, Timeout timeout) override {
        return photon::iouring_recvmsg(sockfd, message, IouringFixedFileFlag | flags, timeout);
    }
};

class IouringFixedFileSocketClient : public IouringSocketClient {
protected:
    using IouringSocketClient::IouringSocketClient;

    KernelSocketStream* create_stream(int socket_family) override {
        return new_stream<IouringFixedFileSocketStream>(socket_family, 0, false);
    }
};

class IouringFixedFileSocketServer : public IouringSocketServer {
protected:
    using IouringSocketServer::IouringSocketServer;

    KernelSocketStream* create_stream(int fd) override {
        return new IouringFixedFileSocketStream(fd);
    }
};

#endif // PHOTON_URING

#ifdef ENABLE_FSTACK_DPDK

class FstackDpdkSocketStream : public KernelSocketStream {
public:
    using KernelSocketStream::KernelSocketStream;

    FstackDpdkSocketStream(int socket_family, bool nonblocking) {
        fd = fstack_socket(socket_family, SOCK_STREAM, 0);
        if (fd < 0) return;
        setsockopt<int>(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    }

    ~FstackDpdkSocketStream() override {
        if (fd < 0) return;
        fstack_shutdown(fd, (int) ShutdownHow::ReadWrite);
        fstack_close(fd);
        fd = -1;
    }

protected:
    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, Timeout timeout) override {
        return fstack_send(sockfd, buf, count, flags, timeout);
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, Timeout timeout) override {
        return fstack_sendmsg(sockfd, message, flags, timeout);
    }

    ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, Timeout timeout) override {
        return fstack_recv(sockfd, buf, count, flags, timeout);
    }

    ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, Timeout timeout) override {
        return fstack_recvmsg(sockfd, message, flags, timeout);
    }

    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        return fstack_setsockopt(fd, level, option_name, option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        return fstack_getsockopt(fd, level, option_name, option_value, option_len);
    }
};

class FstackDpdkSocketClient : public KernelSocketClient {
protected:
    using KernelSocketClient::KernelSocketClient;

    KernelSocketStream* create_stream(int socket_family) override {
        return new FstackDpdkSocketStream(socket_family, true);
    }

    int fd_connect(int fd, const sockaddr* remote, socklen_t addrlen) override {
        return fstack_connect(fd, remote, addrlen, m_timeout);
    }
};

class FstackDpdkSocketServer : public KernelSocketServer {
public:
    using KernelSocketServer::KernelSocketServer;

    int bind(const EndPoint& ep) override {
        if (m_listen_fd >= 0)
            LOG_ERROR_RETURN(EALREADY, -1, "already bound");
        auto s = sockaddr_storage(ep);
        m_listen_fd = fstack_socket(s.get_sockaddr()->sa_family, SOCK_STREAM, 0);   // already non-blocking and no-delay
        if (m_listen_fd < 0) {
            LOG_ERRNO_RETURN(0, -1, "fail to setup DPDK listen fd");
        }
        int ret = fstack_bind(m_listen_fd, s.get_sockaddr(), s.get_socklen());
        if (ret < 0)
            LOG_ERRNO_RETURN(0, ret, "failed to bind to ", s.to_endpoint());
        return 0;
    }

    int bind(const char* path, size_t count) override {
        LOG_ERRNO_RETURN(ENOSYS, -1, "Not implemented");
    }

    int listen(int backlog) override {
        return fstack_listen(m_listen_fd, backlog);
    }

    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        if (fstack_setsockopt(m_listen_fd, level, option_name, option_value, option_len) != 0) {
            LOG_ERRNO_RETURN(EINVAL, -1, "failed to setsockopt");
        }
        return m_opts.put_opt(level, option_name, option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        if (fstack_getsockopt(m_listen_fd, level, option_name, option_value, option_len) == 0)
            return 0;
        return m_opts.get_opt(level, option_name, option_value, option_len);
    }

protected:
    KernelSocketStream* create_stream(int fd) override {
        return new FstackDpdkSocketStream(fd);
    }

    int do_accept(struct sockaddr* addr, socklen_t* addrlen) override {
        return fstack_accept(m_listen_fd, addr, addrlen, m_timeout);
    }

private:
    class FstackSockOptBuffer : public SockOptBuffer {
    public:
        int setsockopt(int fd) override {
            for (auto& opt : *this) {
                if (fstack_setsockopt(fd, opt.level, opt.opt_name, opt.opt_val, opt.opt_len) != 0) {
                    LOG_ERRNO_RETURN(EINVAL, -1, "Failed to setsockopt ",
                                     VALUE(opt.level), VALUE(opt.opt_name), VALUE(opt.opt_val));
                }
            }
            return 0;
        }
    };

    FstackSockOptBuffer m_opts;
};

#endif // ENABLE_FSTACK_DPDK

/* ET Socket - Start */

constexpr static photon::EventsMap<
    EVUnderlay<EVENT_READ, EVENT_WRITE, EVENT_ERROR>,
    EVKey<EPOLLIN | EPOLLRDHUP, EPOLLOUT, EPOLLERR>>
    et_evmap;

struct NotifyContext {
    photon::thread* waiter[3] = {nullptr, nullptr, nullptr};

    int wait_for_state(uint32_t event, uint64_t timeout) {
        waiter[event >> 1] = photon::CURRENT;
        auto ret = photon::thread_usleep(timeout);
        waiter[event >> 1] = nullptr;
        auto eno = &errno;
        if (ret == 0) {
            *eno = ETIMEDOUT;
            return -1;
        }
        if (*eno != EOK) {
            LOG_DEBUG("failed when wait for events: ", ERRNO(* eno));
            return -1;
        }
        return 0;
    }

    void on_event(int ep_event) {
        auto ev = et_evmap.translate_bitwisely(ep_event);
        for (int i = 0; i < 3; i++) {
            if (waiter[i] && (ev & (1 << i))) {
                photon::thread_interrupt(waiter[i], EOK);
            }
        }
    }

    int wait_for_readable(uint64_t timeout) {
        return wait_for_state(EVENT_READ, timeout);
    }

    int wait_for_writable(uint64_t timeout) {
        return wait_for_state(EVENT_WRITE, timeout);
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
            auto c = (NotifyContext*) evs[i].data.ptr;
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

template<typename IOCB, typename WAITCB>
static __FORCE_INLINE__ ssize_t etdoio(IOCB iocb, WAITCB waitcb) {
    while (true) {
        ssize_t ret = iocb();
        if (ret < 0) {
            auto e = errno;  // errno is usually a macro that expands to a function call
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

class ETKernelSocketStream : public KernelSocketStream, public NotifyContext {
public:
    explicit ETKernelSocketStream(int fd) : KernelSocketStream(fd) {
        if (fd >= 0) etpoller.register_notifier(fd, this);
    }

    ~ETKernelSocketStream() {
        if (fd >= 0) etpoller.unregister_notifier(fd);
    }

    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        return DOIO_LOOP(do_sendfile(in_fd, offset, count), BufStep(count));
    }

private:
    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, Timeout timeout) override {
        return etdoio(LAMBDA(::send(sockfd, buf, count, flags)),
                      LAMBDA_TIMEOUT(wait_for_writable(timeout)));
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, Timeout timeout) override {
        return etdoio(LAMBDA(::sendmsg(sockfd, message, flags)),
                      LAMBDA_TIMEOUT(wait_for_writable(timeout)));
    }

    ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, Timeout timeout) override {
        return etdoio(LAMBDA(::read(sockfd, buf, count)),
                      LAMBDA_TIMEOUT(wait_for_readable(timeout)));
    }

    ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, Timeout timeout) override {
        return etdoio(LAMBDA(::recvmsg(sockfd, message, flags)),
                      LAMBDA_TIMEOUT(wait_for_readable(timeout)));
    }

    ssize_t do_sendfile(int in_fd, off_t offset, size_t count) {
        auto timeout = m_timeout;
        return etdoio(LAMBDA(::sendfile(this->fd, in_fd, &offset, count)),
                      LAMBDA_TIMEOUT(wait_for_writable(timeout)));
    }
};

class ETKernelSocketClient : public KernelSocketClient {
public:
    using KernelSocketClient::KernelSocketClient;

protected:
    KernelSocketStream* create_stream(int socket_family) override {
        return new_stream<ETKernelSocketStream>(socket_family);
    }
};

class ETKernelSocketServer : public KernelSocketServer, public NotifyContext {
public:
    using KernelSocketServer::KernelSocketServer;

    ~ETKernelSocketServer() {
        if (m_listen_fd >= 0) etpoller.unregister_notifier(m_listen_fd);
    }

    int bind(const EndPoint& ep) override {
        int fd = m_listen_fd;
        int ret = KernelSocketServer::bind(ep);
        if (fd < 0 && m_listen_fd >= 0)
            etpoller.register_notifier(m_listen_fd, this);
        return ret;
    }

    int bind(const char* path, size_t count) override {
        int fd = m_listen_fd;
        int ret = KernelSocketServer::bind(path, count);
        if (fd < 0 && m_listen_fd >= 0)
            etpoller.register_notifier(m_listen_fd, this);
        return ret;
    }

protected:
    KernelSocketStream* create_stream(int fd) override {
        return new ETKernelSocketStream(fd);
    }

    int do_accept(struct sockaddr* addr, socklen_t* addrlen) override {
        uint64_t timeout = -1;
        return (int) etdoio(LAMBDA(::accept4(m_listen_fd, addr, addrlen, SOCK_NONBLOCK)),
                            LAMBDA_TIMEOUT(wait_for_readable(timeout)));
    }
};

#endif // __linux__

/* ET Socket - End */

extern "C" ISocketClient* new_tcp_socket_client() {
    return new KernelSocketClient();
}
extern "C" ISocketServer* new_tcp_socket_server() {
    return NewObj<KernelSocketServer>()->init();
}
extern "C" ISocketClient* new_uds_client() {
    return new KernelSocketClient();
}
extern "C" ISocketServer* new_uds_server(bool autoremove) {
    return NewObj<KernelSocketServer>(autoremove)->init();
}
#ifdef __linux__
extern "C" ISocketServer* new_zerocopy_tcp_server() {
    return NewObj<ZeroCopySocketServer>()->init();
}
extern "C" ISocketClient* new_zerocopy_tcp_client() {
    return new ZeroCopySocketClient();
}
#ifdef PHOTON_URING
extern "C" ISocketClient* new_iouring_tcp_client() {
    if (photon::iouring_register_files_enabled())
        return new IouringFixedFileSocketClient();
    else
        return new IouringSocketClient();
}
extern "C" ISocketServer* new_iouring_tcp_server() {
    if (photon::iouring_register_files_enabled())
        return NewObj<IouringFixedFileSocketServer>()->init();
    else
        return NewObj<IouringSocketServer>()->init();
}
#endif // PHOTON_URING
extern "C" ISocketClient* new_et_tcp_socket_client() {
    return new ETKernelSocketClient();
}
extern "C" ISocketServer* new_et_tcp_socket_server() {
    return NewObj<ETKernelSocketServer>()->init();
}
extern "C" ISocketClient* new_smc_socket_client() {
    return new SMCSocketClient();
}
extern "C" ISocketServer* new_smc_socket_server() {
    return NewObj<SMCSocketServer>()->init();
}
#ifdef ENABLE_FSTACK_DPDK
extern "C" ISocketClient* new_fstack_dpdk_socket_client() {
    return new FstackDpdkSocketClient();
}
extern "C" ISocketServer* new_fstack_dpdk_socket_server() {
    return NewObj<FstackDpdkSocketServer>()->init();
}
#endif // ENABLE_FSTACK_DPDK
#endif // __linux__

////////////////////////////////////////////////////////////////////////////////

/* Implementations in socket.h */

EndPoint::EndPoint(const char* _ep) {
    estring_view ep(_ep);
    auto pos = ep.find_last_of(':');
    if (pos == 0 || pos == estring::npos)
        return;
    auto port_str = ep.substr(pos + 1);
    if (!port_str.all_digits())
        return;
    auto _port = port_str.to_uint64();
    if (_port > UINT16_MAX)
        return;
    port = (uint16_t)_port;
    auto ipsv = (ep[0] == '[') ? ep.substr(1, pos - 2) : ep.substr(0, pos);
    if (ipsv.length() >= INET6_ADDRSTRLEN - 1)
        return;
    char ip_str[INET6_ADDRSTRLEN];
    memcpy(ip_str, ipsv.data(), ipsv.length());
    ip_str[ipsv.length()] = '\0';
    addr = IPAddr(ip_str);
}

LogBuffer& operator<<(LogBuffer& log, const IPAddr& addr) {
    if (addr.is_ipv4())
        return log.printf(addr.a, '.', addr.b, '.', addr.c, '.', addr.d);
    else {
        if (log.size < INET6_ADDRSTRLEN)
            return log;
        inet_ntop(AF_INET6, &addr.addr, log.ptr, INET6_ADDRSTRLEN);
        log.consume(strlen(log.ptr));
        return log;
    }
}

LogBuffer& operator<<(LogBuffer& log, const EndPoint& ep) {
    if (ep.is_ipv4())
        return log << ep.addr << ':' << ep.port;
    else
        return log << '[' << ep.addr << "]:" << ep.port;
}

}
}

LogBuffer& operator<<(LogBuffer& log, const in_addr& iaddr) {
    return log << photon::net::IPAddr(iaddr);
}
LogBuffer& operator<<(LogBuffer& log, const in6_addr& iaddr) {
    return log << photon::net::IPAddr(iaddr);
}
LogBuffer& operator<<(LogBuffer& log, const sockaddr_in& addr) {
    return log << photon::net::sockaddr_storage(addr).to_endpoint();
}
LogBuffer& operator<<(LogBuffer& log, const sockaddr_in6& addr) {
    return log << photon::net::sockaddr_storage(addr).to_endpoint();
}
