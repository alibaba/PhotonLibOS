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

#include "sasl-stream.h"

#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/net/socket.h>

#include <error.h>

namespace photon {
namespace net {

enum class SecurityRole {
    Client = 1,
    Server = 2,
};
constexpr size_t TOKEN_SIZE = 8 * 1024;

static int delegate_callback(Gsasl *ctx, Gsasl_session *sctx, Gsasl_property prop) {
    Gsasl_prep_cb *cb = (Gsasl_prep_cb *)(gsasl_callback_hook_get(ctx));
    return cb->fire(ctx, sctx, prop);
}

class SaslSession {
  public:
    Gsasl *ctx = nullptr;
    Gsasl_session *session = nullptr;
    const char *mech;
    SecurityRole role;
    Gsasl_auth_cb auth_cb;
    Gsasl_prep_cb prep_cb;
    bool inited = false;

    SaslSession(const char *mech, SecurityRole r, Gsasl_auth_cb auth_cb, Gsasl_prep_cb prep_cb)
        : mech(mech), role(r), auth_cb(auth_cb), prep_cb(prep_cb) {
        inited = initGsaslCtx();
    }
    SaslSession(const SaslSession &) = delete;
    SaslSession(SaslSession &&) = delete;
    SaslSession &operator=(const SaslSession &) = delete;
    SaslSession &operator=(SaslSession &&) = delete;
    ~SaslSession() {
        if (inited) {
            gsasl_finish(session);
            gsasl_done(ctx);
            inited = false;
        }
    }

  private:
    bool initGsaslCtx() {
        int rc = gsasl_init(&ctx);
        if (rc != GSASL_OK) {
            LOG_ERROR_RETURN(0, false, "Cannot initialize libgsasl (`): `", rc, gsasl_strerror(rc));
        }
        gsasl_callback_hook_set(ctx, &prep_cb);
        gsasl_callback_set(ctx, delegate_callback);
        if (role == SecurityRole::Client) {
            rc = gsasl_client_start(ctx, mech, &session);
        } else if (role == SecurityRole::Server) {
            rc = gsasl_server_start(ctx, mech, &session);
        } else {
            LOG_ERROR_RETURN(EINVAL, false, "Incorrect Role");
        }
        if (rc != GSASL_OK) {
            LOG_ERROR_RETURN(0, false, "Cannot initialize ` (`): `",
                             role == SecurityRole::Client ? "client" : "server", rc,
                             gsasl_strerror(rc));
        }
        return true;
    }
};

SaslSession *new_sasl_client_session(const char *mech, Gsasl_auth_cb auth_cb,
                                     Gsasl_prep_cb prep_cb) {
    SaslSession *ret = new SaslSession(mech, SecurityRole::Client, auth_cb, prep_cb);
    if (!ret->inited) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to create Sasl Client Session");
    }
    return ret;
}

SaslSession *new_sasl_server_session(const char *mech, Gsasl_auth_cb auth_cb,
                                     Gsasl_prep_cb prep_cb) {
    SaslSession *ret = new SaslSession(mech, SecurityRole::Server, auth_cb, prep_cb);
    if (!ret->inited) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to create Sasl Server Session");
    }
    return ret;
}

void gsasl_property_set_session(SaslSession *session, Gsasl_property prop, const char *data) {
    gsasl_property_set(session->session, prop, data);
}

void delete_sasl_context(SaslSession *session) { delete session; }

class SaslStream : public net::ISocketStream {
  private:
    SaslSession *sasl_session;
    net::ISocketStream *underlay_stream;
    bool m_ownership;
    Gsasl_qop qop = Gsasl_qop::GSASL_QOP_AUTH;
    char *saslmsg;
    size_t saslmsg_size;
    char *decodebuf = nullptr;
    size_t decodebuf_start = 0;
    size_t decodebuf_finish = 0;

  public:
    SaslStream(SaslSession *session, net::ISocketStream *stream, bool ownership)
        : sasl_session(session), underlay_stream(stream), m_ownership(ownership) {
        saslmsg = (char *)malloc(TOKEN_SIZE);
        saslmsg_size = TOKEN_SIZE;
    }

    ~SaslStream() {
        if (m_ownership) {
            delete underlay_stream;
        }
        free(saslmsg);
        gsasl_free(decodebuf);
    }

    bool initSasl() {
        int rc = sasl_session->auth_cb(sasl_session->session, underlay_stream);
        if (rc != GSASL_OK)
            LOG_ERROR_RETURN(0, false, "Failed to setup SaslConnection!, `", rc);
        // This is for data integrity and privacy protection in DIGEST-MD5, which is not fully
        // supported in libgsasl. Therefore, user should explicitly set GSASL_QOP(in client) and
        // GSASL_QOPS(in server) in session's callback so that future data transportation can be
        // encoded and decoded correctly.
        const char *qop_c = gsasl_property_fast(
            sasl_session->session,
            sasl_session->role == SecurityRole::Client ? GSASL_QOP : GSASL_QOPS);
        if (qop_c && strcmp(qop_c, "qop-int") == 0) {
            qop = GSASL_QOP_AUTH_INT;
        } else if (qop_c && strcmp(qop_c, "qop-conf") == 0) {
            qop = GSASL_QOP_AUTH_CONF;
        }
        LOG_DEBUG("SaslConnection setup!");
        return true;
    }

    ssize_t recv(void *buf, size_t cnt) override { return do_recv(buf, cnt); }

    ssize_t recv(const struct iovec *iov, int iovcnt) override {
        // since recv allows partial read
        return recv(iov[0].iov_base, iov[0].iov_len);
    }
    ssize_t send(const void *buf, size_t cnt) override { return do_send(buf, cnt); }
    ssize_t send(const struct iovec *iov, int iovcnt) override {
        // since send allows partial write
        return send(iov[0].iov_base, iov[0].iov_len);
    }

    ssize_t write(const void *buf, size_t cnt) override {
        return doio_n((void *&)buf, cnt, [&]() __INLINE__ { return send(buf, cnt); });
    }

    ssize_t writev(const struct iovec *iov, int iovcnt) override {
        ssize_t count = 0;
        for (auto v : iovector_view((iovec *)iov, iovcnt)) {
            auto ret = write(v.iov_base, v.iov_len);
            if (ret <= 0) return ret;
            count += ret;
        }
        return count;
    }

    ssize_t read(void *buf, size_t cnt) override {
        return doio_n((void *&)buf, cnt, [&]() __INLINE__ { return recv(buf, cnt); });
    }

    ssize_t readv(const struct iovec *iov, int iovcnt) override {
        ssize_t count = 0;
        for (auto v : iovector_view((iovec *)iov, iovcnt)) {
            auto ret = read(v.iov_base, v.iov_len);
            if (ret <= 0) return ret;
            count += ret;
        }
        return count;
    }

    ssize_t send2(const void *buf, size_t cnt, int flags) override { return send(buf, cnt); }
    ssize_t send2(const struct iovec *iov, int iovcnt, int flags) override {
        return send(iov, iovcnt);
    }

    ssize_t sendfile(int fd, off_t offset, size_t size) override {
        // SASL not supported
        LOG_ERROR_RETURN(ENOSYS, -1, "Not implemented.");
    }

    int close() override {
        if (m_ownership)
            return underlay_stream->close();
        else
            return 0;
    }

    virtual int getsockname(net::EndPoint &addr) override {
        return underlay_stream->getsockname(addr);
    }
    virtual int getpeername(net::EndPoint &addr) override {
        return underlay_stream->getpeername(addr);
    }
    virtual int getsockname(char *path, size_t count) override {
        return underlay_stream->getsockname(path, count);
    }
    virtual int getpeername(char *path, size_t count) override {
        return underlay_stream->getpeername(path, count);
    }
    virtual int setsockopt(int level, int option_name, const void *option_value,
                           socklen_t option_len) override {
        return underlay_stream->setsockopt(level, option_name, option_value, option_len);
    }
    virtual int getsockopt(int level, int option_name, void *option_value,
                           socklen_t *option_len) override {
        return underlay_stream->getsockopt(level, option_name, option_value, option_len);
    }
    virtual uint64_t timeout() override { return underlay_stream->timeout(); }
    virtual void timeout(uint64_t tm) override { underlay_stream->timeout(tm); }

  private:
    template <typename IOCB> __FORCE_INLINE__ ssize_t doio_n(void *&buf, size_t &count, IOCB iocb) {
        auto count0 = count;
        while (count > 0) {
            ssize_t ret = iocb();
            if (ret <= 0)
                return ret;
            (char *&)buf += ret;
            count -= ret;
        }
        return count0;
    }

    int do_send(const void *buf, size_t cnt) {
        if (qop == Gsasl_qop::GSASL_QOP_AUTH) {
            return underlay_stream->send(buf, cnt);
        }

        char *output = nullptr;
        size_t outlen = 0;
        int rc = gsasl_encode(sasl_session->session, static_cast<const char *>(buf), cnt, &output,
                              &outlen);
        DEFER({ gsasl_free(output); });
        if (rc != GSASL_OK) {
            LOG_ERROR_RETURN(0, -1, "Failed to encode data (`): `", rc, gsasl_strerror(rc));
        }
        // LOG_DEBUG("encode, input: `, ouput: ` `", (char *)buf, outlen, output);
        int ret = underlay_stream->write(output, outlen);
        if (ret != static_cast<int>(outlen)) {
            LOG_ERROR_RETURN(ECONNRESET, -1, "Failed to send out all data, datalen: `, ret: `",
                             outlen, ret);
        }

        return cnt;
    }

    ssize_t read_more(void *userbuf, size_t cnt) {
        // The leading four cotet field represents the length of SASL contents as defined in RFC
        // 2222.
        ssize_t ret = underlay_stream->read(saslmsg, 4);
        if (ret != 4) {
            LOG_ERROR_RETURN(0, -1, "Failed to read length of saslmsg, ret: `", ret);
        }
        size_t len = ntohl(*(uint32_t *)saslmsg);
        if (len + 4 > saslmsg_size) {
            saslmsg_size = len + 4;
            saslmsg = (char *)realloc(saslmsg, saslmsg_size);
        }
        ret = underlay_stream->read(saslmsg + 4, len);
        if (ret != static_cast<ssize_t>(len)) {
            LOG_ERROR_RETURN(0, -1, "Incorrect saslmsg size, ret: `, should be `", ret, len);
        }
        int rc =
            gsasl_decode(sasl_session->session, saslmsg, len + 4, &decodebuf, &decodebuf_finish);
        if (rc != GSASL_OK) {
            LOG_ERROR_RETURN(0, -1, "Failed to decode data (`): `", rc, gsasl_strerror(rc));
        }
        // LOG_DEBUG("decode, input: `, ouput: ` `", decodebuf, decodebuf_finish, outputbuf);
        if (cnt > decodebuf_finish) cnt = decodebuf_finish;
        memcpy(userbuf, decodebuf, cnt); // copy to user buffer

        return decodebuf_start = cnt;
    }

    ssize_t do_recv(void *buf, size_t cnt) {
        if (qop == Gsasl_qop::GSASL_QOP_AUTH) {
            return underlay_stream->recv(buf, cnt);
        }

        if (decodebuf_start == decodebuf_finish) { // no data left in decodebuf
            gsasl_free(decodebuf);
            decodebuf = nullptr;
            return read_more(buf, cnt);
        }
        // read from decodebuf
        ssize_t ret = decodebuf_finish - decodebuf_start;
        if (static_cast<ssize_t>(cnt) < ret) {
            ret = cnt;
        }
        memcpy(buf, decodebuf + decodebuf_start, ret);
        decodebuf_start += ret;

        return ret;
    }
};

ISocketStream *new_sasl_stream(SaslSession *session, net::ISocketStream *stream,
                                    bool ownership) {
    auto ret = new SaslStream(session, stream, ownership);
    if (ret->initSasl()) return ret;
    return nullptr;
}

class SaslClient : public net::ISocketClient {
  public:
    SaslSession *session;
    net::ISocketClient *underlay;
    bool ownership;

    SaslClient(SaslSession *session, net::ISocketClient *underlay, bool ownership)
        : session(session), underlay(underlay), ownership(ownership) {}

    ~SaslClient() {
        if (ownership) {
            delete underlay;
        }
    }
    virtual net::ISocketStream *connect(const net::EndPoint &ep) override {
        return new_sasl_stream(session, underlay->connect(ep), true);
    }
    virtual net::ISocketStream *connect(const char *path, size_t count) override {
        return new_sasl_stream(session, underlay->connect(path, count), true);
    }
    virtual int getsockname(net::EndPoint &addr) override { return underlay->getsockname(addr); }
    virtual int getpeername(net::EndPoint &addr) override { return underlay->getpeername(addr); }
    virtual int getsockname(char *path, size_t count) override {
        return underlay->getsockname(path, count);
    }
    virtual int getpeername(char *path, size_t count) override {
        return underlay->getpeername(path, count);
    }
    virtual int setsockopt(int level, int option_name, const void *option_value,
                           socklen_t option_len) override {
        return underlay->setsockopt(level, option_name, option_value, option_len);
    }
    virtual int getsockopt(int level, int option_name, void *option_value,
                           socklen_t *option_len) override {
        return underlay->getsockopt(level, option_name, option_value, option_len);
    }
    virtual uint64_t timeout() override { return underlay->timeout(); }
    virtual void timeout(uint64_t tm) override { underlay->timeout(tm); }
};

ISocketClient *new_sasl_client(SaslSession *session, net::ISocketClient *base,
                                    bool ownership) {
    if (!session || !base || session->role != SecurityRole::Client)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid parameters, ", VALUE(session), VALUE(base));
    return new SaslClient(session, base, ownership);
}

class SaslServer : public net::ISocketServer {
  public:
    SaslSession *session;
    net::ISocketServer *underlay;
    Handler m_handler;
    bool ownership;

    SaslServer(SaslSession *session, net::ISocketServer *underlay, bool ownership)
        : session(session), underlay(underlay), ownership(ownership) {}

    ~SaslServer() {
        if (ownership) {
            delete underlay;
        }
    }
    virtual net::ISocketStream *accept() override {
        return new_sasl_stream(session, underlay->accept(), true);
    }
    virtual net::ISocketStream *accept(net::EndPoint *remote_endpoint) override {
        return new_sasl_stream(session, underlay->accept(remote_endpoint), true);
    }
    virtual int bind(uint16_t port, net::IPAddr addr) override {
        return underlay->bind(port, addr);
    }
    virtual int bind(const char *path, size_t count) override {
        return underlay->bind(path, count);
    }
    virtual int listen(int backlog = 1024) override { return underlay->listen(backlog); }
    int forwarding_handler(net::ISocketStream *stream) {
        return m_handler(new_sasl_stream(session, stream, true));
    }
    virtual net::ISocketServer *set_handler(Handler handler) override {
        m_handler = handler;
        return underlay->set_handler({this, &SaslServer::forwarding_handler});
    }
    virtual int start_loop(bool block = false) override { return underlay->start_loop(block); }
    virtual void terminate() override { return underlay->terminate(); }
    virtual int getsockname(net::EndPoint &addr) override { return underlay->getsockname(addr); }
    virtual int getpeername(net::EndPoint &addr) override { return underlay->getpeername(addr); }
    virtual int getsockname(char *path, size_t count) override {
        return underlay->getsockname(path, count);
    }
    virtual int getpeername(char *path, size_t count) override {
        return underlay->getpeername(path, count);
    }
    virtual int setsockopt(int level, int option_name, const void *option_value,
                           socklen_t option_len) override {
        return underlay->setsockopt(level, option_name, option_value, option_len);
    }
    virtual int getsockopt(int level, int option_name, void *option_value,
                           socklen_t *option_len) override {
        return underlay->getsockopt(level, option_name, option_value, option_len);
    }
    virtual uint64_t timeout() override { return underlay->timeout(); }
    virtual void timeout(uint64_t tm) override { underlay->timeout(tm); }
};

ISocketServer *new_sasl_server(SaslSession *session, net::ISocketServer *base,
                                    bool ownership) {
    if (!session || !base || session->role != SecurityRole::Server)
        LOG_ERROR_RETURN(EINVAL, nullptr, "invalid parameters, ", VALUE(session), VALUE(base));
    return new SaslServer(session, base, ownership);
}

}
}
