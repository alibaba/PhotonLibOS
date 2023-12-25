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

#include "client.h"
#include <bitset>
#include <algorithm>
#include <random>
#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>
#include <photon/common/string_view.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>
#include <photon/net/utils.h>

namespace photon {
namespace net {
namespace http {
static const uint64_t kDNSCacheLife = 3600UL * 1000 * 1000;
static constexpr char USERAGENT[] = "PhotonLibOS_HTTP";


class PooledDialer {
public:
    net::TLSContext* tls_ctx = nullptr;
    bool tls_ctx_ownership;
    std::unique_ptr<ISocketClient> tcpsock;
    std::unique_ptr<ISocketClient> tlssock;
    std::unique_ptr<Resolver> resolver;

    //etsocket seems not support multi thread very well, use tcp_socket now. need to find out why
    PooledDialer(TLSContext *_tls_ctx) :
            tls_ctx(_tls_ctx ? _tls_ctx : new_tls_context(nullptr, nullptr, nullptr)),
            tls_ctx_ownership(_tls_ctx == nullptr),
            resolver(new_default_resolver(kDNSCacheLife)) {
        auto tcp_cli = new_tcp_socket_client();
        auto tls_cli = new_tls_client(tls_ctx, new_tcp_socket_client(), true);
        tcpsock.reset(new_tcp_socket_pool(tcp_cli, -1, true));
        tlssock.reset(new_tcp_socket_pool(tls_cli, -1, true));
    }

    ~PooledDialer() {
        if (tls_ctx_ownership)
            delete tls_ctx;
    }

    ISocketStream* dial(std::string_view host, uint16_t port, bool secure,
                             uint64_t timeout = -1UL);

    template <typename T>
    ISocketStream* dial(const T& x, uint64_t timeout = -1UL) {
        return dial(x.host_no_port(), x.port(), x.secure(), timeout);
    }
};

ISocketStream* PooledDialer::dial(std::string_view host, uint16_t port, bool secure, uint64_t timeout) {
    LOG_DEBUG("Dial to ` `", host, port);
    std::string strhost(host);
    auto ipaddr = resolver->resolve(strhost.c_str());
    if (ipaddr.undefined()) {
        LOG_ERROR_RETURN(0, nullptr, "DNS resolve failed, name = `", host)
    }

    EndPoint ep(ipaddr, port);
    LOG_DEBUG("Connecting ` ssl: `", ep, secure);
    ISocketStream *sock = nullptr;
    if (secure) {
        tlssock->timeout(timeout);
        sock = tlssock->connect(ep);
    } else {
        tcpsock->timeout(timeout);
        sock = tcpsock->connect(ep);
    }
    if (sock) {
        LOG_DEBUG("Connected ` host : ` ssl: ` `", ep, host, secure, sock);
        return sock;
    }
    LOG_ERROR("connection failed, ssl : ` ep : `  host : `", secure, ep, host);
    if (ipaddr.undefined()) LOG_DEBUG("No connectable resolve result");
    // When failed, remove resolved result from dns cache so that following retries can try
    // different ips.
    resolver->discard_cache(strhost.c_str(), ipaddr);
    return nullptr;
}

constexpr uint64_t code3xx() { return 0; }
template<typename...Ts>
constexpr uint64_t code3xx(uint64_t x, Ts...xs)
{
    return (1 << (x-300)) | code3xx(xs...);
}
constexpr static std::bitset<10>
    code_redirect_verb(code3xx(300, 301, 302, 307, 308));

static constexpr size_t kMinimalHeadersSize = 8 * 1024 - 1;
enum RoundtripStatus {
    ROUNDTRIP_SUCCESS,
    ROUNDTRIP_FAILED,
    ROUNDTRIP_REDIRECT,
    ROUNDTRIP_NEED_RETRY,
    ROUNDTRIP_FORCE_RETRY,
    ROUNDTRIP_FAST_RETRY,
};

class ClientImpl : public Client {
public:
    PooledDialer m_dialer;
    CommonHeaders<> m_common_headers;
    ICookieJar *m_cookie_jar;
    ClientImpl(ICookieJar *cookie_jar, TLSContext *tls_ctx) :
        m_dialer(tls_ctx), m_cookie_jar(cookie_jar) { }

    using SocketStream_ptr = std::unique_ptr<ISocketStream>;
    int redirect(Operation* op) {
        if (op->resp.body_size() > 0) {
            op->resp.skip_remain();
        }

        auto location = op->resp.headers["Location"];
        if (location.empty()) {
            LOG_ERROR_RETURN(EINVAL, ROUNDTRIP_FAILED,
                "redirect but has no field location");
        }
        LOG_DEBUG("Redirect to ", location);

        Verb v;
        auto sc = op->status_code - 300;
        if (sc == 3) {  // 303
            v = Verb::GET;
        } else if (sc < 10 && code_redirect_verb[sc]) {
            v = op->req.verb();
        } else {
            LOG_ERROR_RETURN(EINVAL, ROUNDTRIP_FAILED,
                "invalid 3xx status code: ", op->status_code);
        }

        if (op->req.redirect(v, location, op->enable_proxy) < 0) {
            LOG_ERRNO_RETURN(0, ROUNDTRIP_FAILED, "redirect failed");
        }
        return ROUNDTRIP_REDIRECT;
    }

    int concurreny = 0;
    int do_roundtrip(Operation* op, Timeout tmo) {
        concurreny++;
        LOG_DEBUG(VALUE(concurreny));
        DEFER(concurreny--);
        op->status_code = -1;
        if (tmo.timeout() == 0)
            LOG_ERROR_RETURN(ETIMEDOUT, ROUNDTRIP_FAILED, "connection timedout");
        auto &req = op->req;
        auto s = (m_proxy && !m_proxy_url.empty())
                     ? m_dialer.dial(m_proxy_url, tmo.timeout())
                     : m_dialer.dial(req, tmo.timeout());
        if (!s) {
            if (errno == ECONNREFUSED) {
                LOG_ERROR_RETURN(0, ROUNDTRIP_FAST_RETRY, "connection refused")
            }
            LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "connection failed");
        }

        SocketStream_ptr sock(s);
        LOG_DEBUG("Sending request ` `", req.verb(), req.target());
        if (req.send_header(sock.get()) < 0) {
            sock->close();
            req.reset_status();
            LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "send header failed, retry");
        }
        sock->timeout(tmo.timeout());
        if (op->body_stream) {
            // send body_stream
            if (req.write_stream(op->body_stream) < 0) {
                sock->close();
                req.reset_status();
                LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "send body stream failed, retry");
            }
        } else {
            // call body_writer
            if (op->body_writer(&req) < 0) {
                sock->close();
                req.reset_status();
                LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "failed to call body writer, retry");
            }
        }

        if (req.send() < 0) {
            sock->close();
            req.reset_status();
            LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "failed to ensure send");
        }

        LOG_DEBUG("Request sent, wait for response ` `", req.verb(), req.target());
        auto space = req.get_remain_space();
        auto &resp = op->resp;

        if (space.second > kMinimalHeadersSize) {
            resp.reset(space.first, space.second, false, sock.release(), true, req.verb());
        } else {
            auto buf = malloc(kMinimalHeadersSize);
            resp.reset((char *)buf, kMinimalHeadersSize, true, sock.release(), true, req.verb());
        }
        if (op->resp.receive_header(tmo.timeout()) != 0) {
            req.reset_status();
            LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "read response header failed");
        }

        op->status_code = resp.status_code();
        LOG_DEBUG("Got response ` ` code=` || content_length=`", req.verb(),
                  req.target(), resp.status_code(), resp.headers.content_length());
        if (m_cookie_jar) m_cookie_jar->get_cookies_from_headers(req.host(), &resp);
        if (resp.status_code() < 400 && resp.status_code() >= 300 && op->follow)
            return redirect(op);
        return ROUNDTRIP_SUCCESS;
    }

    int call(Operation* /*IN, OUT*/ op) override {
        auto content_length = op->req.headers.content_length();
        auto encoding = op->req.headers["Transfer-Encoding"];
        if ((content_length != 0) && (encoding == "chunked")) {
            op->status_code = -1;
            LOG_ERROR_RETURN(EINVAL, ROUNDTRIP_FAILED,
                            "Content-Length and Transfer-Encoding conflicted");
        }
        op->req.headers.insert("User-Agent", USERAGENT);
        op->req.headers.insert("Connection", "keep-alive");
        op->req.headers.merge(m_common_headers);
        if (m_cookie_jar && m_cookie_jar->set_cookies_to_headers(&op->req) != 0)
            LOG_ERROR_RETURN(0, -1, "set_cookies_to_headers failed");
        Timeout tmo(std::min(op->timeout.timeout(), m_timeout));
        int retry = 0, followed = 0, ret = 0;
        uint64_t sleep_interval = 0;
        while (followed <= op->follow && retry <= op->retry && tmo.timeout() != 0) {
            ret = do_roundtrip(op, tmo);
            if (ret == ROUNDTRIP_SUCCESS || ret == ROUNDTRIP_FAILED) break;
            switch (ret) {
                case ROUNDTRIP_NEED_RETRY:
                    photon::thread_usleep(sleep_interval * 1000UL);
                    sleep_interval = (sleep_interval + 500) * 2;
                    ++retry;
                    break;
                case ROUNDTRIP_FAST_RETRY:
                    ++retry;
                    break;
                case ROUNDTRIP_REDIRECT:
                    retry = 0;
                    ++followed;
                    break;
                default:
                    break;
            }
            if (tmo.timeout() == 0)
                LOG_ERROR_RETURN(ETIMEDOUT, -1, "connection timedout");
            if (followed > op->follow || retry > op->retry)
                LOG_ERRNO_RETURN(0, -1,  "connection failed");
        }
        if (ret != ROUNDTRIP_SUCCESS) LOG_ERROR_RETURN(0, -1,"too many retry, roundtrip failed");
        return 0;
    }

    ISocketStream* native_connect(std::string_view host, uint16_t port, bool secure, uint64_t timeout) override {
        return m_dialer.dial(host, port, secure, timeout);
    }

    CommonHeaders<>* common_headers() override {
        return &m_common_headers;
    }
};

Client* new_http_client(ICookieJar *cookie_jar, TLSContext *tls_ctx) {
    return new ClientImpl(cookie_jar, tls_ctx);
}

} // namespace http
} // namespace net
} // namespace photon
