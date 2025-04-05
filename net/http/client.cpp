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
#include <photon/photon.h>

namespace photon {
namespace net {
namespace http {
static const uint64_t kDNSCacheLife = 3600UL * 1000 * 1000;
static constexpr char USERAGENT[] = "PhotonLibOS_HTTP";


class PooledDialer {
public:
    net::TLSContext* tls_ctx = nullptr;
    std::unique_ptr<ISocketClient> tcpsock;
    std::unique_ptr<ISocketClient> tlssock;
    std::unique_ptr<ISocketClient> udssock;
    std::unique_ptr<Resolver> resolver;
    photon::mutex init_mtx;
    bool initialized = false;
    bool tls_ctx_ownership = false;

    // If there is a photon thread switch during construction, the constructor might be called
    // multiple times, even for a thread_local instance. Therefore, ensure that there is no photon
    // thread switch inside the constructor. Place the initialization work in init() and ensure it
    // is initialized only once.
    PooledDialer() {
        photon::fini_hook({this, &PooledDialer::at_photon_fini});
    }

    int init(TLSContext *_tls_ctx, std::vector<IPAddr> &src_ips) {
        if (initialized)
            return 0;
        SCOPED_LOCK(init_mtx);
        if (initialized)
            return 0;
        tls_ctx = _tls_ctx;
        if (!tls_ctx) {
            tls_ctx_ownership = true;
            tls_ctx = new_tls_context(nullptr, nullptr, nullptr);
            tls_ctx->set_verify_mode(VerifyMode::PEER);  // act like curl
        }
        auto tcp_cli = new_tcp_socket_client(src_ips.data(), src_ips.size());
        auto tls_cli = new_tls_client(tls_ctx, new_tcp_socket_client(src_ips.data(), src_ips.size()), true);
        tcpsock.reset(new_tcp_socket_pool(tcp_cli, -1, true));
        tlssock.reset(new_tcp_socket_pool(tls_cli, -1, true));
        udssock.reset(new_uds_client());
        resolver.reset(new_default_resolver(kDNSCacheLife));
        initialized = true;
        return 0;
    }

    void at_photon_fini() {
        resolver.reset();
        udssock.reset();
        tlssock.reset();
        tcpsock.reset();
        if (tls_ctx_ownership)
            delete tls_ctx;
        initialized = false;
        tls_ctx_ownership = false;
    }

    ISocketStream* dial(std::string_view host, uint16_t port, bool secure,
                             uint64_t timeout = -1UL);

    template <typename T>
    ISocketStream* dial(const T& x, uint64_t timeout = -1UL) {
        return dial(x.host_no_port(), x.port(), x.secure(), timeout);
    }

    ISocketStream* dial(std::string_view uds_path, uint64_t timeout = -1UL);
};

ISocketStream* PooledDialer::dial(std::string_view host, uint16_t port, bool secure, uint64_t timeout) {
    LOG_DEBUG("Dialing to `:`", host, port);
    auto ipaddr = resolver->resolve(host);
    if (ipaddr.undefined()) {
        LOG_ERROR_RETURN(ENOENT, nullptr, "DNS resolve failed, name = `", host)
    }

    EndPoint ep(ipaddr, port);
    LOG_DEBUG("Connecting ` ssl: `", ep, secure);
    ISocketStream *sock = nullptr;
    if (secure) {
        tlssock->timeout(timeout);
        sock = tlssock->connect(ep);
        tls_stream_set_hostname(sock, estring_view(host).extract_c_str());
    } else {
        tcpsock->timeout(timeout);
        sock = tcpsock->connect(ep);
    }
    if (sock) {
        LOG_DEBUG("Connected ` ", ep, VALUE(host), VALUE(secure));
        return sock;
    }
    LOG_ERROR("connection failed, ssl : ` ep : `  host : `", secure, ep, host);
    // When failed, remove resolved result from dns cache so that following retries can try
    // different ips.
    resolver->discard_cache(host, ipaddr);
    return nullptr;
}

ISocketStream* PooledDialer::dial(std::string_view uds_path, uint64_t timeout) {
    udssock->timeout(timeout);
    auto stream = udssock->connect(uds_path.data());
    if (!stream)
        LOG_ERRNO_RETURN(0, nullptr, "failed to dial to unix socket `", uds_path);
    return stream;
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
    CommonHeaders<> m_common_headers;
    TLSContext *m_tls_ctx;
    ICookieJar *m_cookie_jar;
    ClientImpl(ICookieJar *cookie_jar, TLSContext *tls_ctx) :
        m_tls_ctx(tls_ctx),
        m_cookie_jar(cookie_jar) {
    }
    PooledDialer& get_dialer() {
        thread_local PooledDialer dialer;
        dialer.init(m_tls_ctx, m_bind_ips);
        return dialer;
    }

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

    int do_roundtrip(Operation* op, Timeout tmo) {
        op->status_code = -1;
        if (tmo.timeout() == 0)
            LOG_ERROR_RETURN(ETIMEDOUT, ROUNDTRIP_FAILED, "connection timedout");
        auto &req = op->req;
        ISocketStream* s;
        if (m_proxy && !m_proxy_url.empty())
            s = get_dialer().dial(m_proxy_url, tmo.timeout());
        else if (!op->uds_path.empty())
            s = get_dialer().dial(op->uds_path, tmo.timeout());
        else
            s = get_dialer().dial(req, tmo.timeout());
        if (!s) {
            if (errno == ECONNREFUSED || errno == ENOENT) {
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
        if (op->body_buffer_size > 0) {
            // send body_buffer
            if (req.write(op->body_buffer, op->body_buffer_size) < 0) {
                sock->close();
                req.reset_status();
                LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "send body buffer failed, retry");
            }
        } else if (op->body_stream) {
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
        op->req.headers.merge(m_common_headers);
        op->req.headers.insert("User-Agent", m_user_agent.empty() ? std::string_view(USERAGENT)
                                                                  : std::string_view(m_user_agent));
        op->req.headers.insert("Connection", "keep-alive");
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
        return get_dialer().dial(host, port, secure, timeout);
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
