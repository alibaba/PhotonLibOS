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
#include <photon/net/socket.h>
#include <photon/net/basic_socket.h>
#include <photon/thread/thread.h>

#include "../base_socket.h"

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
        auto bio = BIO_new_mem_buf((void*)cert_str, -1);
        DEFER(BIO_free(bio));
        auto cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        auto ret = SSL_CTX_use_certificate(ctx, cert);
        if (ret != 1) {
            ERR_error_string_n(ret, errbuf, sizeof(errbuf));
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
        return 0;
    }
    int ssl_set_pkey(const char* key_str, const char* passphrase) {
        char errbuf[4096];
        if (passphrase) {
            SSL_CTX_set_default_passwd_cb(ctx, &pem_password_cb);
            SSL_CTX_set_default_passwd_cb_userdata(ctx, this);
            set_pass_phrase(passphrase);
        }
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
        return 0;
    }
};

TLSContext* new_tls_context(const char* cert_str, const char* key_str,
                            const char* passphrase) {
    GlobalSSLContext::getInstance();
    auto ret = new TLSContext();
    if (ret->ctx == NULL) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to create TLS context");
    }
    if (cert_str && ret->ssl_set_cert(cert_str)) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to set TLS cert");
    };
    if (key_str && ret->ssl_set_pkey(key_str, passphrase)) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to set TLS pkey");
    }
    return ret;
}

void delete_tls_context(TLSContext* ctx) { delete ctx; }

class TLSSocketStream : public ForwardSocketStream {
public:
    SecurityRole role;
    photon::mutex rmtx, wmtx;

    SSL* ssl;
    BIO* rbio;
    BIO* wbio;

    TLSSocketStream(TLSContext* ctx, ISocketStream* stream, SecurityRole r, bool ownership = false)
        : ForwardSocketStream(stream, ownership), role(r) {
        ssl = SSL_new(ctx->ctx);
        rbio = BIO_new(BIO_s_mem());
        wbio = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl, rbio, wbio);
    }

    ~TLSSocketStream() {
        SSL_free(ssl);
    }

    // dump all wbio buffer to socket
    // return failure if write failed
    int do_write() {
        char* buf;
        BUF_MEM* bufptr;
        ssize_t buflen;
        int n = 0;
        if (!BIO_eof(wbio)) {
            BIO_get_mem_ptr(wbio, &bufptr);
            buflen = bufptr->length;
            buf = bufptr->data;
            n = m_underlay->write(buf, buflen);
            if (n != buflen) {
                LOG_ERROR_RETURN(ECONNRESET, -1, "Failed to send out all data");
            }
            bufptr->length = 0;
        };
        return n;
    }

    // recv from socket and put into rbio
    int do_read() {
        char buf[BUFFER_SIZE];
        int ret, n;
        n = m_underlay->recv(buf, BUFFER_SIZE);
        if (n > 0) {
            ret = BIO_write(rbio, buf, n);
            if (ret < 0) {
                char errbuf[MAX_ERRSTRING_SIZE];
                auto e = SSL_get_error(ssl, ret);
                ERR_error_string_n(e, errbuf, MAX_ERRSTRING_SIZE);
                LOG_ERROR_RETURN(ECONNRESET, ret, errbuf);
            }
        }
        return n;
    }

    template <typename Action>
    int do_io_action(Action&& action) {
        int ret;
        while (1) {
            // read or write to buffered bio
            ret = action(ssl);
            // flush all writable content
            // always keep wbio clean
            int wret;
            do {
                wret = do_write();
            } while (wret > 0);
            // return failure if flush failed
            if (wret < 0) return -1;
            
            if (ret >= 0) {
                // succeed
                // if operation is write, 
                return ret;
            } else {
                auto e = SSL_get_error(ssl, ret);
                switch (e) {
                    case SSL_ERROR_WANT_WRITE:
                        // since alread flushed
                        // just go another round
                        break;
                    case SSL_ERROR_WANT_READ:
                        // need to read more into rbio
                        ret = do_read();
                        // if closed or failed, return failure
                        if (ret <= 0) return ret;
                        break;
                    default:
                        // other error, return failure
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

    ssize_t recv(void* buf, size_t cnt, int flags = 0) override {
        return get_ready_and_do_io_action(
            [&](SSL* ssl) -> int { return SSL_read(ssl, buf, cnt); });
    }

    ssize_t recv(const struct iovec* iov, int iovcnt, int flags = 0) override {
        // since recv allows partial read
        return recv(iov[0].iov_base, iov[0].iov_len);
    }
    ssize_t send(const void* buf, size_t cnt, int flags = 0) override {
        return get_ready_and_do_io_action(
            [&](SSL* ssl) -> int { return SSL_write(ssl, buf, cnt); });
    }
    ssize_t send(const struct iovec* iov, int iovcnt, int flags = 0) override {
        // since send allows partial write
        return send(iov[0].iov_base, iov[0].iov_len);
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

    ssize_t sendfile(int fd, off_t offset, size_t size) override {
        // SSL not supported
        return -2;
    }

    int close() override {
        if (m_ownership)
            return m_underlay->close();
        else
            return 0;
    }
};

ISocketStream* new_tls_stream(TLSContext* ctx, ISocketStream* base,
                                   SecurityRole role, bool ownership) {
    if (!ctx || !base)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid parameters, ", VALUE(ctx),
                         VALUE(base));
    LOG_DEBUG("New tls stream on ", VALUE(ctx), VALUE(base));
    return new TLSSocketStream(ctx, base, role, ownership);
};

class TLSSocketClient : public ForwardSocketClient {
public:
    TLSContext* ctx;

    TLSSocketClient(TLSContext* ctx, ISocketClient* underlay, bool ownership)
            : ForwardSocketClient(underlay, ownership), ctx(ctx) {}

    virtual ISocketStream* connect(const char* path, size_t count) override {
        return new_tls_stream(ctx, m_underlay->connect(path, count), SecurityRole::Client, true);
    }

    virtual ISocketStream* connect(EndPoint remote, EndPoint local = EndPoint()) override {
        return new_tls_stream(ctx, m_underlay->connect(remote, local), SecurityRole::Client, true);
    }
};

ISocketClient* new_tls_client(TLSContext* ctx, ISocketClient* base,
                                   bool ownership) {
    if (!ctx || !base)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid parameters, ", VALUE(ctx),
                         VALUE(base));
    return new TLSSocketClient(ctx, base, ownership);
}

class TLSSocketServer : public ForwardSocketServer {
public:
    TLSContext* ctx;
    Handler m_handler;

    TLSSocketServer(TLSContext* ctx, ISocketServer* underlay, bool ownership)
            : ForwardSocketServer(underlay, ownership), ctx(ctx) {}

    virtual ISocketStream* accept(EndPoint* remote_endpoint = nullptr) override {
        return new_tls_stream(ctx, m_underlay->accept(remote_endpoint),
                              SecurityRole::Server, true);
    }

    int forwarding_handler(ISocketStream* stream) {
        auto s = new_tls_stream(ctx, stream, SecurityRole::Server, false);
        DEFER(delete s);
        return m_handler(s);
    }

    virtual ISocketServer* set_handler(Handler handler) override {
        m_handler = handler;
        return m_underlay->set_handler({this, &TLSSocketServer::forwarding_handler});
    }
};

ISocketServer* new_tls_server(TLSContext* ctx, ISocketServer* base,
                                   bool ownership) {
    if (!ctx || !base)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid parameters, ", VALUE(ctx),
                         VALUE(base));
    return new TLSSocketServer(ctx, base, ownership);
}

}
}
