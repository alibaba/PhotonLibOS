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
#include <photon/common/alog-stdstring.h>
#include <photon/common/estring.h>
#include <photon/common/iovector.h>
#include <photon/common/string_view.h>
#include <photon/net/socket.h>
#include <photon/net/utils.h>
#include <photon/thread/thread.h>
#include <photon/photon.h>

namespace photon {
namespace net {
namespace http {
static constexpr char USERAGENT[] = "PhotonLibOS_HTTP";

// The shared_ptr atomic free functions are required by the C++14 baseline.
// libstdc++ deprecates them in C++23 in favor of atomic<shared_ptr<T>>, which
// is only available since C++20. Keep the compatibility shim narrowly scoped.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static std::shared_ptr<Resolver> atomic_load_resolver(
        const std::shared_ptr<Resolver>* resolver) {
    return std::atomic_load(resolver);
}

static void atomic_store_resolver(std::shared_ptr<Resolver>* target,
                                  std::shared_ptr<Resolver> resolver) {
    std::atomic_store(target, std::move(resolver));
}
#pragma GCC diagnostic pop

class ClientImpl;

constexpr uint64_t code3xx() { return 0; }
template<typename...Ts>
constexpr uint64_t code3xx(uint64_t x, Ts...xs)
{
    return (1 << (x-300)) | code3xx(xs...);
}
constexpr static std::bitset<10>
    code_redirect_verb(code3xx(300, 301, 302, 307, 308));

static constexpr size_t kMinimalHeadersSize = 8 * 1024 - 1;

Client::~Client() = default;

void Client::set_resolver(Resolver* resolver, bool ownership) {
    // Keep the old lease outside the lock scope: resolver destruction can yield
    // or reenter set_resolver(), and must never run while the spinlock is held.
    std::shared_ptr<Resolver> current;
    {
        SCOPED_LOCK(m_resolver_lock);
        current = atomic_load_resolver(&m_resolver);
        if (current.get() == resolver) return;

        std::shared_ptr<Resolver> next;
        if (resolver) {
            if (ownership)
                next.reset(resolver);
            else
                next = std::shared_ptr<Resolver>(resolver, [](Resolver*) { });
        }
        atomic_store_resolver(&m_resolver, std::move(next));
    }
}

void Client::set_proxy(std::string_view proxy) {
    m_proxy_url.from_string(proxy);
    m_proxy = true;
    auto ui = m_proxy_url.user_passwd();
    if (!ui.empty()) {
        std::string encoded;
        Base64Encode(ui, encoded);
        m_proxy_auth = "Basic " + encoded;
    } else {
        m_proxy_auth.clear();
    }
}

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
    // The dispatcher is cross-vCPU safe; each lazily-created child is a complete
    // single-vCPU HTTP stack with its own final connection pool.
    std::unique_ptr<IDialer> m_builtin_dialer;

    ClientImpl(ICookieJar *cookie_jar, TLSContext *tls_ctx) :
        m_tls_ctx(tls_ctx),
        m_cookie_jar(cookie_jar),
        m_builtin_dialer(new_vcpu_local_dialer(
            {this, &ClientImpl::make_dialer})) {
    }

    IDialer* make_dialer() {   // on the current vCPU, for this client
        return new_http_dialer(m_tls_ctx, m_bind_ips);
    }

    IDialer* acquire_dialer() {
        if (m_dialer) return m_dialer;   // injected via set_dialer()
        return m_builtin_dialer.get();
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

    // Where this operation has to connect to: a proxy takes precedence over a
    // unix socket, which takes precedence over the origin itself.
    DialTarget dial_target(Operation* op, std::string_view proxy_auth) {
        DialTarget t;
        t.host = op->req.host_no_port();
        t.port = op->req.port();
        t.secure = op->req.secure();
        auto& proxy = op->proxy_url.empty() ? m_proxy_url : op->proxy_url;
        if (op->enable_proxy && !proxy.empty()) {
            t.proxy_host = proxy.host_no_port();
            t.proxy_port = proxy.port();
            t.proxy_secure = proxy.secure();
            t.proxy_auth = proxy_auth;
        } else {
            t.uds_path = op->uds_path;
        }
        return t;
    }

    // The Proxy-Authorization in effect: the userinfo of the per-operation proxy,
    // or the client-level credentials when the client's proxy is the one used.
    std::string proxy_auth_of(Operation* op) {
        if (!op->enable_proxy) return {};
        if (op->proxy_url.empty()) return m_proxy_auth;
        auto ui = op->proxy_url.user_passwd();
        if (ui.empty()) return {};   // another proxy: never reuse credentials
        std::string encoded;
        Base64Encode(ui, encoded);
        return "Basic " + encoded;
    }

    int do_roundtrip(Operation* op, Timeout tmo, std::string_view proxy_auth) {
        op->status_code = -1;
        if (tmo.timeout() == 0)
            LOG_ERROR_RETURN(ETIMEDOUT, ROUNDTRIP_FAILED, "connection timedout");
        auto &req = op->req;
        auto t = dial_target(op, proxy_auth);
        // Which headers the proxy gets is decided per hop, and never stored in the
        // caller's request: a redirect may turn a forwarded request into a tunneled
        // one, and only the former is read by the proxy -- inside a tunnel it is
        // the origin that reads them.
        ProxyAuth pa;
        if (t.via_proxy()) {
            if (m_proxy_authenticator && m_proxy_authenticator(t, pa) < 0)
                LOG_ERROR_RETURN(0, ROUNDTRIP_FAILED, "the proxy authenticator failed");
            t.proxy_headers = &pa.headers;
            t.proxy_pool_key = pa.pool_key;
        }
        auto dialer = acquire_dialer();
        if (!m_dialer) t.resolver = atomic_load_resolver(&m_resolver);
        auto s = dialer->dial(t, tmo.timeout());
        if (!s) {
            if (errno == ECONNREFUSED || errno == ENOENT) {
                LOG_ERROR_RETURN(0, ROUNDTRIP_FAST_RETRY, "connection refused")
            }
            LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "connection failed");
        }

        SocketStream_ptr sock(s);
        // a forwarded request is the one the proxy reads, so it carries the proxy's
        // headers; for a tunneled one they went into the CONNECT instead
        const HeadersBase* proxy_headers = nullptr;
        if (t.via_proxy() && !t.need_tunnel()) {
            if (!proxy_auth.empty()) {
                auto ret = pa.headers.insert("Proxy-Authorization", proxy_auth);
                if (ret < 0 && ret != -EEXIST)   // the authenticator's own one wins
                    LOG_ERROR_RETURN(0, ROUNDTRIP_FAILED, "failed to set Proxy-Authorization");
            }
            proxy_headers = &pa.headers;
        }
        LOG_DEBUG("Sending request ` `", req.verb(), req.target());
        if (req.send_header(sock.get(), proxy_headers) < 0) {
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
        auto proxy_auth = proxy_auth_of(op);
        auto& proxy = op->proxy_url.empty() ? m_proxy_url : op->proxy_url;
        auto header_proxy_auth = op->req.headers["Proxy-Authorization"];
        if (op->enable_proxy && !proxy.empty() && !header_proxy_auth.empty()) {
            proxy_auth.assign(header_proxy_auth.data(), header_proxy_auth.size());
            op->req.headers.erase("Proxy-Authorization");
        }
        if (m_cookie_jar && m_cookie_jar->set_cookies_to_headers(&op->req) != 0)
            LOG_ERROR_RETURN(0, -1, "set_cookies_to_headers failed");
        Timeout tmo(std::min(op->timeout.timeout(), m_timeout));
        int retry = 0, followed = 0, ret = 0;
        uint64_t sleep_interval = 0;
        while (followed <= op->follow && retry <= op->retry && tmo.timeout() != 0) {
            ret = do_roundtrip(op, tmo, proxy_auth);
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
        DialTarget t;
        t.host = host;
        t.port = port;
        t.secure = secure;
        auto dialer = acquire_dialer();
        if (!m_dialer) t.resolver = atomic_load_resolver(&m_resolver);
        return dialer->dial(t, timeout);
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
