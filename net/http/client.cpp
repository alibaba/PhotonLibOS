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
#include <photon/common/estring.h>
#include <photon/common/iovector.h>
#include <photon/common/string_view.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>
#include <photon/net/utils.h>
#include <photon/photon.h>

namespace photon {
namespace net {
namespace http {
static const uint64_t kDNSCacheLife = 3600ULL * 1000 * 1000;
static constexpr char USERAGENT[] = "PhotonLibOS_HTTP";


class PooledDialer {
public:
    net::TLSContext* tls_ctx = nullptr;
    std::unique_ptr<ISocketClient> tcpsock;
    std::unique_ptr<ISocketClient> udssock;
    std::unique_ptr<Resolver> resolver;
    std::unique_ptr<ISocketPool> tcp_pool;
    photon::mutex init_mtx;
    bool initialized = false;
    bool tls_ctx_ownership = false;

    // If there is a photon thread switch during construction, the constructor might be called
    // multiple times, even for a thread_local instance. Therefore, ensure that there is no photon
    // thread switch inside the constructor. Place the initialization work in init() and ensure it
    // is initialized only once.
    int init(TLSContext *_tls_ctx, std::vector<IPAddr> &src_ips) {
        if (initialized)
            return 0;
        SCOPED_LOCK(init_mtx);
        if (initialized)
            return 0;
        photon::fini_hook({this, &PooledDialer::at_photon_fini});
        tls_ctx = _tls_ctx;
        if (!tls_ctx) {
            tls_ctx_ownership = true;
            tls_ctx = new_tls_context(nullptr, nullptr, nullptr);
            tls_ctx->set_verify_mode(VerifyMode::PEER);  // act like curl
        }
        tcpsock.reset(new_tcp_socket_client(src_ips.data(), src_ips.size()));
        udssock.reset(new_uds_client());
        tcp_pool.reset(new_tcp_socket_pool(nullptr, -1ULL, false));
        resolver.reset(new_default_resolver(kDNSCacheLife));
        initialized = true;
        return 0;
    }

    void at_photon_fini() {
        tcp_pool.reset();
        resolver.reset();
        udssock.reset();
        tcpsock.reset();
        if (tls_ctx_ownership)
            delete tls_ctx;
        initialized = false;
        tls_ctx_ownership = false;
    }

    // Single entry point: choose dial method based on Operation proxy config.
    ISocketStream* dial(Client::Operation* op, Timeout tmo);

    ISocketStream* dial(std::string_view host, uint16_t port, bool secure, uint64_t timeout);

private:
    // Key format for pool connection grouping:
    //   direct/proxy: <host>:<port>:<0|1>  (0=TCP, 1=TLS)
    //   tunnel:       <proxy_host>:<proxy_port>:<s|p>:<target_host>:<target_port>[:<auth>]
    static std::string make_key(std::string_view host, uint16_t port, bool secure) {
        return estring().appends(host, ":", port, ":", secure ? "1" : "0");
    }

    static std::string make_tunnel_key(const StoredURL& proxy, std::string_view target_host,
                                       uint16_t target_port, const Headers* headers) {
        auto auth = headers ? headers->get_value("Proxy-Authorization") : std::string_view{};
        auto phost = proxy.host_no_port();
        return estring().appends(phost, ":", proxy.port(), ":", proxy.secure() ? "s" : "p",
                                 ":", target_host, ":", target_port,
                                 estring::make_conditional_cat_list(!auth.empty(), ":", auth));
    }

    // Factory: DNS resolve -> TCP connect -> optional TLS wrap with SNI.
    // Discards the resolved IP from cache on connection failure.
    ISocketStream* make_stream(std::string_view host, uint16_t port, bool secure, uint64_t timeout) {
        auto ip = resolver->resolve(host);
        if (ip.undefined())
            LOG_ERROR_RETURN(ENOENT, nullptr, "DNS resolve failed, name=`", host);
        EndPoint ep(ip, port);
        tcpsock->timeout(timeout);
        auto stream = tcpsock->connect(ep);
        if (!stream) {
            resolver->discard_cache(host, ip);
            LOG_ERROR_RETURN(0, nullptr, "connection failed, ep=` host=` secure=`", ep, host, secure);
        }
        if (secure) {
            stream = new_tls_stream(tls_ctx, stream, SecurityRole::Client, true);
            if (!stream)
                LOG_ERRNO_RETURN(0, nullptr, "TLS handshake failed, host=`", host);
            tls_stream_set_hostname(stream, std::string(host).c_str());
        }
        LOG_DEBUG("Connected ` ssl:` host:`", ep, secure, host);
        return stream;
    }

    // Factory: full CONNECT tunnel handshake sequence.
    // DNS resolve proxy -> TCP -> [TLS to proxy] -> CONNECT handshake -> TLS to target.
    ISocketStream* make_tunnel_stream(const StoredURL& proxy_url, std::string_view target_host,
                                      uint16_t target_port, const Headers* headers, uint64_t timeout) {
        auto proxy_ip = resolver->resolve(proxy_url.host_no_port());
        if (proxy_ip.undefined())
            LOG_ERROR_RETURN(ENOENT, nullptr, "CONNECT tunnel: DNS resolve failed for proxy `", proxy_url.host_no_port());
        EndPoint proxy_ep(proxy_ip, proxy_url.port());
        tcpsock->timeout(timeout);
        auto stream = tcpsock->connect(proxy_ep);
        if (!stream) {
            resolver->discard_cache(proxy_url.host_no_port(), proxy_ip);
            LOG_ERRNO_RETURN(0, nullptr, "CONNECT tunnel: failed to connect to proxy");
        }
        stream->timeout(timeout);
        if (proxy_url.secure()) {
            auto tls = new_tls_stream(tls_ctx, stream, SecurityRole::Client, true);
            if (!tls) {
                delete stream;
                LOG_ERRNO_RETURN(0, nullptr, "CONNECT tunnel: TLS to proxy failed");
            }
            stream = tls;
            tls_stream_set_hostname(stream, std::string(proxy_url.host_no_port()).c_str());
        }
        if (do_connect_handshake(stream, target_host, target_port, headers, timeout) != 0) {
            delete stream;
            return nullptr;
        }
        auto tls = new_tls_stream(tls_ctx, stream, SecurityRole::Client, true);
        if (!tls) {
            delete stream;
            LOG_ERRNO_RETURN(0, nullptr, "CONNECT tunnel: TLS to target failed");
        }
        stream = tls;
        tls_stream_set_hostname(stream, std::string(target_host).c_str());
        return stream;
    }

    // Send CONNECT request and verify 2xx response.
    int do_connect_handshake(ISocketStream* stream, std::string_view target_host,
                             uint16_t target_port, const Headers* headers, uint64_t timeout) {
        char buf[4096];
        estring authority;
        authority.appends(target_host, ":", target_port);
        Request req(buf, (uint16_t)sizeof(buf), Verb::CONNECT, authority);
        if (headers) {
            for (auto it = headers->begin(); it != headers->end(); ++it)
                req.headers.insert(it.first(), it.second());
        }
        if (req.send_header(stream) < 0)
            LOG_ERRNO_RETURN(0, -1, "CONNECT tunnel: failed to send CONNECT request");

        // Response needs room for receive_bytes' MAX_TRANSFER_BYTES + RESERVED_INDEX_SIZE.
        char rbuf[8192];
        Response resp(rbuf, (uint16_t)sizeof(rbuf));
        resp.reset(stream, false);
        if (resp.receive_header(timeout) != 0)
            LOG_ERRNO_RETURN(0, -1, "CONNECT tunnel: failed to read proxy response");
        auto sc = resp.status_code();
        if (sc < 200 || sc >= 300)
            LOG_ERROR_RETURN(EINVAL, -1, "CONNECT tunnel: proxy returned status `", sc);
        return 0;
    }
};

ISocketStream* PooledDialer::dial(Client::Operation* op, Timeout tmo) {
    auto* proxy = op->get_active_proxy();

    // HTTPS target + proxy: CONNECT tunnel.
    if (proxy && op->req.secure()) {
        auto key = make_tunnel_key(*proxy, op->req.host_no_port(), op->req.port(), &op->proxy_header);
        return tcp_pool->connect(key, [&]() -> ISocketStream* {
            return make_tunnel_stream(*proxy, op->req.host_no_port(), op->req.port(), &op->proxy_header, tmo.timeout());
        });
    }

    // HTTP proxy: connect to proxy endpoint.
    if (proxy) {
        auto key = make_key(proxy->host_no_port(), proxy->port(), proxy->secure());
        return tcp_pool->connect(key, [&]() -> ISocketStream* {
            return make_stream(proxy->host_no_port(), proxy->port(), proxy->secure(), tmo.timeout());
        });
    }

    // Unix domain socket.
    if (!op->uds_path.empty()) {
        udssock->timeout(tmo.timeout());
        return udssock->connect(op->uds_path.data());
    }

    // Direct TCP/TLS connection.
    auto key = make_key(op->req.host_no_port(), op->req.port(), op->req.secure());
    return tcp_pool->connect(key, [&]() -> ISocketStream* {
        return make_stream(op->req.host_no_port(), op->req.port(), op->req.secure(), tmo.timeout());
    });
}

ISocketStream* PooledDialer::dial(std::string_view host, uint16_t port, bool secure, uint64_t timeout) {
    auto key = make_key(host, port, secure);
    return tcp_pool->connect(key, [&]() -> ISocketStream* {
        return make_stream(host, port, secure, timeout);
    });
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
    CommonHeaders<> m_proxy_headers;
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

        if (op->req.redirect(v, location, op->get_active_proxy() != nullptr) < 0) {
            LOG_ERRNO_RETURN(0, ROUNDTRIP_FAILED, "redirect failed");
        }
        return ROUNDTRIP_REDIRECT;
    }

    int do_roundtrip(Operation* op, Timeout tmo) {
        op->status_code = -1;
        if (tmo.timeout() == 0)
            LOG_ERROR_RETURN(ETIMEDOUT, ROUNDTRIP_FAILED, "connection timedout");
        auto &req = op->req;
        ISocketStream* s = get_dialer().dial(op, tmo);
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
        resp.reset_status(HEADER_SENT);
        if (resp.receive_header(tmo.timeout()) != 0) {
            req.reset_status();
            resp.reset(nullptr, false);
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

        // Proxy header assembly: op proxy_header <- client proxy_headers <- URL auth
        auto proxy = op->get_active_proxy();
        if (proxy) {
            op->proxy_header.merge(m_proxy_headers);
            if (!proxy->user_passwd().empty()) {
                std::string encoded;
                Base64Encode(proxy->user_passwd(), encoded);
                op->proxy_header.insert("Proxy-Authorization", "Basic " + encoded);
            }
        }

        // Rebuild request line if necessary (absolute URI for HTTP proxy)
        auto need_abs = proxy && !op->req.secure();
        auto is_abs = what_protocol(estring_view(op->req.target())) != 0;
        if (need_abs != is_abs) {
            op->req.redirect(op->req.verb(), op->req.target(), need_abs);
        }

        // For HTTP/1.1 proxy: merge proxy_header into req.headers (sent with the request).
        // For HTTPS proxy: proxy_header stays separate (used by CONNECT handshake only).
        if (proxy && !op->req.secure()) {
            op->req.headers.merge(op->proxy_header);
        }

        Timeout tmo(std::min(op->timeout.timeout(), m_timeout));
        int retry = 0, followed = 0, ret = 0;
        uint64_t sleep_interval = 0;
        while (followed <= op->follow && retry <= op->retry && tmo.timeout() != 0) {
            ret = do_roundtrip(op, tmo);
            if (ret == ROUNDTRIP_SUCCESS || ret == ROUNDTRIP_FAILED) break;
            switch (ret) {
                case ROUNDTRIP_NEED_RETRY:
                    photon::thread_usleep(std::min(sleep_interval, tmo.timeout()));
                    sleep_interval = (sleep_interval + 500'000ULL) * 2;
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

    CommonHeaders<>* proxy_headers() override {
        return &m_proxy_headers;
    }
};

Client* new_http_client(ICookieJar *cookie_jar, TLSContext *tls_ctx) {
    return new ClientImpl(cookie_jar, tls_ctx);
}

} // namespace http
} // namespace net
} // namespace photon
