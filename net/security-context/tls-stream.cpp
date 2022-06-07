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

#include "tls-stream.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <vector>

#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>
#include <photon/net/abstract_socket.h>
#include <photon/net/socket.h>
#include <photon/thread/thread.h>

namespace photon {
namespace net {

constexpr size_t BUFFER_SIZE = 4 * 1024;
constexpr size_t MAX_PASSPHASE_SIZE = 4 * 1024;
constexpr size_t MAX_ERRSTRING_SIZE = 4 * 1024;

template <typename T>
class Singleton {
public:
    static T& getInstance() {
        static T instance;
        return instance;
    }
    // delete copy and move constructors and assign operators
    Singleton(Singleton const&) = delete;             // Copy construct
    Singleton(Singleton&&) = delete;                  // Move construct
    Singleton& operator=(Singleton const&) = delete;  // Copy assign
    Singleton& operator=(Singleton&&) = delete;       // Move assign
protected:
    Singleton() {}
    ~Singleton() {}
};

class GlobalSSLContext : public Singleton<GlobalSSLContext> {
public:
    std::vector<std::unique_ptr<photon::mutex>> mtx;

    static void threadid_callback(CRYPTO_THREADID* id) {
        CRYPTO_THREADID_set_pointer(id, photon::get_vcpu());
    }

    static void lock_callback(int mode, int n, const char* file, int line) {
        auto& ctx = getInstance();
        if (mode & CRYPTO_LOCK) {
            ctx.mtx[n]->lock();
        } else {
            ctx.mtx[n]->unlock();
        }
    }

    GlobalSSLContext() {
        SSL_load_error_strings();
        SSL_library_init();
        OpenSSL_add_all_ciphers();
        OpenSSL_add_all_digests();
        OpenSSL_add_all_algorithms();
        mtx.clear();
        for (int i = 0; i < CRYPTO_num_locks(); i++) {
            mtx.emplace_back(std::make_unique<photon::mutex>());
        }
        CRYPTO_THREADID_set_callback(&GlobalSSLContext::threadid_callback);
        CRYPTO_set_locking_callback(&GlobalSSLContext::lock_callback);
    }

    ~GlobalSSLContext() {
        CRYPTO_set_id_callback(NULL);
        CRYPTO_set_locking_callback(NULL);
    }
};

class TLSContext {
public:
    SSL_CTX* ctx;
    char pempassword[MAX_PASSPHASE_SIZE];

    TLSContext() {
        char errbuf[4096];
        ctx = SSL_CTX_new(SSLv23_method());
        if (ctx == nullptr) {
            auto e = ERR_get_error();
            ERR_error_string_n(e, errbuf, MAX_ERRSTRING_SIZE);
            LOG_ERROR(0, -1, "Failed to initial TLS: ", errbuf);
        }
    }

    ~TLSContext() {
        if (ctx) SSL_CTX_free(ctx);
    }

    static int pem_password_cb(char* buf, int size, int rwflag, void* ptr) {
        auto self = (TLSContext*)ptr;
        strncpy(buf, self->pempassword, size);
        return strlen(buf);
    }
    ssize_t set_pass_phrase(const char* pass) {
        strncpy(pempassword, pass, sizeof(pempassword));
        return strlen(pempassword);
    }
    int ssl_set_cert(const char* cert_str) {
        char errbuf[4096];
        if (cert_str) {
            auto bio = BIO_new_mem_buf((void*)cert_str, -1);
            DEFER(BIO_free(bio));
            auto cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
            auto ret = SSL_CTX_use_certificate(ctx, cert);
            if (ret != 1) {
                ERR_error_string_n(ret, errbuf, sizeof(errbuf));
                LOG_ERROR_RETURN(0, -1, errbuf);
            }
        } else
            return -1;
        return 0;
    }
    int ssl_set_pkey(const char* key_str, const char* passphrase) {
        char errbuf[4096];
        if (passphrase) {
            SSL_CTX_set_default_passwd_cb(ctx, &pem_password_cb);
            SSL_CTX_set_default_passwd_cb_userdata(ctx, this);
            set_pass_phrase(passphrase);
        }
        if (key_str) {
            auto bio = BIO_new_mem_buf((void*)key_str, -1);
            DEFER(BIO_free(bio));
            auto key = PEM_read_bio_RSAPrivateKey(bio, nullptr,
                                                  &pem_password_cb, this);
            auto ret = SSL_CTX_use_RSAPrivateKey(ctx, key);
            if (ret != 1) {
                ERR_error_string_n(ret, errbuf, sizeof(errbuf));
                LOG_ERROR_RETURN(0, -1, errbuf);
            }
            ret = SSL_CTX_check_private_key(ctx);
            if (ret != 1) {
                ERR_error_string_n(ret, errbuf, sizeof(errbuf));
                LOG_ERROR_RETURN(0, -1, errbuf);
            }
        } else
            return -1;
        return 0;
    }
};

TLSContext* new_tls_context(const char* cert_str, const char* key_str,
                            const char* passphrase) {
    auto ret = new TLSContext();
    if (ret->ctx == NULL) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to create TLS context");
    }
    if (ret->ssl_set_cert(cert_str)) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to set TLS cert");
    };
    if (ret->ssl_set_pkey(key_str, passphrase)) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to set TLS pkey");
    }
    return ret;
}

void delete_tls_context(TLSContext* ctx) { delete ctx; }

class TLSStream : public net::ISocketStream {
public:
    SecurityRole role;
    net::ISocketStream* underlay_stream;
    bool ownership;
    photon::mutex rmtx, wmtx;

    SSL* ssl;
    BIO* rbio;
    BIO* wbio;

    TLSStream(TLSContext* ctx, ISocketStream* stream, SecurityRole r,
              bool ownership = false)
        : role(r), underlay_stream(stream), ownership(ownership) {
        ssl = SSL_new(ctx->ctx);
        rbio = BIO_new(BIO_s_mem());
        wbio = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl, rbio, wbio);
    }

    ~TLSStream() {
        SSL_free(ssl);
        if (ownership) {
            delete underlay_stream;
        }
    }
    int do_write() {
        char* buf;
        BUF_MEM* bufptr;
        ssize_t buflen;
        int n = 0;
        if (!BIO_eof(wbio)) {
            BIO_get_mem_ptr(wbio, &bufptr);
            buflen = bufptr->length;
            buf = bufptr->data;
            n = underlay_stream->write(buf, buflen);
            if (n != buflen) {
                LOG_ERROR_RETURN(ECONNRESET, -1, "Failed to send out all data");
            }
            bufptr->length = 0;
        };
        return n;
    }

    int do_read() {
        char buf[BUFFER_SIZE];
        int ret, n;
        if (BIO_eof(rbio)) {
            n = underlay_stream->recv(buf, BUFFER_SIZE);
            ERRNO err;
            if (n > 0) {
                ret = BIO_write(rbio, buf, n);
                if (ret < 0) {
                    char errbuf[MAX_ERRSTRING_SIZE];
                    auto e = SSL_get_error(ssl, ret);
                    ERR_error_string_n(e, errbuf, MAX_ERRSTRING_SIZE);
                    LOG_ERROR_RETURN(ECONNRESET, ret, errbuf);
                }
            } else {
                return n;
            }
        };
        return 0;
    }

    template <typename Action>
    int do_io_action(Action&& action) {
        int ret;
        while (1) {
            ret = action(ssl);
            if (do_write() < 0) return -1;
            if (ret >= 0) {
                return ret;
            } else {
                auto e = SSL_get_error(ssl, ret);
                switch (e) {
                    case SSL_ERROR_WANT_WRITE:
                    case SSL_ERROR_WANT_READ:
                        ret = do_read();
                        if (ret < 0) return ret;
                        break;
                    default:
                        return ret;
                }
            }
        }
    }

    inline int get_ready() {
        switch (role) {
            case SecurityRole::Client:
                return do_io_action([](SSL* ssl) { return SSL_connect(ssl); });
            case SecurityRole::Server:
                return do_io_action([](SSL* ssl) { return SSL_accept(ssl); });
            default:
                LOG_ERROR_RETURN(EINVAL, -EINVAL,
                                 "Role not set before io action");
        }
    }

    template <typename Action>
    int get_ready_and_do_io_action(Action&& action) {
        int ret;
        if (!SSL_is_init_finished(ssl)) {
            ret = get_ready();
            if (ret != 1) {
                char errbuf[4096];
                auto e = SSL_get_error(ssl, ret);
                ERR_error_string_n(e, errbuf, 4096);
                LOG_ERROR_RETURN(-ret, ret, errbuf);
            }
        }
        return do_io_action(std::move(action));
    }

    ssize_t recv(void* buf, size_t cnt) override {
        return get_ready_and_do_io_action(
            [&](SSL* ssl) -> int { return SSL_read(ssl, buf, cnt); });
    }

    ssize_t recv(const struct iovec* iov, int iovcnt) override {
        // since recv allows partial read
        return recv(iov[0].iov_base, iov[0].iov_len);
    }
    ssize_t send(const void* buf, size_t cnt) override {
        return get_ready_and_do_io_action(
            [&](SSL* ssl) -> int { return SSL_write(ssl, buf, cnt); });
    }
    ssize_t send(const struct iovec* iov, int iovcnt) override {
        // since send allows partial write
        return send(iov[0].iov_base, iov[0].iov_len);
    }

    template <typename IOCB>
    __FORCE_INLINE__ ssize_t doio_n(void*& buf, size_t& count, IOCB iocb) {
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
    __FORCE_INLINE__ ssize_t doiov_n(iovector_view& v, IOCB iocb) {
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

    ssize_t write(const void* buf, size_t cnt) override {
        return doio_n((void*&)buf, cnt,
                      [&]() __INLINE__ { return send(buf, cnt); });
    }

    ssize_t writev(const struct iovec* iov, int iovcnt) override {
        if (iovcnt == 1) return write(iov->iov_base, iov->iov_len);
        SmartCloneIOV<32> ciov(iov, iovcnt);
        iovector_view v(ciov.ptr, iovcnt);
        return doiov_n(v, [&] { return send(v.iov, v.iovcnt); });
    }

    ssize_t read(void* buf, size_t cnt) override {
        return doio_n((void*&)buf, cnt,
                      [&]() __INLINE__ { return recv(buf, cnt); });
    }

    ssize_t readv(const struct iovec* iov, int iovcnt) override {
        if (iovcnt == 1) return read(iov->iov_base, iov->iov_len);
        SmartCloneIOV<32> ciov(iov, iovcnt);
        iovector_view v(ciov.ptr, iovcnt);
        return doiov_n(v, [&] { return recv(v.iov, v.iovcnt); });
    }

    ssize_t send2(const void* buf, size_t cnt, int flags) override {
        return send(buf, cnt);
    }
    ssize_t send2(const struct iovec* iov, int iovcnt, int flags) override {
        return send(iov, iovcnt);
    }

    ssize_t sendfile(int fd, off_t offset, size_t size) override {
        // SSL not supported
        return -2;
    }

    int close() override {
        if (ownership)
            return underlay_stream->close();
        else
            return 0;
    }

    virtual int getsockname(net::EndPoint& addr) override {
        return underlay_stream->getsockname(addr);
    }
    virtual int getpeername(net::EndPoint& addr) override {
        return underlay_stream->getpeername(addr);
    }
    virtual int getsockname(char* path, size_t count) override {
        return underlay_stream->getsockname(path, count);
    }
    virtual int getpeername(char* path, size_t count) override {
        return underlay_stream->getpeername(path, count);
    }
    virtual int setsockopt(int level, int option_name, const void* option_value,
                           socklen_t option_len) override {
        return underlay_stream->setsockopt(level, option_name, option_value,
                                           option_len);
    }
    virtual int getsockopt(int level, int option_name, void* option_value,
                           socklen_t* option_len) override {
        return underlay_stream->getsockopt(level, option_name, option_value,
                                           option_len);
    }
    virtual uint64_t timeout() override { return underlay_stream->timeout(); }
    virtual void timeout(uint64_t tm) override { underlay_stream->timeout(tm); }
    virtual Object* get_underlay_object(int level) override {
        return level ? underlay_stream->get_underlay_object(level - 1) : nullptr;
    }
};

net::ISocketStream* new_tls_stream(TLSContext* ctx, net::ISocketStream* base,
                                   SecurityRole role, bool ownership) {
    if (!ctx || !base)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid parameters, ", VALUE(ctx),
                         VALUE(base));
    LOG_DEBUG("New tls stream on ", VALUE(ctx), VALUE(base));
    return new TLSStream(ctx, base, role, ownership);
};

class TLSClient : public net::ISocketClient {
public:
    TLSContext* ctx;
    net::ISocketClient* underlay;
    bool ownership;

    TLSClient(TLSContext* ctx, net::ISocketClient* underlay, bool ownership)
        : ctx(ctx), underlay(underlay), ownership(ownership) {}

    ~TLSClient() {
        if (ownership) {
            delete underlay;
        }
    }
    virtual Object* get_underlay_object(int level) override {
        return level ? underlay->get_underlay_object(level - 1) : nullptr;
    }
    virtual net::ISocketStream* connect(const net::EndPoint& ep) override {
        return new_tls_stream(ctx, underlay->connect(ep), SecurityRole::Client,
                              true);
    }
    virtual net::ISocketStream* connect(const char* path,
                                        size_t count) override {
        return new_tls_stream(ctx, underlay->connect(path, count),
                              SecurityRole::Client, true);
    }
    virtual int getsockname(net::EndPoint& addr) override {
        return underlay->getsockname(addr);
    }
    virtual int getpeername(net::EndPoint& addr) override {
        return underlay->getpeername(addr);
    }
    virtual int getsockname(char* path, size_t count) override {
        return underlay->getsockname(path, count);
    }
    virtual int getpeername(char* path, size_t count) override {
        return underlay->getpeername(path, count);
    }
    virtual int setsockopt(int level, int option_name, const void* option_value,
                           socklen_t option_len) override {
        return underlay->setsockopt(level, option_name, option_value,
                                    option_len);
    }
    virtual int getsockopt(int level, int option_name, void* option_value,
                           socklen_t* option_len) override {
        return underlay->getsockopt(level, option_name, option_value,
                                    option_len);
    }
    virtual uint64_t timeout() override { return underlay->timeout(); }
    virtual void timeout(uint64_t tm) override { underlay->timeout(tm); }
};

net::ISocketClient* new_tls_client(TLSContext* ctx, net::ISocketClient* base,
                                   bool ownership) {
    if (!ctx || !base)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid parameters, ", VALUE(ctx),
                         VALUE(base));
    return new TLSClient(ctx, base, ownership);
}

class TLSServer : public net::ISocketServer {
public:
    TLSContext* ctx;
    net::ISocketServer* underlay;
    Handler m_handler;
    bool ownership;

    TLSServer(TLSContext* ctx, net::ISocketServer* underlay, bool ownership)
        : ctx(ctx), underlay(underlay), ownership(ownership) {}

    ~TLSServer() {
        if (ownership) {
            delete underlay;
        }
    }
    virtual Object* get_underlay_object(int level) override {
        return level ? underlay->get_underlay_object(level - 1) : nullptr;
    }
    virtual net::ISocketStream* accept() override {
        return new_tls_stream(ctx, underlay->accept(), SecurityRole::Server,
                              true);
    }
    virtual net::ISocketStream* accept(
        net::EndPoint* remote_endpoint) override {
        return new_tls_stream(ctx, underlay->accept(remote_endpoint),
                              SecurityRole::Server, true);
    }
    virtual int bind(uint16_t port, net::IPAddr addr) override {
        return underlay->bind(port, addr);
    }
    virtual int bind(const char* path, size_t count) override {
        return underlay->bind(path, count);
    }
    virtual int listen(int backlog = 1024) override {
        return underlay->listen(backlog);
    }
    int forwarding_handler(net::ISocketStream* stream) {
        auto s = new_tls_stream(ctx, stream, SecurityRole::Server, false);
        DEFER(delete s);
        return m_handler(s);
    }
    virtual net::ISocketServer* set_handler(Handler handler) override {
        m_handler = handler;
        return underlay->set_handler({this, &TLSServer::forwarding_handler});
    }
    virtual int start_loop(bool block = false) override {
        return underlay->start_loop(block);
    }
    virtual void terminate() override { return underlay->terminate(); }
    virtual int getsockname(net::EndPoint& addr) override {
        return underlay->getsockname(addr);
    }
    virtual int getpeername(net::EndPoint& addr) override {
        return underlay->getpeername(addr);
    }
    virtual int getsockname(char* path, size_t count) override {
        return underlay->getsockname(path, count);
    }
    virtual int getpeername(char* path, size_t count) override {
        return underlay->getpeername(path, count);
    }
    virtual int setsockopt(int level, int option_name, const void* option_value,
                           socklen_t option_len) override {
        return underlay->setsockopt(level, option_name, option_value,
                                    option_len);
    }
    virtual int getsockopt(int level, int option_name, void* option_value,
                           socklen_t* option_len) override {
        return underlay->getsockopt(level, option_name, option_value,
                                    option_len);
    }
    virtual uint64_t timeout() override { return underlay->timeout(); }
    virtual void timeout(uint64_t tm) override { underlay->timeout(tm); }
};

net::ISocketServer* new_tls_server(TLSContext* ctx, net::ISocketServer* base,
                                   bool ownership) {
    if (!ctx || !base)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid parameters, ", VALUE(ctx),
                         VALUE(base));
    return new TLSServer(ctx, base, ownership);
}

}  // namespace Security
}
