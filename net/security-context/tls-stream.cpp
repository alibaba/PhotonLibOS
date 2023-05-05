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
#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>
#include <photon/net/basic_socket.h>
#include <photon/net/socket.h>
#include <photon/thread/thread.h>

#include <vector>

#include "../base_socket.h"

namespace photon {
namespace net {

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

    static unsigned long threadid_callback() {
        return (uint64_t)photon::get_vcpu();
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
        CRYPTO_set_id_callback(&GlobalSSLContext ::threadid_callback);
        CRYPTO_set_locking_callback(&GlobalSSLContext::lock_callback);
    }

    ~GlobalSSLContext() {
        CRYPTO_set_id_callback(NULL);
        CRYPTO_set_locking_callback(NULL);
    }
};

class TLSContextImpl : public TLSContext {
public:
    SSL_CTX* ctx;
    char pempassword[MAX_PASSPHASE_SIZE];

    explicit TLSContextImpl(TLSVersion ver) {
        char errbuf[4096];
        const SSL_METHOD *method = nullptr;
        switch (ver) {
            case TLSVersion::SSL23:
                method = SSLv23_method();
                break;
            case TLSVersion::TLS11:
                method = TLSv1_1_method();
                break;
            case TLSVersion::TLS12:
                method = TLSv1_2_method();
                break;
            default:
                method = TLSv1_2_method();
        }
        ctx = SSL_CTX_new(method);
        if (ctx == nullptr) {
            ERR_error_string_n(ERR_get_error(), errbuf, MAX_ERRSTRING_SIZE);
            LOG_ERROR(0, -1, "Failed to initial TLS: ", errbuf);
        }
        SSL_CTX_set_ecdh_auto(ctx, 1);
    }

    ~TLSContextImpl() override {
        if (ctx) SSL_CTX_free(ctx);
    }

    static int pem_password_cb(char* buf, int size, int rwflag, void* ptr) {
        auto self = (TLSContextImpl*)ptr;
        strncpy(buf, self->pempassword, size);
        return strlen(buf);
    }
    int set_pass_phrase(const char* pass) override {
        strncpy(pempassword, pass, sizeof(pempassword));
        return strlen(pempassword);
    }
    int set_cert(const char* cert_str) override {
        char errbuf[4096];
        auto bio = BIO_new_mem_buf((void*)cert_str, -1);
        DEFER(BIO_free(bio));
        auto cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        auto ret = SSL_CTX_use_certificate(ctx, cert);
        if (ret != 1) {
            ERR_error_string_n(ERR_get_error(), errbuf, MAX_ERRSTRING_SIZE);
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
        return 0;
    }
    int set_pkey(const char* key_str, const char* passphrase) override {
        char errbuf[4096];
        if (passphrase) {
            SSL_CTX_set_default_passwd_cb(ctx, &pem_password_cb);
            SSL_CTX_set_default_passwd_cb_userdata(ctx, this);
            set_pass_phrase(passphrase);
        }
        auto bio = BIO_new_mem_buf((void*)key_str, -1);
        DEFER(BIO_free(bio));
        auto key =
            PEM_read_bio_PrivateKey(bio, nullptr, &pem_password_cb, this);
        auto ret = SSL_CTX_use_PrivateKey(ctx, key);
        if (ret != 1) {
            ERR_error_string_n(ERR_get_error(), errbuf, MAX_ERRSTRING_SIZE);
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
        ret = SSL_CTX_check_private_key(ctx);
        if (ret != 1) {
            ERR_error_string_n(ERR_get_error(), errbuf, MAX_ERRSTRING_SIZE);
            LOG_ERROR_RETURN(0, -1, errbuf);
        }
        return 0;
    }
};

void __OpenSSLGlobalInit() { (void)GlobalSSLContext::getInstance(); }

TLSContext* new_tls_context(const char* cert_str, const char* key_str,
                            const char* passphrase, TLSVersion version) {
    __OpenSSLGlobalInit();
    auto ret = new TLSContextImpl(version);
    if (ret->ctx == NULL) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to create TLS context");
    }
    if (cert_str && ret->set_cert(cert_str)) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to set TLS cert");
    };
    if (key_str && ret->set_pkey(key_str, passphrase)) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to set TLS pkey");
    }
    return ret;
}

class TLSSocketStream : public ForwardSocketStream {
public:
    SSL* ssl;
    BIO* ssbio;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    static void* BIO_get_data(BIO* b) { return b->ptr; }

    static void BIO_set_data(BIO* b, void* ptr) { b->ptr = ptr; }

    static int BIO_get_shutdown(BIO* b) { return b->shutdown; }

    static void BIO_set_shutdown(BIO* b, int num) { b->shutdown = num; }

    static int BIO_get_init(BIO* b) { return b->init; }

    static void BIO_set_init(BIO* b, int num) { b->init = num; }

    static BIO_METHOD* BIO_meth_new(int type, const char* name) {
        auto ret = new BIO_METHOD;
        memset(ret, 0, sizeof(BIO_METHOD));
        ret->type = type;
        ret->name = name;
        return ret;
    }

    static void BIO_meth_free(BIO_METHOD* bm) { delete bm; }

    static void BIO_meth_set_write(BIO_METHOD* biom,
                                   int (*write)(BIO*, const char*, int)) {
        biom->bwrite = write;
    }

    static void BIO_meth_set_read(BIO_METHOD* biom,
                                  int (*read)(BIO*, char*, int)) {
        biom->bread = read;
    }

    static void BIO_meth_set_puts(BIO_METHOD* biom,
                                  int (*puts)(BIO*, const char*)) {
        biom->bputs = puts;
    }

    static void BIO_meth_set_ctrl(BIO_METHOD* biom,
                                  long (*ctrl)(BIO*, int, long, void*)) {
        biom->ctrl = ctrl;
    }

    static void BIO_meth_set_create(BIO_METHOD* biom, int (*create)(BIO*)) {
        biom->create = create;
    }

    static void BIO_meth_set_destroy(BIO_METHOD* biom, int (*destroy)(BIO*)) {
        biom->destroy = destroy;
    }

#endif

    struct BIOMethodDeleter {
        void operator()(BIO_METHOD* ptr) { BIO_meth_free(ptr); }
    };

    static BIO_METHOD* BIO_s_sockstream() {
        static std::unique_ptr<BIO_METHOD, BIOMethodDeleter> meth(
            BIO_meth_new(BIO_TYPE_SOURCE_SINK, "BIO_PHOTON_SOCKSTREAM"));
        auto ret = meth.get();
        BIO_meth_set_write(ret, &TLSSocketStream::ssbio_bwrite);
        BIO_meth_set_read(ret, &TLSSocketStream::ssbio_bread);
        BIO_meth_set_puts(ret, &TLSSocketStream::ssbio_bputs);
        BIO_meth_set_ctrl(ret, &TLSSocketStream::ssbio_ctrl);
        BIO_meth_set_create(ret, &TLSSocketStream::ssbio_create);
        BIO_meth_set_destroy(ret, &TLSSocketStream::ssbio_destroy);
        return ret;
    }

    static ISocketStream* get_bio_sockstream(BIO* b) {
        return (ISocketStream*)BIO_get_data(b);
    }

    static int ssbio_bwrite(BIO* b, const char* buf, int cnt) {
        return get_bio_sockstream(b)->write(buf, cnt);
    }

    static int ssbio_bread(BIO* b, char* buf, int cnt) {
        return get_bio_sockstream(b)->recv(buf, cnt);
    }

    static int ssbio_bputs(BIO* bio, const char* str) {
        return ssbio_bwrite(bio, str, strlen(str));
    }

    static long ssbio_ctrl(BIO* b, int cmd, long num, void* ptr) {
        long ret = 1;

        switch (cmd) {
            case BIO_C_SET_FILE_PTR:
                BIO_set_data(b, ptr);
                BIO_set_shutdown(b, num);
                BIO_set_init(b, 1);
                break;
            case BIO_CTRL_GET_CLOSE:
                ret = BIO_get_shutdown(b);
                break;
            case BIO_CTRL_SET_CLOSE:
                BIO_set_shutdown(b, num);
                break;
            case BIO_CTRL_DUP:
            case BIO_CTRL_FLUSH:
                ret = 1;
                break;
            default:
                ret = 0;
                break;
        }
        return (ret);
    }

    static int ssbio_create(BIO* bi) {
        BIO_set_data(bi, nullptr);
        BIO_set_init(bi, 0);
        return (1);
    }

    static int ssbio_destroy(BIO*) { return 1; }

    TLSSocketStream(TLSContext* ctx, ISocketStream* stream, SecurityRole r,
                    bool ownership = false)
        : ForwardSocketStream(stream, ownership) {
        ssl = SSL_new(((TLSContextImpl*)ctx)->ctx);
        ssbio = BIO_new(BIO_s_sockstream());
        BIO_ctrl(ssbio, BIO_C_SET_FILE_PTR, 0, stream);
        SSL_set_bio(ssl, ssbio, ssbio);

        switch (r) {
            case SecurityRole::Client:
                SSL_set_connect_state(ssl);
                break;
            case SecurityRole::Server:
                SSL_set_accept_state(ssl);
                break;
            default:
                break;
        }
    }

    ~TLSSocketStream() {
        close();
        SSL_free(ssl);
    }

    ssize_t recv(void* buf, size_t cnt, int flags = 0) override {
        return SSL_read(ssl, buf, cnt);
    }

    ssize_t recv(const struct iovec* iov, int iovcnt, int flags = 0) override {
        // since recv allows partial read
        return recv(iov[0].iov_base, iov[0].iov_len);
    }
    ssize_t send(const void* buf, size_t cnt, int flags = 0) override {
        return SSL_write(ssl, buf, cnt);
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

    ssize_t sendfile(int fd, off_t offset, size_t count) override {
        return sendfile_fallback(this, fd, offset, count);
    }

    int shutdown(ShutdownHow) override { return SSL_shutdown(ssl); }

    int close() override {
        shutdown(ShutdownHow::ReadWrite);
        if (m_ownership) {
            return m_underlay->close();
        } else
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
        return new_tls_stream(ctx, m_underlay->connect(path, count),
                              SecurityRole::Client, true);
    }

    virtual ISocketStream* connect(EndPoint remote,
                                   EndPoint local = EndPoint()) override {
        return new_tls_stream(ctx, m_underlay->connect(remote, local),
                              SecurityRole::Client, true);
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

    virtual ISocketStream* accept(
        EndPoint* remote_endpoint = nullptr) override {
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
        return m_underlay->set_handler(
            {this, &TLSSocketServer::forwarding_handler});
    }
};

ISocketServer* new_tls_server(TLSContext* ctx, ISocketServer* base,
                              bool ownership) {
    if (!ctx || !base)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid parameters, ", VALUE(ctx),
                         VALUE(base));
    return new TLSSocketServer(ctx, base, ownership);
}

}  // namespace net
}  // namespace photon
