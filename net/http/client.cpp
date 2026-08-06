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
#include <sched.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/intrusive_list.h>
#include <photon/common/iovector.h>
#include <photon/common/string_view.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>
#include <photon/net/utils.h>
#include <photon/thread/thread.h>
#include <photon/photon.h>

namespace photon {
namespace net {
namespace http {
static const uint64_t kDNSCacheLife = 3600ULL * 1000 * 1000;
static constexpr char USERAGENT[] = "PhotonLibOS_HTTP";

class ClientImpl;

// Built-in dialer, owned by a (client, vCPU) pair. The connection pools and
// the collector thread inside are bound to the vCPU that created them, so a
// PooledDialer must be created, used and destroyed on its own vCPU.
class PooledDialer : public IDialer, public intrusive_list_node<PooledDialer> {
public:
    net::TLSContext* tls_ctx = nullptr;
    bool tls_ctx_ownership = false;
    std::unique_ptr<ISocketClient> tcpsock;
    std::unique_ptr<ISocketClient> tlssock;
    std::unique_ptr<ISocketClient> udssock;
    Resolver* resolver;   // shared, not owned (DialerRegistry's or set_resolver()'s)
    ClientImpl* owner;    // backref; stable while listed in the registry
    vcpu_base* vcpu = photon::get_vcpu();

    PooledDialer(ClientImpl* owner, TLSContext* _tls_ctx, Resolver* resolver,
                 std::vector<IPAddr>& src_ips)
            : resolver(resolver), owner(owner) {
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
    }

    ~PooledDialer() override {
        // the pools must go before the TLS context they use
        udssock.reset();
        tlssock.reset();
        tcpsock.reset();
        if (tls_ctx_ownership)
            delete tls_ctx;
    }

    ISocketStream* dial(std::string_view host, uint16_t port, bool secure,
                        uint64_t timeout) override;

    ISocketStream* dial(std::string_view uds_path, uint64_t timeout) override;
};

// Per-vCPU registry owning the built-in dialers created on this vCPU. A
// dialer is destroyed either by ~ClientImpl (claiming it out of the list,
// possibly from another vCPU), or by the photon::fini() hook of this vCPU --
// whichever comes first -- so that no pool or collector thread outlives its
// vCPU, even for leaked clients or the fini+init cycle of a pthread_atfork
// handler. Whoever unlinks a dialer from `dialers` destroys it.
struct DialerRegistry {
    photon::spinlock lock;   // guards `dialers`; taken cross-vCPU by ~ClientImpl
    intrusive_list<PooledDialer, false> dialers;
    Resolver* default_resolver = nullptr;
    bool fini_hook_registered = false;

    PooledDialer* find_locked(ClientImpl* c) {
        for (auto d : dialers)
            if (d->owner == c) return d;
        return nullptr;
    }

    bool contains_locked(PooledDialer* target) {
        for (auto d : dialers)
            if (d == target) return true;
        return false;
    }

    Resolver* get_default_resolver() {
        if (!default_resolver) {
            auto r = new_default_resolver(kDNSCacheLife);
            if (!default_resolver) default_resolver = r;   // ctor may yield
            else delete r;
        }
        return default_resolver;
    }

    void ensure_fini_hook() {
        if (!fini_hook_registered) {
            fini_hook_registered = true;
            photon::fini_hook({this, &DialerRegistry::at_photon_fini});
        }
    }

    void at_photon_fini();   // defined after ClientImpl
};

static thread_local DialerRegistry g_dialer_registry;

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
    photon::spinlock m_dref_lock;   // guards m_drefs
    struct DialerRef {
        PooledDialer* dialer;
        DialerRegistry* registry;
        vcpu_base* vcpu;
    };
    std::vector<DialerRef> m_drefs;  // one built-in dialer per vCPU used

    ClientImpl(ICookieJar *cookie_jar, TLSContext *tls_ctx) :
        m_tls_ctx(tls_ctx),
        m_cookie_jar(cookie_jar) {
    }

    ~ClientImpl() override {
        while (true) {
            DialerRef e;
            {
                SCOPED_LOCK(m_dref_lock);
                if (m_drefs.empty()) break;
                e = m_drefs.back();
            }
            if (!photon::CURRENT) {
                // no photon context to run pool destruction; disown the dialer
                // and let the fini hook of its vCPU destroy it
                bool disowned = false;
                {
                    SCOPED_LOCK(e.registry->lock);
                    if (e.registry->contains_locked(e.dialer)) {
                        e.dialer->owner = nullptr;
                        disowned = true;
                    }
                }
                if (disowned) {
                    SCOPED_LOCK(m_dref_lock);
                    remove_dref_locked(e.dialer);
                } else {
                    ::sched_yield();   // the fini hook is dropping our backref
                }
                continue;
            }
            bool claimed = false;
            {
                SCOPED_LOCK(e.registry->lock);
                if (e.registry->contains_locked(e.dialer)) {
                    e.registry->dialers.erase(e.dialer);
                    claimed = true;
                }
            }
            if (!claimed) {
                // claimed by the fini hook of its vCPU, which will drop our
                // backref shortly; wait for that
                photon::thread_yield();
                continue;
            }
            {
                SCOPED_LOCK(m_dref_lock);
                remove_dref_locked(e.dialer);
            }
            destroy_dialer(e);
        }
    }

    void remove_dref_locked(PooledDialer* d) {
        for (auto it = m_drefs.begin(); it != m_drefs.end(); ++it)
            if (it->dialer == d) {
                m_drefs.erase(it);
                return;
            }
    }

    // built-in dialers must be destroyed on their own vCPU. Never migrate
    // CURRENT for this: after landing on another OS thread, reads of
    // photon::CURRENT may hit the stale TLS slot cached by the compiler.
    // Send a helper thread over instead, and wait for it.
    struct DestroyCtx {
        PooledDialer* dialer;
        photon::semaphore done;
        DestroyCtx(PooledDialer* d) : dialer(d), done(0) {}
    };
    static void* do_destroy_dialer(void* arg) {
        auto ctx = (DestroyCtx*)arg;
        delete ctx->dialer;
        ctx->done.signal(1);
        return nullptr;
    }
    void destroy_dialer(const DialerRef& e) {
        if (e.vcpu == photon::get_vcpu()) {
            delete e.dialer;
            return;
        }
        DestroyCtx ctx(e.dialer);
        auto th = photon::thread_create(&do_destroy_dialer, &ctx);
        if (photon::thread_migrate(th, e.vcpu) < 0)
            LOG_WARN("failed to migrate to the dialer's vCPU, destroying locally");
        ctx.done.wait(1);
    }

    IDialer* acquire_dialer() {
        if (m_dialer) return m_dialer;   // injected via set_dialer()
        auto& reg = g_dialer_registry;
        {
            SCOPED_LOCK(reg.lock);
            auto d = reg.find_locked(this);
            if (d) return d;
        }
        reg.ensure_fini_hook();
        auto resolver = m_resolver ? m_resolver : reg.get_default_resolver();
        auto d = new PooledDialer(this, m_tls_ctx, resolver, m_bind_ips);
        PooledDialer* existing;
        {
            SCOPED_LOCK(reg.lock);
            existing = reg.find_locked(this);
            if (!existing) {
                reg.dialers.push_back(d);
                SCOPED_LOCK(m_dref_lock);
                m_drefs.push_back({d, &reg, d->vcpu});
                return d;
            }
        }
        delete d;   // lost a benign race against a sibling thread of this vCPU
        return existing;
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
        auto dialer = acquire_dialer();
        ISocketStream* s;
        if (op->enable_proxy && !op->proxy_url.empty())
            s = dial_to(dialer, op->proxy_url, tmo.timeout());
        else if (op->enable_proxy && !m_proxy_url.empty())
            s = dial_to(dialer, m_proxy_url, tmo.timeout());
        else if (!op->uds_path.empty())
            s = dialer->dial(op->uds_path, tmo.timeout());
        else
            s = dial_to(dialer, req, tmo.timeout());
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
        if (op->enable_proxy && !m_proxy_auth.empty())
            op->req.headers.insert("Proxy-Authorization", m_proxy_auth);
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

    template <typename T>
    static ISocketStream* dial_to(IDialer* d, const T& x, uint64_t timeout) {
        return d->dial(x.host_no_port(), x.port(), x.secure(), timeout);
    }

    ISocketStream* native_connect(std::string_view host, uint16_t port, bool secure, uint64_t timeout) override {
        return acquire_dialer()->dial(host, port, secure, timeout);
    }

    CommonHeaders<>* common_headers() override {
        return &m_common_headers;
    }
};

void DialerRegistry::at_photon_fini() {
    for (;;) {
        lock.lock();
        auto d = dialers.pop_front();
        lock.unlock();
        if (!d) break;
        auto o = d->owner;   // stable: only ~ClientImpl of a listed dialer resets it
        if (o) {
            SCOPED_LOCK(o->m_dref_lock);
            o->remove_dref_locked(d);
        }
        delete d;   // on its own vCPU: the hook runs there
    }
    delete default_resolver;
    default_resolver = nullptr;
    fini_hook_registered = false;   // photon::fini() clears the hook vector
}

Client* new_http_client(ICookieJar *cookie_jar, TLSContext *tls_ctx) {
    return new ClientImpl(cookie_jar, tls_ctx);
}

} // namespace http
} // namespace net
} // namespace photon
