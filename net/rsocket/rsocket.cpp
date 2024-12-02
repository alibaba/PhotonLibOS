#include "rsocket.h"

#include <fcntl.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/common/timeout.h>
#include <photon/net/basic_socket.h>
#include <photon/net/socket.h>
#include <photon/thread/thread11.h>
#include <rdma/rsocket.h>

#include <vector>

namespace photon {
namespace net {

struct tmp_msg_hdr : public ::msghdr {
    tmp_msg_hdr(const iovec* iov, int iovcnt) : ::msghdr{} {
        this->msg_iov = (iovec*)iov;
        this->msg_iovlen = iovcnt;
    }

    explicit tmp_msg_hdr(iovector_view& view)
        : tmp_msg_hdr(view.iov, view.iovcnt) {}

    operator ::msghdr*() { return this; }
};

struct RSockFD {
    int fd = -1;

    explicit RSockFD(int fd) : fd(fd) {
        if (fd >= 0) rfcntl(fd, F_SETFL, O_NONBLOCK, 1);
    }
    RSockFD() {
        fd = rsocket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd >= 0) {
            rfcntl(fd, F_SETFL, O_NONBLOCK, 1);
        }
    }
    ~RSockFD() {
        if (fd >= 0) {
            close();
        }
    }
    RSockFD(const RSockFD&) = delete;
    RSockFD& operator=(const RSockFD&) = delete;
    RSockFD(RSockFD&& o) { std::swap(fd, o.fd); }
    RSockFD& operator=(RSockFD&& o) {
        std::swap(fd, o.fd);
        return *this;
    }

    operator bool() const { return fd >= 0; }

    int close() {
        int ret = -1;
        if (fd >= 0) {
            ret = rclose(fd);
            fd = -1;
        }
        return ret;
    }

    Object* get_underlay_object(uint64_t) { return (Object*)(uint64_t)fd; }

    int setsockopt(int level, int option_name, const void* option_value,
                   socklen_t option_len) {
        return rsetsockopt(fd, level, option_name, option_value, option_len);
    }
    int getsockopt(int level, int option_name, void* option_value,
                   socklen_t* option_len) {
        return rgetsockopt(fd, level, option_name, option_value, option_len);
    }

    int do_get_name(int fd, Getter getter, EndPoint& addr) {
        sockaddr_storage storage;
        socklen_t len = storage.get_max_socklen();
        int ret = getter(fd, storage.get_sockaddr(), &len);
        if (ret < 0 || len > storage.get_max_socklen()) return -1;
        addr = storage.to_endpoint();
        return 0;
    }

    int getsockname(photon::net::EndPoint& addr) {
        sockaddr_storage storage;
        socklen_t addrlen = sizeof(storage.store);
        auto ret = rgetsockname(fd, (sockaddr*)&storage.store, &addrlen);
        if (ret == 0) addr = storage.to_endpoint();
        return ret;
    }
    int getpeername(photon::net::EndPoint& addr) {
        sockaddr_storage storage;
        socklen_t addrlen = sizeof(storage.store);
        auto ret = rgetpeername(fd, (sockaddr*)&storage.store, &addrlen);
        if (ret == 0) addr = storage.to_endpoint();
        return ret;
    }
    int do_poll(int events, uint64_t timeout) {
        int ret;
        struct pollfd fds;
        fds.fd = fd;
        fds.events = events;
        Timeout tmo(timeout);

        do {
            ret = rpoll(&fds, 1, 0);
            photon::thread_yield();
        } while (!ret && tmo.timeout());
        if (ret == 0) {
            errno = ETIMEDOUT;
            return -1;
        } else {
            return ret == 1
                       ? (fds.revents & (POLLERR | POLLHUP | POLLIN | POLLOUT))
                       : ret;
        }
    }

    template <int EVENT, typename FUNC, typename... ARGS>
    ssize_t do_io_action(uint64_t t, FUNC f, ARGS... args) {
        Timeout tmo(t);
        do {
            auto ret = do_poll(EVENT | POLLERR | POLLHUP, tmo.timeout());
            if (ret < 0) return ret;
            if (ret & EVENT) {
                auto x = f(fd, args...);
                if (x < 0) {
                    ERRNO err;
                    if (err.no == EAGAIN || err.no == EWOULDBLOCK) {
                        continue;
                    }
                }
                return x;
            }
        } while (tmo.timeout());
        return 0;
    }

    template <int EVENT, typename FUNC, typename... ARGS>
    ssize_t do_io_fully_action(uint64_t t, FUNC f, size_t n, ARGS... args) {
        Timeout tmo(t);
        ssize_t x = 0;
        while (x < (ssize_t)n) {
            auto ret = do_io_action<EVENT>(tmo.timeout(), f, args...);
            if (ret < 0) return ret;
            if (ret == 0) return x;
            x += ret;
        }
        return x;
    }

    ssize_t recv(uint64_t timeout, void* buf, size_t count, int flags = 0) {
        return do_io_action<POLLIN>(timeout, rrecv, buf, count, flags);
    }
    ssize_t recv(uint64_t timeout, const struct iovec* iov, int iovcnt,
                 int flags = 0) {
        tmp_msg_hdr msg(iov, iovcnt);
        return do_io_action<POLLIN>(timeout, rrecvmsg, msg, flags);
    }
    ssize_t read(uint64_t timeout, void* buf, size_t count) {
        return do_io_fully_action<POLLIN>(timeout, rrecv, count, buf, count, 0);
    }
    ssize_t readv(uint64_t timeout, const struct iovec* iov, int iovcnt) {
        iovector_view viov{(iovec*)iov, iovcnt};
        tmp_msg_hdr msg(viov);
        return do_io_fully_action<POLLIN>(timeout, rrecvmsg, viov.sum(), msg,
                                          0);
    }
    ssize_t send(uint64_t timeout, const void* buf, size_t count,
                 int flags = 0) {
        return do_io_action<POLLOUT>(timeout, rsend, buf, count, flags);
    }
    ssize_t send(uint64_t timeout, const struct iovec* iov, int iovcnt,
                 int flags = 0) {
        tmp_msg_hdr msg(iov, iovcnt);
        return do_io_action<POLLOUT>(timeout, rsendmsg, msg, flags);
    }
    ssize_t write(uint64_t timeout, const void* buf, size_t count) {
        return do_io_fully_action<POLLOUT>(timeout, rsend, count, buf, count,
                                           0);
    }
    ssize_t writev(uint64_t timeout, const struct iovec* iov, int iovcnt) {
        iovector_view viov{(iovec*)iov, iovcnt};
        tmp_msg_hdr msg(viov);
        return do_io_fully_action<POLLOUT>(timeout, rsendmsg, viov.sum(), msg,
                                           0);
    }
    int shutdown(ShutdownHow how) {
        return rshutdown(fd, static_cast<int>(how));
    }
    int bind(const photon::net::EndPoint& addr) {
        sockaddr_storage addr_storage(addr);
        return rbind(fd, addr_storage.get_sockaddr(),
                     addr_storage.get_socklen());
    }

    int listen(int backlog) { return rlisten(fd, backlog); }

    template <typename Func>
    RSockFD accept(uint64_t timeout, photon::net::EndPoint* remote_endpoint,
                   Func&& interrupt) {
        sockaddr_storage addr;
        socklen_t socklen = sizeof(addr.store);
        Timeout tmo(timeout);
        int ret;
        do {
            ret = raccept(fd, (sockaddr*)&addr.store, &socklen);
            if (ret < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    if (interrupt()) {
                        errno = EINTR;
                        break;
                    }
                    photon::thread_yield();
                } else {
                    break;
                }
            }
        } while (ret < 0 && tmo.timeout());
        if (ret < 0) return RSockFD(-1);
        if (remote_endpoint) {
            *remote_endpoint = addr.to_endpoint();
        }
        return RSockFD(ret);
    }

    int connect(uint64_t timeout,
                const photon::net::EndPoint& remote_endpoint) {
        Timeout tmo(timeout);
        sockaddr_storage addr(remote_endpoint);
        auto ret = rconnect(fd, (sockaddr*)&addr.store, addr.get_socklen());
        if (ret < 0) {
            if (errno == EINPROGRESS) {
                auto r = do_poll(POLLOUT, tmo.timeout());
                if (r & POLLOUT)
                    return 0;
                else if (r < 0)
                    return r;
            } else {
                return ret;
            }
        }
        return ret;
    }
};

class RSocketStream : public photon::net::ISocketStream {
public:
    RSockFD rfd;
    uint64_t m_timeout = -1UL;

    explicit RSocketStream(RSockFD&& fd) : rfd(std::move(fd)) {}

    uint64_t timeout() const override { return m_timeout; }
    void timeout(uint64_t tm) override { m_timeout = tm; }
    Object* get_underlay_object(uint64_t) override {
        return rfd.get_underlay_object(0);
    }
    int setsockopt(int level, int option_name, const void* option_value,
                   socklen_t option_len) override {
        return rfd.setsockopt(level, option_name, option_value, option_len);
    }
    int getsockopt(int level, int option_name, void* option_value,
                   socklen_t* option_len) override {
        return rfd.getsockopt(level, option_name, option_value, option_len);
    }
    int getsockname(photon::net::EndPoint& addr) override {
        return rfd.getsockname(addr);
    }
    int getpeername(photon::net::EndPoint& addr) override {
        return rfd.getpeername(addr);
    }
    int getsockname(char* path, size_t count) override {
        errno = ENOSYS;
        return -1;
    }
    int getpeername(char* path, size_t count) override {
        errno = ENOSYS;
        return -1;
    }
    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        errno = ENOSYS;
        return -1;
    }
    int close() override { return rfd.close(); }

    int shutdown(ShutdownHow how) override { return rfd.shutdown(how); }

    ssize_t recv(void* buf, size_t count, int flags = 0) override {
        return rfd.recv(m_timeout, buf, count, flags);
    }
    ssize_t recv(const struct iovec* iov, int iovcnt, int flags = 0) override {
        return rfd.recv(m_timeout, iov, iovcnt, flags);
    }
    ssize_t send(const void* buf, size_t count, int flags = 0) override {
        return rfd.send(m_timeout, buf, count, flags);
    }
    ssize_t send(const struct iovec* iov, int iovcnt, int flags = 0) override {
        return rfd.send(m_timeout, iov, iovcnt, flags);
    }
    ssize_t read(void* buf, size_t count) override {
        return rfd.read(m_timeout, buf, count);
    }
    ssize_t readv(const struct iovec* iov, int iovcnt) override {
        return rfd.readv(m_timeout, iov, iovcnt);
    }
    ssize_t write(const void* buf, size_t count) override {
        return rfd.write(m_timeout, buf, count);
    }
    ssize_t writev(const struct iovec* iov, int iovcnt) override {
        return rfd.writev(m_timeout, iov, iovcnt);
    }
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

    virtual int setsockopt(RSockFD& rfd) {
        for (auto& opt : *this) {
            if (rfd.setsockopt(opt.level, opt.opt_name, opt.opt_val,
                               opt.opt_len) != 0) {
                LOG_ERRNO_RETURN(EINVAL, -1, "Failed to setsockopt ",
                                 VALUE(opt.level), VALUE(opt.opt_name),
                                 VALUE(opt.opt_val));
            }
        }
        return 0;
    }
};

class RSocketClient : public photon::net::ISocketClient {
public:
    uint64_t m_timeout = -1UL;
    SockOptBuffer m_opts;

    photon::net::ISocketStream* connect(const char* path,
                                        size_t count = 0) override {
        errno = ENOSYS;
        return nullptr;
    }

    photon::net::ISocketStream* connect(
        const photon::net::EndPoint& remote,
        const photon::net::EndPoint* local) override {
        // do something
        RSockFD sock;
        if (m_opts.setsockopt(sock) < 0) return nullptr;
        if (local != nullptr) {
            if (sock.bind(*local) < 0) {
                LOG_ERROR_RETURN(0, nullptr, "failed to bind local port");
            }
        }
        auto ret = sock.connect(m_timeout, remote);
        if (ret < 0)
            LOG_ERROR_RETURN(0, nullptr, "failed to connect to remote");
        return new RSocketStream(std::move(sock));
    }

    ~RSocketClient() override {}

    Object* get_underlay_object(uint64_t) override {
        errno = ENOSYS;
        return nullptr;
    }

    int setsockopt(int level, int option_name, const void* option_value,
                   socklen_t option_len) override {
        return m_opts.put_opt(level, option_name, option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value,
                   socklen_t* option_len) override {
        return m_opts.get_opt(level, option_name, option_value, option_len);
    }

    uint64_t timeout() const override { return m_timeout; }

    void timeout(uint64_t tm) override { m_timeout = tm; }
};

class RSocketServer : public photon::net::ISocketServer {
public:
    RSockFD rfd;
    bool m_block_serv = false;
    bool waiting = false;
    Handler m_handler;
    std::atomic<photon::thread*> workth{};
    uint64_t m_timeout = -1UL;
    ~RSocketServer() override { terminate(); }
    int bind(const EndPoint& ep) override { return rfd.bind(ep); }
    int bind(const char* path, size_t count) override {
        errno = ENOSYS;
        return -1;
    }
    int listen(int backlog = 1024) override { return rfd.listen(backlog); }
    photon::net::ISocketStream* accept(
        photon::net::EndPoint* remote_endpoint = nullptr) override {
        auto fd = rfd.accept(m_timeout, remote_endpoint,
                             [&] { return !check_running(); });
        if (!fd) return nullptr;
        return new RSocketStream(std::move(fd));
    }

    ISocketServer* set_handler(Handler handler) override {
        m_handler = handler;
        return this;
    }
    static void handler(Handler m_handler, photon::net::ISocketStream* sess) {
        m_handler(sess);
        delete sess;
    }
    bool check_running() { return workth.load(); }
    int accept_loop() {
        photon::thread* th = nullptr;
        if (!workth.compare_exchange_strong(th, photon::CURRENT))
            LOG_ERROR_RETURN(EALREADY, -1, "Already listening");
        while (check_running()) {
            waiting = true;
            auto connection = accept();
            waiting = false;
            if (!check_running()) return 0;
            if (connection) {
                connection->timeout(m_timeout);
                photon::thread_create11(&RSocketServer::handler, m_handler,
                                        connection);
            } else {
                LOG_WARN(
                    "KernelSocketServer: failed to accept new connections: `",
                    ERRNO());
                photon::thread_yield();
            }
        }
        return 0;
    }

    int start_loop(bool block) override {
        if (check_running())
            LOG_ERROR_RETURN(EALREADY, -1, "Already listening");
        m_block_serv = block;
        if (block) return accept_loop();
        auto loop = &RSocketServer::accept_loop;
        auto th = photon::thread_create((photon::thread_entry&)loop, this);
        thread_enable_join(th);
        thread_yield_to(th);
        return 0;
    }
    void terminate() final {
        photon::thread* th = workth.exchange(nullptr);
        if (!th) return;
        if (waiting) {
            thread_interrupt(th);
            if (!m_block_serv) thread_join((photon::join_handle*)th);
        }
    }
    Object* get_underlay_object(uint64_t) override {
        return rfd.get_underlay_object(0);
    }

    int setsockopt(int level, int option_name, const void* option_value,
                   socklen_t option_len) override {
        return rfd.setsockopt(level, option_name, option_value, option_len);
    }
    int getsockopt(int level, int option_name, void* option_value,
                   socklen_t* option_len) override {
        return rfd.getsockopt(level, option_name, option_value, option_len);
    }

    uint64_t timeout() const override { return m_timeout; }
    void timeout(uint64_t tm) override { m_timeout = tm; }
    int getsockname(photon::net::EndPoint& addr) override {
        return rfd.getsockname(addr);
    }
    int getpeername(photon::net::EndPoint& addr) override {
        return rfd.getpeername(addr);
    }
    int getsockname(char* path, size_t count) override {
        errno = ENOSYS;
        return -1;
    }
    int getpeername(char* path, size_t count) override {
        errno = ENOSYS;
        return -1;
    }
};

extern "C" photon::net::ISocketClient* new_rsocket_client() {
    return new RSocketClient();
}

extern "C" photon::net::ISocketServer* new_rsocket_server() {
    auto ret = new RSocketServer();
    if (!ret->rfd) {
        delete ret;
        return nullptr;
    }
    return ret;
}

}  // namespace net
}  // namespace photon