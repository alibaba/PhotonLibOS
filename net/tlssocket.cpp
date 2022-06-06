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

#include "tlssocket.h"

#include <netinet/tcp.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include "abstract_socket.h"
#include "basic_socket.h"
#include "socket.h"

namespace photon {
namespace net {

static SSL_CTX* g_ctx = nullptr;
static photon::mutex g_mtx;

static char pempassword[4096];

int pem_password_cb(char* buf, int size, int rwflag, void*) {
    strncpy(buf, pempassword, size);
    return strlen(buf);
}
ssize_t set_pass_phrase(const char* pass) {
    strncpy(pempassword, pass, sizeof(pempassword));
    return strlen(pempassword);
}
void* ssl_get_ctx() { return g_ctx; }
int ssl_init(void* cert, void* key) {
    photon::scoped_lock l(g_mtx);
    char errbuf[4096];
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_ciphers();
    OpenSSL_add_all_digests();
    OpenSSL_add_all_algorithms();
    g_ctx = SSL_CTX_new(TLSv1_2_method());
    if (g_ctx == nullptr) {
        auto e = ERR_get_error();
        ERR_error_string_n(e, errbuf, 4096);
        LOG_ERROR_RETURN(0, -1, "Failed to initial TLS 1.2: ", errbuf);
    }
    int ret = 1;
    if (cert) {
        if (!key) {
            LOG_ERROR_RETURN(EINVAL, -1, "path_of_key is nullptr");
        }
        ret = SSL_CTX_use_certificate(g_ctx, (X509*)cert);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
        ret = SSL_CTX_use_RSAPrivateKey(g_ctx, (RSA*)key);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
        ret = SSL_CTX_check_private_key(g_ctx);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
    }
    return 0;
}
int ssl_init(const char* cert_path, const char* key_path,
             const char* passphrase) {
    photon::scoped_lock l(g_mtx);
    char errbuf[4096];
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_ciphers();
    OpenSSL_add_all_digests();
    OpenSSL_add_all_algorithms();
    g_ctx = SSL_CTX_new(TLSv1_2_method());
    if (g_ctx == nullptr) {
        auto e = ERR_get_error();
        ERR_error_string_n(e, errbuf, 4096);
        LOG_ERROR_RETURN(0, -1, "Failed to initial TLS 1.2: ", errbuf);
    }
    int ret = 1;
    if (passphrase) set_pass_phrase(passphrase);
    SSL_CTX_set_default_passwd_cb(g_ctx, &pem_password_cb);
    if (cert_path) {
        if (!key_path) {
            LOG_ERROR_RETURN(EINVAL, -1, "path_of_key is nullptr");
        }
        ret = SSL_CTX_use_certificate_file(g_ctx, cert_path, SSL_FILETYPE_PEM);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
        ret = SSL_CTX_use_RSAPrivateKey_file(g_ctx, key_path, SSL_FILETYPE_PEM);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
        ret = SSL_CTX_check_private_key(g_ctx);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
    }
    return 0;
}
int ssl_init() {
    photon::scoped_lock l(g_mtx);
    char errbuf[4096];
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_ciphers();
    OpenSSL_add_all_digests();
    OpenSSL_add_all_algorithms();
    if (g_ctx) SSL_CTX_free(g_ctx);
    g_ctx = SSL_CTX_new(TLSv1_2_method());
    if (g_ctx == nullptr) {
        auto e = ERR_get_error();
        ERR_error_string_n(e, errbuf, 4096);
        LOG_ERROR_RETURN(0, -1, "Failed to initial TLS 1.2: ", errbuf);
    }
    return 0;
}
int ssl_set_cert(const char* cert_str) {
    photon::scoped_lock l(g_mtx);
    char errbuf[4096];
    if (cert_str) {
        auto bio = BIO_new_mem_buf((void*)cert_str, -1);
        DEFER(BIO_free(bio));
        auto cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        auto ret = SSL_CTX_use_certificate(g_ctx, cert);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
    } else
        return -1;
    return 0;
}
int ssl_set_pkey(const char* key_str, const char* passphrase) {
    photon::scoped_lock l(g_mtx);
    char errbuf[4096];
    if (passphrase) {
        SSL_CTX_set_default_passwd_cb(g_ctx, &pem_password_cb);
        set_pass_phrase(passphrase);
    }
    if (key_str) {
        auto bio = BIO_new_mem_buf((void*)key_str, -1);
        auto key =
            PEM_read_bio_RSAPrivateKey(bio, nullptr, &pem_password_cb, nullptr);
        auto ret = SSL_CTX_use_RSAPrivateKey(g_ctx, key);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
        ret = SSL_CTX_check_private_key(g_ctx);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
    } else
        return -1;
    return 0;
}
int ssl_fini() {
    photon::scoped_lock l(g_mtx);
    SSL_CTX_free(g_ctx);
    g_ctx = nullptr;
    return 0;
}

template <typename IOCB>
static __FORCE_INLINE__ ssize_t ssl_doio(IOCB iocb, SSL* ssl,
                                         uint64_t timeout) {
    Timeout tmo(timeout);
    int fd = SSL_get_fd(ssl);
    while (true) {
        ssize_t ret = iocb();
        if (ret <= 0) {
            auto e = SSL_get_error(ssl, ret);
            switch (e) {
                case SSL_ERROR_NONE:
                case SSL_ERROR_WANT_CONNECT:
                case SSL_ERROR_WANT_ACCEPT:
                case SSL_ERROR_WANT_X509_LOOKUP:
                    return 0;
                case SSL_ERROR_ZERO_RETURN:
                    LOG_ERROR_RETURN(ECONNRESET, -1, "SSL disconnected");
                case SSL_ERROR_WANT_READ:
                    if (photon::wait_for_fd_readable(fd, tmo.timeout()))
                        return ret;
                    continue;
                case SSL_ERROR_WANT_WRITE:
                    if (photon::wait_for_fd_writable(fd, tmo.timeout()))
                        return ret;
                    continue;
                case SSL_ERROR_SSL:
                    char errbuf[4096];
                    ERR_error_string_n(e, errbuf, 4096);
                    LOG_ERROR_RETURN(
                        EIO, -1, "Non-recoverable SSL library error: ", errbuf);
                default:
                    return ret;
            }
        }
        return ret;
    }
}

ssize_t ssl_read(SSL* ssl, void* buf, size_t count, uint64_t timeout) {
    return ssl_doio(LAMBDA(::SSL_read(ssl, buf, count)), ssl, timeout);
}
ssize_t ssl_write(SSL* ssl, const void* buf, size_t count, uint64_t timeout) {
    return ssl_doio(LAMBDA(::SSL_write(ssl, buf, count)), ssl, timeout);
}
ssize_t ssl_read_n(SSL* ssl, void* buf, size_t count, uint64_t timeout) {
    return doio_n(buf, count,
                  LAMBDA_TIMEOUT(ssl_read(ssl, buf, count, timeout)));
}
ssize_t ssl_write_n(SSL* ssl, const void* buf, size_t count, uint64_t timeout) {
    return doio_n((void*&)buf, count,
                  LAMBDA_TIMEOUT(ssl_write(ssl, buf, count, timeout)));
}
ssize_t ssl_connect(SSL* ssl, uint64_t timeout) {
    return ssl_doio(LAMBDA(::SSL_connect(ssl)), ssl, timeout) == 1 ? 0 : -1;
}
ssize_t ssl_accept(SSL* ssl, uint64_t timeout) {
    return ssl_doio(LAMBDA(::SSL_accept(ssl)), ssl, timeout) == 1 ? 0 : -1;
}
ssize_t ssl_set_cert_file(const char* path) {
    photon::scoped_lock l(g_mtx);
    char errbuf[4096];
    auto ret = SSL_CTX_use_certificate_file(g_ctx, path, SSL_FILETYPE_PEM);
    if (ret != 1) {
        ERR_error_string_n(ret, errbuf, sizeof(errbuf));
        LOG_ERROR_RETURN(0, -1, errbuf);
    }
    return 0;
}
ssize_t ssl_set_pkey_file(const char* path) {
    photon::scoped_lock l(g_mtx);
    char errbuf[4096];
    auto ret = SSL_CTX_use_PrivateKey_file(g_ctx, path, SSL_FILETYPE_PEM);
    if (ret != 1) {
        ERR_error_string_n(ret, errbuf, sizeof(errbuf));
        LOG_ERROR_RETURN(0, -1, errbuf);
    }
    ret = SSL_CTX_check_private_key(g_ctx);
    if (ret != 1) {
        ERR_error_string_n(ret, errbuf, sizeof(errbuf));
        LOG_ERROR_RETURN(0, -1, errbuf);
    }
    return 0;
}

class TLSSocketImpl : public SocketBase {
public:
    int fd;
    uint64_t m_timeout = -1;
    photon::mutex m_rmutex, m_wmutex;
    SSL* m_ssl;
    explicit TLSSocketImpl(int fd) : fd(fd), m_ssl(SSL_new(g_ctx)) {
        if (m_ssl) SSL_set_fd(m_ssl, fd);
    }
    TLSSocketImpl()
        : fd(net::socket(AF_INET, SOCK_STREAM, 0)), m_ssl(SSL_new(g_ctx)) {
        if (fd > 0) {
            int val = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
        }
        if (m_ssl) SSL_set_fd(m_ssl, fd);
    }
    virtual ~TLSSocketImpl() override {
        close();
    }
    virtual int close() override {
        if (m_ssl) {
            SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl=nullptr;
        }
        auto ret = ::close(fd);
        if (ret < 0) LOG_ERROR_RETURN(0, -1, "failed to close socket");
        fd = -1;
        return 0;
    }
    int fdconnect(const EndPoint& ep) {
        auto addr_in = ep.to_sockaddr_in();
        auto ret = net::connect(fd, (struct sockaddr*)&addr_in, sizeof(addr_in),
                                m_timeout);
        if (ret != 0) return ret;
        SSL_set_fd(m_ssl, fd);
        return ssl_connect(m_ssl, m_timeout);
    }
    int do_ssl_accept() { return ssl_accept(m_ssl, m_timeout); }
    int do_accept() { return net::accept(fd, nullptr, nullptr, m_timeout); }
    int do_accept(EndPoint& remote_endpoint) {
        struct sockaddr_in addr_in;
        socklen_t len = sizeof(addr_in);
        int cfd = net::accept(fd, (struct sockaddr*)&addr_in, &len, m_timeout);
        if (cfd < 0 || len != sizeof(addr_in)) return -1;

        remote_endpoint.from_sockaddr_in(addr_in);
        return cfd;
    }
    virtual ISocketStream* accept(EndPoint* remote_endpoint) override {
        int cfd = remote_endpoint ? do_accept(*remote_endpoint) : do_accept();
        if (cfd < 0) return nullptr;
        auto code = photon::wait_for_fd_readable(cfd, m_timeout);
        if (code < 0) return nullptr;
        auto ret = new TLSSocketImpl(cfd);
        if (ret->do_ssl_accept()) {  // failed
            delete ret;
            return nullptr;
        }
        return ret;
    }
    // read, write keeps "have to read/write N bytes"
    virtual ssize_t read(void* buf, size_t count) override {
        photon::scoped_lock lock(m_rmutex);
        return ssl_read_n(m_ssl, buf, count, m_timeout);
    }
    // somehow ,openssl lack of diov type op.
    virtual ssize_t readv(const struct iovec* iov, int iovcnt) override {
        Timeout tmo(m_timeout);
        auto iovec = IOVector(iov, iovcnt);  // make copy, might change
        ssize_t ret = 0;
        photon::scoped_lock lock(m_rmutex);
        while (!iovec.empty()) {
            auto tmp = ssl_read_n(m_ssl, iovec.front().iov_base,
                                  iovec.front().iov_len, tmo.timeout());
            if (tmp < 0) return tmp;
            iovec.extract_front(tmp);
            ret += tmp;
        }
        return ret;
    }
    virtual ssize_t readv_mutable(struct iovec* iov, int iovcnt) override {
        // there might be a faster implementaion in derived class
        return readv(iov, iovcnt);
    }
    virtual ssize_t write(const void* buf, size_t count) override {
        photon::scoped_lock lock(m_wmutex);
        return ssl_write_n(m_ssl, buf, count, m_timeout);
    }
    virtual ssize_t writev(const struct iovec* iov, int iovcnt) override {
        Timeout tmo(m_timeout);
        auto iovec = IOVector(iov, iovcnt);  // make copy, might change
        ssize_t ret = 0;
        photon::scoped_lock lock(m_wmutex);
        while (!iovec.empty()) {
            auto tmp = ssl_write_n(m_ssl, iovec.front().iov_base,
                                   iovec.front().iov_len, tmo.timeout());
            if (tmp < 0) return tmp;
            iovec.extract_front(tmp);
            ret += tmp;
        }
        return ret;
    }
    virtual ssize_t writev_mutable(struct iovec* iov, int iovcnt) override {
        // there might be a faster implementaion in derived class
        return writev(iov, iovcnt);
    }
    virtual ssize_t recv(void* buf, size_t count) override {
        photon::scoped_lock lock(m_rmutex);
        return ssl_read(m_ssl, buf, count, m_timeout);
    }
    virtual ssize_t recv(const struct iovec* iov, int iovcnt) override {
        Timeout tmo(m_timeout);
        auto iovec = IOVector(iov, iovcnt);  // make copy, might change
        ssize_t ret = 0;
        photon::scoped_lock lock(m_rmutex);
        while (!iovec.empty()) {
            auto tmp = ssl_read(m_ssl, iovec.front().iov_base,
                                iovec.front().iov_len, tmo.timeout());
            if (tmp < 0) return tmp;
            if (((uint64_t)tmp) < iovec.front().iov_len) {
                return ret + tmp;
            }
            iovec.extract_front(tmp);
            ret += tmp;
        }
        return ret;
    }

    virtual ssize_t send(const void* buf, size_t count) override {
        photon::scoped_lock lock(m_wmutex);
        return ssl_write(m_ssl, buf, count, m_timeout);
    }
    virtual ssize_t send(const struct iovec* iov, int iovcnt) override {
        Timeout tmo(m_timeout);
        auto iovec = IOVector(iov, iovcnt);  // make copy, might change
        ssize_t ret = 0;
        photon::scoped_lock lock(m_wmutex);
        while (!iovec.empty()) {
            auto tmp = ssl_write_n(m_ssl, iovec.front().iov_base,
                                   iovec.front().iov_len, tmo.timeout());
            if (tmp < 0) return tmp;
            if (((uint64_t)tmp) < iovec.front().iov_len) {
                return ret + tmp;
            }
            iovec.extract_front(tmp);
            ret += tmp;
        }
        return ret;
    }
    virtual int bind(uint16_t port, IPAddr addr) override {
        auto addr_in = EndPoint{addr, port}.to_sockaddr_in();
        return ::bind(fd, (struct sockaddr*)&addr_in, sizeof(addr_in));
    }
    virtual int listen(int backlog) override { return ::listen(fd, backlog); }
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
    virtual uint64_t timeout() override { return m_timeout; }
    virtual void timeout(uint64_t tm) override { m_timeout = tm; }
    virtual int setsockopt(int level, int option_name, const void* option_value,
                           socklen_t option_len) override {
        return ::setsockopt(fd, level, option_name, option_value, option_len);
    }
    virtual int getsockopt(int level, int option_name, void* option_value,
                           socklen_t* option_len) override {
        return ::getsockopt(fd, level, option_name, option_value, option_len);
    }
    virtual int shutdown(ShutdownHow how) override {
        // shutdown how defined as 0 for RD, 1 for WR and 2 for RDWR
        // in sys/socket.h, cast ShutdownHow into int just fits
        return ::shutdown(fd, static_cast<int>(how));
    }
};

class TLSSocketServer : public TLSSocketImpl {
protected:
    Handler m_handler;

    photon::thread* workth;
    bool waiting;

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
                photon::thread_create11(&TLSSocketServer::handler, this, sess);
            } else {
                photon::thread_usleep(1000);
            }
        }
        return 0;
    }

    void handler(ISocketStream* sess) {
        m_handler(sess);
        delete sess;
    }

public:
    explicit TLSSocketServer(int fd) : TLSSocketImpl(fd), workth(nullptr) {}
    TLSSocketServer() : TLSSocketImpl(), workth(nullptr) {}
    virtual ~TLSSocketServer() { terminate(); }

    virtual int start_loop(bool block) override {
        if (workth) LOG_ERROR_RETURN(EALREADY, -1, "Already listening");
        if (block) return accept_loop();
        auto th = photon::thread_create11(&TLSSocketServer::accept_loop, this);
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
        return TLSSocketImpl::bind(port, addr);
    }
    virtual int bind(const char* path, size_t count) override {
        LOG_ERROR_RETURN(EINVAL, -1, "Not implemeted");
    }
    virtual int listen(int backlog) override {
        return TLSSocketImpl::listen(backlog);
    }

    virtual ISocketStream* accept(EndPoint* remote_endpoint) override {
        return TLSSocketImpl::accept(remote_endpoint);
    }
    virtual ISocketStream* accept() override {
        return TLSSocketImpl::accept(nullptr);
    }
    bool is_socket(const char* path) const {
        struct stat statbuf;
        if (0 == stat(path, &statbuf)) return S_ISSOCK(statbuf.st_mode) != 0;
        return false;
    }

    virtual uint64_t timeout() override { return TLSSocketImpl::timeout(); }
    virtual void timeout(uint64_t x) override {
        return TLSSocketImpl::timeout(x);
    }
};

// factory class
class TLSSocketClient : public SocketBase {
public:
    SockOptBuffer opts;
    uint64_t m_timeout = -1;

    virtual int setsockopt(int level, int option_name, const void* option_value,
                           socklen_t option_len) override {
        return opts.put_opt(level, option_name, option_value, option_len);
    }

    TLSSocketImpl* create_socket() {
        auto sock = new TLSSocketImpl();
        if (sock->fd < 0 || sock->m_ssl == nullptr) {
            delete sock;
            LOG_ERROR_RETURN(0, nullptr, "Failed to create socket fd");
        }
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

    virtual ISocketStream* connect(const EndPoint& ep) override {
        auto sock = create_socket();
        if (!sock) return nullptr;
        auto ret = sock->fdconnect(ep);
        if (ret < 0) {
            delete sock;
            LOG_ERROR_RETURN(0, nullptr, "Failed to connect to ", ep);
        }
        return sock;
    }
    virtual uint64_t timeout() override { return m_timeout; }
    virtual void timeout(uint64_t tm) override { m_timeout = tm; }
};

net::ISocketClient* new_tls_socket_client() { return new TLSSocketClient(); }

net::ISocketServer* new_tls_socket_server() {
    if (g_ctx == nullptr)
        LOG_ERROR_RETURN(ENXIO, nullptr, "libssl have not initialized");
    auto ret = new TLSSocketServer();
    if (ret->fd < 0 || ret->m_ssl == nullptr) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "SSL create failed");
    }
    return ret;
}

}  // namespace net
}
