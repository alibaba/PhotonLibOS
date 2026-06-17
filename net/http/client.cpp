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
#include <photon/common/estring.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>
#include <photon/net/utils.h>
#include <photon/photon.h>
#include "../stream_pool.h"

namespace photon {
namespace net {
namespace http {
static const uint64_t kDNSCacheLife = 3600UL * 1000 * 1000;
static constexpr char USERAGENT[] = "PhotonLibOS_HTTP";

// Creates a CONNECT tunnel through an HTTP or HTTPS proxy (RFC 7231 §4.3.6).
class ConnectTunnelClient {
public:
    ConnectTunnelClient(ISocketClient* tcp_client, bool ownership,
                        TLSContext* tls_ctx, Resolver* resolver)
        : m_tcp_cli(tcp_client), m_tcp_cli_ownership(ownership),
          m_tls_ctx(tls_ctx), m_resolver(resolver) {}

    ~ConnectTunnelClient() {
        if (m_tcp_cli_ownership)
            delete m_tcp_cli;
    }

    // Establish a CONNECT tunnel: DNS resolve -> TCP -> [TLS to proxy]
    // -> CONNECT handshake -> TLS to target.
    ISocketStream* connect(const StoredURL& proxy_url,
                           std::string_view target_host, uint16_t target_port,
                           const Headers* headers) {
        auto proxy_ip = m_resolver->resolve(proxy_url.host_no_port());
        if (proxy_ip.undefined()) {
            LOG_ERROR_RETURN(ENOENT, nullptr,
                "CONNECT tunnel: DNS resolve failed for proxy `",
                proxy_url.host_no_port());
        }
        EndPoint proxy_ep(proxy_ip, proxy_url.port());
        auto stream = m_tcp_cli->connect(proxy_ep);
        if (!stream) {
            m_resolver->discard_cache(proxy_url.host_no_port(), proxy_ip);
            LOG_ERRNO_RETURN(0, nullptr, "CONNECT tunnel: failed to connect to proxy");
        }

        if (proxy_url.secure()) {
            stream = new_tls_stream(m_tls_ctx, stream, SecurityRole::Client, true);
            if (!stream)
                LOG_ERRNO_RETURN(0, nullptr, "CONNECT tunnel: TLS to proxy failed");
            auto host = proxy_url.host_no_port();
            tls_stream_set_hostname(stream, std::string(host).c_str());
        }

        if (do_connect_handshake(stream, target_host, target_port, headers) != 0) {
            delete stream;
            return nullptr;
        }

        stream = new_tls_stream(m_tls_ctx, stream, SecurityRole::Client, true);
        if (!stream)
            LOG_ERRNO_RETURN(0, nullptr, "CONNECT tunnel: TLS to target failed");
        tls_stream_set_hostname(stream, std::string(target_host).c_str());
        return stream;
    }

private:
    int do_connect_handshake(ISocketStream* stream,
                             std::string_view target_host, uint16_t target_port,
                             const Headers* headers) {
        char buf[4096];
        // CONNECT target is always host:port per RFC 7231 §4.3.6.
        int n = snprintf(buf, sizeof(buf),
            "CONNECT %.*s:%u HTTP/1.1\r\n",
            (int)target_host.size(), target_host.data(), target_port);
        if (n < 0 || n >= (int)sizeof(buf))
            LOG_ERROR_RETURN(ENOMEM, -1, "CONNECT request too long");
        // Host header is required for CONNECT (points to the target).
        n += snprintf(buf + n, sizeof(buf) - n, "Host: %.*s:%u\r\n",
            (int)target_host.size(), target_host.data(), target_port);
        if (n >= (int)sizeof(buf))
            LOG_ERROR_RETURN(ENOMEM, -1, "CONNECT request too long");
        // Forward proxy-specific headers (Proxy-Authorization, etc.).
        if (headers) {
            for (auto it = headers->begin(); it != headers->end(); ++it) {
                auto k = it.first();
                auto v = it.second();
                if ((size_t)(n + k.size() + v.size() + 4) >= sizeof(buf))
                    LOG_ERROR_RETURN(ENOMEM, -1, "CONNECT request too long");
                memcpy(buf + n, k.data(), k.size()); n += k.size();
                buf[n++] = ':'; buf[n++] = ' ';
                memcpy(buf + n, v.data(), v.size()); n += v.size();
                buf[n++] = '\r'; buf[n++] = '\n';
            }
        }
        if (n + 2 >= (int)sizeof(buf))
            LOG_ERROR_RETURN(ENOMEM, -1, "CONNECT request too long");
        buf[n++] = '\r'; buf[n++] = '\n'; buf[n] = '\0';
        if (stream->write(buf, n) != n)
            LOG_ERRNO_RETURN(0, -1, "CONNECT tunnel: failed to send CONNECT request");

        size_t total = 0;
        while (total < sizeof(buf) - 1) {
            auto rc = stream->recv(buf + total, sizeof(buf) - 1 - total);
            if (rc <= 0)
                LOG_ERRNO_RETURN(0, -1, "CONNECT tunnel: failed to read proxy response");
            total += rc;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n"))
                break;
        }

        auto space = strchr(buf, ' ');
        if (!space)
            LOG_ERROR_RETURN(EINVAL, -1, "CONNECT tunnel: invalid proxy response");
        int status_code = atoi(space + 1);
        if (status_code < 200 || status_code >= 300) {
            auto end = strstr(buf, "\r\n\r\n");
            if (end) *end = '\0';
            LOG_ERROR_RETURN(EINVAL, -1,
                "CONNECT tunnel: proxy returned status ", status_code,
                ", response: ", buf);
        }
        return 0;
    }

    ISocketClient* m_tcp_cli;
    bool m_tcp_cli_ownership;
    TLSContext* m_tls_ctx;
    Resolver* m_resolver;
};

// Caches CONNECT tunnel streams keyed by (proxy, target, auth).
class TunnelPool : public StreamPoolBase<std::string> {
public:
    TunnelPool(ConnectTunnelClient* tunnel_cli, bool ownership, uint64_t ttl_us = -1UL)
        : StreamPoolBase(ttl_us), m_tunnel_cli(tunnel_cli), m_ownership(ownership) {}

    ~TunnelPool() {
        if (m_ownership)
            delete m_tunnel_cli;
    }

    // Establish or reuse a cached CONNECT tunnel.
    // headers contains proxy-specific headers (Proxy-Authorization, etc.) from call().
    ISocketStream* connect_tunnel(const StoredURL& proxy_url, std::string_view target_host,
                                  uint16_t target_port, const Headers* headers) {
        auto auth = std::string(headers ? headers->get_value("Proxy-Authorization")
                                        : std::string_view{});
        auto host = proxy_url.host_no_port();
        auto key = estring::snprintf("%.*s:%u:%c:%.*s:%u",
            (int)host.size(), host.data(), proxy_url.port(),
            proxy_url.secure() ? 's' : 'p',
            (int)target_host.size(), target_host.data(), target_port);
        if (!auth.empty()) {
            key += ':';
            key += auth;
        }

        return acquire(key, [this, &proxy_url, target_host, target_port, headers]() {
            return m_tunnel_cli->connect(proxy_url, target_host, target_port, headers);
        });
    }

private:
    ConnectTunnelClient* m_tunnel_cli;
    bool m_ownership;
};

class PooledDialer {
public:
    net::TLSContext* tls_ctx = nullptr;
    std::unique_ptr<ISocketClient> tcpsock;
    std::unique_ptr<ISocketClient> tlssock;
    std::unique_ptr<ISocketClient> udssock;
    std::unique_ptr<Resolver> resolver;
    std::unique_ptr<TunnelPool> tunnel_pool;
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
        auto tcp_cli = new_tcp_socket_client(src_ips.data(), src_ips.size());
        auto tls_cli = new_tls_client(tls_ctx, new_tcp_socket_client(src_ips.data(), src_ips.size()), true);
        tcpsock.reset(new_tcp_socket_pool(tcp_cli, -1, true));
        tlssock.reset(new_tcp_socket_pool(tls_cli, -1, true));
        udssock.reset(new_uds_client());
        resolver.reset(new_default_resolver(kDNSCacheLife));
        auto tunnel_tcp = new_tcp_socket_client(src_ips.data(), src_ips.size());
        auto tunnel_cli = new ConnectTunnelClient(tunnel_tcp, true, tls_ctx, resolver.get());
        tunnel_pool.reset(new TunnelPool(tunnel_cli, true));
        initialized = true;
        return 0;
    }

    void at_photon_fini() {
        tunnel_pool.reset();
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

    // Single entry point: choose dial method based on Operation proxy config.
    ISocketStream* dial(Client::Operation* op, Timeout tmo);
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

ISocketStream* PooledDialer::dial(Client::Operation* op, Timeout tmo) {
    auto* active_proxy_url = op->get_active_proxy();

    // CONNECT tunnel: proxy + HTTPS target
    // proxy_header is used for the CONNECT handshake (req.headers is for the target server).
    if (active_proxy_url && op->req.secure()) {
        auto* stream = tunnel_pool->connect_tunnel(
            *active_proxy_url, op->req.host_no_port(), op->req.port(), &op->proxy_header);
        if (!stream) {
            LOG_ERROR("CONNECT tunnel failed, target: `:`",
                      op->req.host_no_port(), op->req.port());
            return nullptr;
        }
        return stream;
    }

    // HTTP proxy: dial proxy endpoint directly (secure case handled by CONNECT above)
    if (active_proxy_url) {
        return dial(*active_proxy_url, tmo.timeout());
    }

    // UDS
    if (!op->uds_path.empty()) {
        return dial(op->uds_path, tmo.timeout());
    }

    // Direct connection
    return dial(op->req, tmo.timeout());
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

        bool use_proxy = op->get_active_proxy() != nullptr;
        if (op->req.redirect(v, location, use_proxy) < 0) {
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

        // Proxy header assembly: op proxy_header (already set) <- client proxy_header <- URL auth
        auto proxy = op->get_active_proxy();
        if (proxy) {
            op->proxy_header.merge(m_proxy_headers);
            if (!proxy->user_passwd().empty()) {
                std::string encoded;
                Base64Encode(proxy->user_passwd(), encoded);
                op->proxy_header.insert("Proxy-Authorization", "Basic " + encoded);
            }
        }

        // rebuild request line if necessary
        auto need_abs = proxy && !op->req.secure();
        auto is_abs = what_protocol(estring_view(op->req.target())) == 1;
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
                    sleep_interval = (sleep_interval + 500'000UL) * 2;
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
