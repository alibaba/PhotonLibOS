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

class KernelSocketStream : public SocketStreamBase {
public:
    using ISocketStream::setsockopt;
    using ISocketStream::getsockopt;
    int fd = -1;
    explicit KernelSocketStream(int fd) : fd(fd) {}
    KernelSocketStream(int socket_family, bool nonblocking) {
        if (fd >= 0)
            return;
        if (nonblocking) {
            fd = net::socket(socket_family, SOCK_STREAM, 0);
        } else {
            fd = ::socket(socket_family, SOCK_STREAM, 0);
        }
        if (fd >= 0 && (socket_family == AF_INET || socket_family == AF_INET6)) {
            int val = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, (socklen_t) sizeof(val));
        }
    }
    ~KernelSocketStream() override {
        if (fd < 0) return;
        shutdown(ShutdownHow::ReadWrite);
        close();
    }
    ssize_t read(void* buf, size_t count) override {
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(do_recv(fd, buf, count, 0, timeout));
        return net::doio_n(buf, count, cb);
    }
    ssize_t readv(const iovec* iov, int iovcnt) override {
        SmartCloneIOV<8> clone(iov, iovcnt);
        iovector_view view(clone.ptr, iovcnt);
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(do_recvmsg(fd, tmp_msg_hdr(view), 0, timeout));
        return net::doiov_n(view, cb);
    }
    ssize_t write(const void* buf, size_t count) override {
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(do_send(fd, buf, count, MSG_NOSIGNAL, timeout));
        return net::doio_n((void*&) buf, count, cb);
    }
    ssize_t writev(const iovec* iov, int iovcnt) override {
        SmartCloneIOV<8> clone(iov, iovcnt);
        iovector_view view(clone.ptr, iovcnt);
        uint64_t timeout = m_timeout;
        auto cb = LAMBDA_TIMEOUT(do_sendmsg(fd, tmp_msg_hdr(view), MSG_NOSIGNAL, timeout));
        return net::doiov_n(view, cb);
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

    virtual ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, uint64_t timeout) {
        return photon::net::send(sockfd, buf, count, flags, timeout);
    }
    virtual ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, uint64_t timeout) {
        return photon::net::sendmsg(sockfd, message, flags, timeout);
    }
    virtual ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, uint64_t timeout) {
        return photon::net::recv(sockfd, buf, count, flags, timeout);
    }
    virtual ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, uint64_t timeout) {
        return photon::net::recvmsg(sockfd, message, flags, timeout);
    }

    struct tmp_msg_hdr : public ::msghdr {
        tmp_msg_hdr(const iovec* iov, int iovcnt) : ::msghdr{} {
            this->msg_iov = (iovec*) iov;
            this->msg_iovlen = iovcnt;
        }

        explicit tmp_msg_hdr(iovector_view& view) : tmp_msg_hdr(view.iov, view.iovcnt) {}

        operator ::msghdr*() {
            return this;
        }
    };
};

class KernelSocketClient : public SocketClientBase {
public:
    KernelSocketClient(bool nonblocking) : m_nonblocking(nonblocking) {}

    ISocketStream* connect(const char* path, size_t count) override {
        struct sockaddr_un addr_un;
        if (fill_uds_path(addr_un, path, count) != 0) return nullptr;
        return do_connect((const sockaddr*) &addr_un, sizeof(addr_un));
    }

    ISocketStream* connect(EndPoint remote, EndPoint local = EndPoint()) override {
        sockaddr_storage r(remote);
        if (local.undefined()) {
            return do_connect(r.get_sockaddr(), r.get_socklen());
        }
        sockaddr_storage l(local);
        return do_connect(r.get_sockaddr(), r.get_socklen(), l.get_sockaddr(), l.get_socklen());
    }

protected:
    bool m_nonblocking;

    virtual KernelSocketStream* create_stream(int socket_family) {
        return new KernelSocketStream(socket_family, m_nonblocking);
    }

    virtual int fd_connect(int fd, const sockaddr* remote, socklen_t addrlen) {
        return net::connect(fd, remote, addrlen, m_timeout);
    }

    ISocketStream* do_connect(const sockaddr* remote, socklen_t len_remote,
                              const sockaddr* local = nullptr, socklen_t len_local = 0) {
        auto stream = create_stream(remote->sa_family);
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
            if (::bind(ptr->fd, local, len_local) != 0) {
                LOG_ERRNO_RETURN(0, nullptr, "fail to bind socket");
            }
        }
        auto ret = fd_connect(ptr->fd, remote, len_remote);
        if (ret < 0) {
            LOG_ERRNO_RETURN(0, nullptr, "Failed to connect socket");
        }
        return ptr.release();
    }
};

class KernelSocketServer : public SocketServerBase {
public:
    using ISocketServer::setsockopt;
    using ISocketServer::getsockopt;

    KernelSocketServer(int socket_family, bool autoremove, bool nonblocking) :
            m_socket_family(socket_family),
            m_autoremove(autoremove),
            m_nonblocking(nonblocking) {
    }

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

    // Comply with the NewObj interface.
    virtual int init() {
        if (m_nonblocking) {
            m_listen_fd = net::socket(m_socket_family, SOCK_STREAM, 0);
        } else {
            m_listen_fd = ::socket(m_socket_family, SOCK_STREAM, 0);
        }
        if (m_listen_fd < 0) {
            LOG_ERRNO_RETURN(0, -1, "fail to setup listen fd");
        }
        if (m_socket_family == AF_INET || m_socket_family == AF_INET6) {
            if (setsockopt<int>(IPPROTO_TCP, TCP_NODELAY, 1) != 0) {
                LOG_ERRNO_RETURN(EINVAL, -1, "failed to setsockopt of TCP_NODELAY");
            }
        }
        return 0;
    }

    int start_loop(bool block) override {
        if (workth) LOG_ERROR_RETURN(EALREADY, -1, "Already listening");
        m_block = block;
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
            if (!m_block)
                thread_join((join_handle*)th);
        }
    }

    ISocketServer* set_handler(Handler handler) override {
        m_handler = handler;
        return this;
    }

    int bind(uint16_t port, IPAddr addr) override {
        if (m_socket_family == AF_INET6 && addr.undefined()) {
            addr = IPAddr::V6Any();
        }
        sockaddr_storage s(EndPoint(addr, port));
        return ::bind(m_listen_fd, s.get_sockaddr(), s.get_socklen());
    }

    int bind(const char* path, size_t count) override {
        if (m_autoremove && is_socket(path)) {
            unlink(path);
        }
        struct sockaddr_un addr_un;
        int ret = fill_uds_path(addr_un, path, count);
        if (ret < 0) return -1;
        return ::bind(m_listen_fd, (struct sockaddr*) &addr_un, sizeof(addr_un));
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
        if (::setsockopt(m_listen_fd, level, option_name, option_value, option_len) != 0) {
            LOG_ERRNO_RETURN(EINVAL, -1, "failed to setsockopt");
        }
        return m_opts.put_opt(level, option_name, option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        if (::getsockopt(m_listen_fd, level, option_name, option_value, option_len) == 0) return 0;
        return m_opts.get_opt(level, option_name, option_value, option_len);
    }

protected:
    int m_socket_family;
    bool m_autoremove;
    bool m_nonblocking;

    Handler m_handler;
    photon::thread* workth = nullptr;
    bool m_block = false;
    bool waiting = false;
    int m_listen_fd = -1;

    virtual KernelSocketStream* create_stream(int fd) {
        return new KernelSocketStream(fd);
    }

    virtual int fd_accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
        return net::accept(fd, addr, addrlen);
    }

    int do_accept() { return fd_accept(m_listen_fd, nullptr, nullptr); }

    int do_accept(EndPoint& remote_endpoint) {
        sockaddr_storage s;
        socklen_t len = s.get_max_socklen();
        int cfd = fd_accept(m_listen_fd, s.get_sockaddr(), &len);
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
            auto sess = accept();
            waiting = false;
            if (!workth) return 0;
            if (sess) {
                sess->timeout(m_timeout);
                photon::thread_create11(&KernelSocketServer::handler, m_handler, sess);
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

#ifdef __linux__
class ZeroCopySocketStream : public KernelSocketStream {
public:
    explicit ZeroCopySocketStream(int fd) : KernelSocketStream(fd) {
        setsockopt<int>(SOL_SOCKET, SO_ZEROCOPY, 1);
    }

    ZeroCopySocketStream(int socket_family, bool nonblocking) :
            KernelSocketStream(socket_family, nonblocking) {
        setsockopt<int>(SOL_SOCKET, SO_ZEROCOPY, 1);
    }

protected:
    uint32_t m_num_calls = 0;

    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, uint64_t timeout) override {
        struct iovec iov{const_cast<void*>(buf), count};
        return do_sendmsg(sockfd, tmp_msg_hdr(&iov, 1), flags, timeout);
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, uint64_t timeout) override {
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

    int init() override {
        if (!net::zerocopy_available()) {
            LOG_ERROR_RETURN(0, -1, "zerocopy not available");
        }
        if (KernelSocketServer::init() != 0) {
            return -1;
        }
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
        return new ZeroCopySocketStream(socket_family, m_nonblocking);
    }
};

#ifdef PHOTON_URING

class IouringSocketStream : public KernelSocketStream {
public:
    using KernelSocketStream::KernelSocketStream;

protected:
    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, uint64_t timeout) override {
        if (flags & ZEROCOPY_FLAG)
            return photon::iouring_send_zc(sockfd, buf, count, flags | MSG_WAITALL, timeout);
        else
            return photon::iouring_send(sockfd, buf, count, flags, timeout);
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, uint64_t timeout) override {
        if (flags & ZEROCOPY_FLAG)
            return photon::iouring_sendmsg_zc(sockfd, message, flags | MSG_WAITALL, timeout);
        else
            return photon::iouring_sendmsg(sockfd, message, flags, timeout);
    }

    ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, uint64_t timeout) override {
        return photon::iouring_recv(sockfd, buf, count, flags, timeout);
    }

    ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, uint64_t timeout) override {
        return photon::iouring_recvmsg(sockfd, message, flags, timeout);
    }
};

class IouringSocketClient : public KernelSocketClient {
public:
    using KernelSocketClient::KernelSocketClient;

protected:
    KernelSocketStream* create_stream(int socket_family) override {
        return new IouringSocketStream(socket_family, m_nonblocking);
    }

    int fd_connect(int fd, const sockaddr* remote, socklen_t addrlen) override {
        return photon::iouring_connect(fd, remote, addrlen, m_timeout);
    }
};

class IouringSocketServer : public KernelSocketServer {
public:
    using KernelSocketServer::KernelSocketServer;

protected:
    KernelSocketStream* create_stream(int fd) override {
        return new IouringSocketStream(fd);
    }

    int fd_accept(int fd, struct sockaddr* addr, socklen_t* addrlen) override {
        return photon::iouring_accept(fd, addr, addrlen, -1);
    }
};

class IouringFixedFileSocketStream : public IouringSocketStream {
public:
    explicit IouringFixedFileSocketStream(int fd) :
            IouringSocketStream(fd) {
        if (fd >= 0)
            photon::iouring_register_files(fd);
    }

    IouringFixedFileSocketStream(int socket_family, bool nonblocking) :
            IouringSocketStream(socket_family, nonblocking) {
        if (fd >= 0)
            photon::iouring_register_files(fd);
    }

    ~IouringFixedFileSocketStream() override {
        if (fd >= 0)
            photon::iouring_unregister_files(fd);
    }

private:
    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, uint64_t timeout) override {
        if (flags & ZEROCOPY_FLAG)
            return photon::iouring_send_zc(sockfd, buf, count, IouringFixedFileFlag | flags | MSG_WAITALL, timeout);
        else
            return photon::iouring_send(sockfd, buf, count, IouringFixedFileFlag | flags, timeout);
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, uint64_t timeout) override {
        if (flags & ZEROCOPY_FLAG)
            return photon::iouring_sendmsg_zc(sockfd, message, IouringFixedFileFlag | flags | MSG_WAITALL, timeout);
        else
            return photon::iouring_sendmsg(sockfd, message, IouringFixedFileFlag | flags, timeout);
    }

    ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, uint64_t timeout) override {
        return photon::iouring_recv(sockfd, buf, count, IouringFixedFileFlag | flags, timeout);
    }

    ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, uint64_t timeout) override {
        return photon::iouring_recvmsg(sockfd, message, IouringFixedFileFlag | flags, timeout);
    }
};

class IouringFixedFileSocketClient : public IouringSocketClient {
protected:
    using IouringSocketClient::IouringSocketClient;

    KernelSocketStream* create_stream(int socket_family) override {
        return new IouringFixedFileSocketStream(socket_family, m_nonblocking);
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

    FstackDpdkSocketStream(int socket_family, bool nonblocking) : KernelSocketStream(socket_family, nonblocking) {
        fd = fstack_socket(socket_family, SOCK_STREAM, 0);
        if (fd < 0)
            return;
        if (socket_family == AF_INET) {
            int val = 1;
            fstack_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, (socklen_t) sizeof(val));
        }
    }

    ~FstackDpdkSocketStream() override {
        if (fd < 0) return;
        fstack_shutdown(fd, (int) ShutdownHow::ReadWrite);
        fstack_close(fd);
        fd = -1;
    }

protected:
    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, uint64_t timeout) override {
        return fstack_send(sockfd, buf, count, flags, timeout);
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, uint64_t timeout) override {
        return fstack_sendmsg(sockfd, message, flags, timeout);
    }

    ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, uint64_t timeout) override {
        return fstack_recv(sockfd, buf, count, flags, timeout);
    }

    ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, uint64_t timeout) override {
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
        return new FstackDpdkSocketStream(socket_family, m_nonblocking);
    }

    int fd_connect(int fd, const sockaddr* remote, socklen_t addrlen) override {
        return fstack_connect(fd, remote, addrlen, m_timeout);
    }
};

class FstackDpdkSocketServer : public KernelSocketServer {
public:
    using KernelSocketServer::KernelSocketServer;

    int init() override {
        m_listen_fd = fstack_socket(m_socket_family, SOCK_STREAM, 0);
        if (m_listen_fd < 0)
            return -1;
        if (m_socket_family == AF_INET) {
            int val = 1;
            fstack_setsockopt(m_listen_fd, IPPROTO_TCP, TCP_NODELAY, &val, (socklen_t) sizeof(val));
        }
        return 0;
    }

    int bind(uint16_t port, IPAddr addr) override {
        auto addr_in = EndPoint(addr, port).to_sockaddr_in();
        return fstack_bind(m_listen_fd, (sockaddr*) &addr_in, sizeof(addr_in));
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

    int fd_accept(int fd, struct sockaddr* addr, socklen_t* addrlen) override {
        return fstack_accept(fd, addr, addrlen, m_timeout);
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

    ETKernelSocketStream(int socket_family, bool nonblocking) :
            KernelSocketStream(socket_family, nonblocking) {
        if (fd >= 0) etpoller.register_notifier(fd, this);
    }

    ~ETKernelSocketStream() {
        if (fd >= 0) etpoller.unregister_notifier(fd);
    }

    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        void* buf_unused = nullptr;
        return doio_n(buf_unused, count, LAMBDA(do_sendfile(in_fd, offset, count)));
    }

private:
    ssize_t do_send(int sockfd, const void* buf, size_t count, int flags, uint64_t timeout) override {
        return etdoio(LAMBDA(::send(sockfd, buf, count, flags)),
                      LAMBDA_TIMEOUT(wait_for_writable(timeout)));
    }

    ssize_t do_sendmsg(int sockfd, const struct msghdr* message, int flags, uint64_t timeout) override {
        return etdoio(LAMBDA(::sendmsg(sockfd, message, flags)),
                      LAMBDA_TIMEOUT(wait_for_writable(timeout)));
    }

    ssize_t do_recv(int sockfd, void* buf, size_t count, int flags, uint64_t timeout) override {
        return etdoio(LAMBDA(::read(sockfd, buf, count)),
                      LAMBDA_TIMEOUT(wait_for_readable(timeout)));
    }

    ssize_t do_recvmsg(int sockfd, struct msghdr* message, int flags, uint64_t timeout) override {
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
        return new ETKernelSocketStream(socket_family, m_nonblocking);
    }
};

class ETKernelSocketServer : public KernelSocketServer, public NotifyContext {
public:
    using KernelSocketServer::KernelSocketServer;

    ~ETKernelSocketServer() {
        if (m_listen_fd >= 0) etpoller.unregister_notifier(m_listen_fd);
    }

    int init() override {
        if (KernelSocketServer::init() != 0) return -1;
        return etpoller.register_notifier(m_listen_fd, this);
    }

protected:
    KernelSocketStream* create_stream(int fd) override {
        return new ETKernelSocketStream(fd);
    }

    int fd_accept(int fd, struct sockaddr* addr, socklen_t* addrlen) override {
        uint64_t timeout = -1;
        return (int) etdoio(LAMBDA(::accept4(fd, addr, addrlen, SOCK_NONBLOCK)),
                            LAMBDA_TIMEOUT(wait_for_readable(timeout)));
    }
};

#endif // __linux__

/* ET Socket - End */

extern "C" ISocketClient* new_tcp_socket_client() {
    return new KernelSocketClient(true);
}
extern "C" ISocketClient* new_tcp_socket_client_ipv6() {
    return new KernelSocketClient(true);
}
extern "C" ISocketServer* new_tcp_socket_server() {
    return NewObj<KernelSocketServer>(AF_INET, false, true)->init();
}
extern "C" ISocketServer* new_tcp_socket_server_ipv6() {
    return NewObj<KernelSocketServer>(AF_INET6, false, true)->init();
}
extern "C" ISocketClient* new_uds_client() {
    return new KernelSocketClient(true);
}
extern "C" ISocketServer* new_uds_server(bool autoremove) {
    return NewObj<KernelSocketServer>(AF_UNIX, autoremove, true)->init();
}
#ifdef __linux__
extern "C" ISocketServer* new_zerocopy_tcp_server() {
    return NewObj<ZeroCopySocketServer>(AF_INET, false, true)->init();
}
extern "C" ISocketClient* new_zerocopy_tcp_client() {
    return new ZeroCopySocketClient(true);
}
#ifdef PHOTON_URING
extern "C" ISocketClient* new_iouring_tcp_client() {
    if (photon::iouring_register_files_enabled())
        return new IouringFixedFileSocketClient(false);
    else
        return new IouringSocketClient(false);
}
extern "C" ISocketServer* new_iouring_tcp_server() {
    if (photon::iouring_register_files_enabled())
        return NewObj<IouringFixedFileSocketServer>(AF_INET, false, false)->init();
    else
        return NewObj<IouringSocketServer>(AF_INET, false, false)->init();
}
#endif // PHOTON_URING
extern "C" ISocketClient* new_et_tcp_socket_client() {
    return new ETKernelSocketClient(true);
}
extern "C" ISocketServer* new_et_tcp_socket_server() {
    return NewObj<ETKernelSocketServer>(AF_INET, false, true)->init();
}
extern "C" ISocketClient* new_smc_socket_client() {
    return new KernelSocketClient(true);
}
extern "C" ISocketServer* new_smc_socket_server() {
    return NewObj<KernelSocketServer>(AF_SMC, false, true)->init();
}
#ifdef ENABLE_FSTACK_DPDK
extern "C" ISocketClient* new_fstack_dpdk_socket_client() {
    return new FstackDpdkSocketClient(true);
}
extern "C" ISocketServer* new_fstack_dpdk_socket_server() {
    return NewObj<FstackDpdkSocketServer>(false, true)->init();
}
#endif // ENABLE_FSTACK_DPDK
#endif // __linux__

////////////////////////////////////////////////////////////////////////////////

/* Implementations in socket.h */

EndPoint::EndPoint(const char* s) {
    estring_view ep(s);
    auto pos = ep.find_last_of(':');
    if (pos == estring::npos)
        return;
    // Detect IPv6 or IPv4
    estring ip_str = ep[pos - 1] == ']' ? ep.substr(1, pos - 2) : ep.substr(0, pos);
    auto ip = IPAddr(ip_str.c_str());
    if (ip.undefined())
        return;
    auto port_str = ep.substr(pos + 1);
    if (!port_str.all_digits())
        return;
    addr = ip;
    port = std::stoul(port_str);
}

LogBuffer& operator<<(LogBuffer& log, const IPAddr addr) {
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

LogBuffer& operator<<(LogBuffer& log, const EndPoint ep) {
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
LogBuffer& operator<<(LogBuffer& log, const sockaddr& addr) {
    return log << photon::net::sockaddr_storage(addr).to_endpoint();
}