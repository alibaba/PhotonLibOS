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
#include <photon/common/estring.h>
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
static const uint64_t kResolverDrainTimeout = 3ULL * 1000 * 1000;
static constexpr char USERAGENT[] = "PhotonLibOS_HTTP";
// a CONNECT response is a status line and a few headers; anything longer than
// this is not something we would be able to make sense of anyway
static constexpr size_t kTunnelRespSize = 4 * 1024;
// a CONNECT request is a request line, a Host, and whatever the authenticator adds
static constexpr uint16_t kTunnelReqSize = 8 * 1024 - 1;

class ClientImpl;

// One DNS cache for the whole process, so that each host is cold-resolved once
// instead of once per vCPU. The cache owns a photon::Timer, thus it lives on the
// vCPU that created it and must not outlive that vCPU's runtime -- borrowers
// hold it across a single resolver call only. When its vCPU finishes, the cache
// is unpublished, and the next borrower publishes a fresh one in its place.
class SharedResolver {
public:
    // borrowed resolver; also used to carry a resolver that isn't shared at all
    class Ref {
    public:
        Ref(SharedResolver* owner, Resolver* r) : _owner(owner), _r(r) { }
        Ref(Ref&& rhs) : _owner(rhs._owner), _r(rhs._r) { rhs._r = nullptr; }
        Ref(const Ref&) = delete;
        ~Ref() { if (_owner && _r) _owner->put(); }
        Resolver* operator->() const { return _r; }
    protected:
        SharedResolver* _owner;
        Resolver* _r;
    };

    Ref borrow() {
        {
            SCOPED_LOCK(_lock);
            if (_resolver) return ++_users, Ref{this, _resolver};
        }
        auto r = new_default_resolver(kDNSCacheLife);   // may yield
        Resolver* redundant = nullptr;
        {
            SCOPED_LOCK(_lock);
            if (_resolver) redundant = r;   // lost a benign race
            else _resolver = r, _vcpu = photon::get_vcpu();
            ++_users;
            r = _resolver;
        }
        delete redundant;   // outside the lock: destroying its timer may yield
        return {this, r};
    }

    // called from the fini hook of the vCPU owning the cache, once every
    // built-in dialer of that vCPU is gone
    void at_photon_fini() {
        Resolver* r;
        {
            SCOPED_LOCK(_lock);
            if (!_resolver || _vcpu != photon::get_vcpu()) return;
            r = _resolver;
            _resolver = nullptr;   // dials elsewhere will publish a new cache
            _vcpu = nullptr;
        }
        // a borrow spans a single resolver call, so this drains quickly; should
        // it somehow not, leaking the cache beats freeing it under a borrower
        Timeout tmo(kResolverDrainTimeout);
        while (true) {
            uint32_t users;
            {
                SCOPED_LOCK(_lock);
                users = _users;
            }
            if (users == 0) break;
            if (tmo.expired())
                LOG_ERROR_RETURN(0, , "DNS cache is still borrowed by other vCPUs, leaking it, ", VALUE(users));
            photon::thread_usleep(1000);
        }
        delete r;
    }

protected:
    photon::spinlock _lock;   // guards all below; taken cross-vCPU
    Resolver* _resolver = nullptr;
    vcpu_base* _vcpu = nullptr;
    uint32_t _users = 0;

    void put() {
        SCOPED_LOCK(_lock);
        --_users;
    }
};

static SharedResolver g_shared_resolver;

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
    // created on demand, when a CONNECT tunnel is first asked for
    std::unique_ptr<ISocketClient> proxy_tcp;   // outer leg to a plaintext proxy
    std::unique_ptr<ISocketClient> proxy_tls;   // outer leg to a TLS proxy
    std::unique_ptr<ISocketPool> tunnelsock;    // pools the established tunnels
    std::vector<IPAddr> bind_ips;
    Resolver* resolver;   // set_resolver()'s, not owned; null for the shared cache
    ClientImpl* owner;    // backref; stable while listed in the registry
    vcpu_base* vcpu = photon::get_vcpu();

    PooledDialer(ClientImpl* owner, TLSContext* _tls_ctx, Resolver* resolver,
                 const std::vector<IPAddr>& src_ips)
            : bind_ips(src_ips), resolver(resolver), owner(owner) {
        tls_ctx = _tls_ctx;
        if (!tls_ctx) {
            tls_ctx_ownership = true;
            tls_ctx = new_tls_context(nullptr, nullptr, nullptr);
            tls_ctx->set_verify_mode(VerifyMode::PEER);  // act like curl
        }
        auto tcp_cli = new_tcp_socket_client(bind_ips.data(), bind_ips.size());
        auto tls_cli = new_tls_client(tls_ctx, new_tcp_socket_client(bind_ips.data(), bind_ips.size()), true);
        tcpsock.reset(new_tcp_socket_pool(tcp_cli, -1, true));
        tlssock.reset(new_tcp_socket_pool(tls_cli, -1, true));
        udssock.reset(new_uds_client());
    }

    ~PooledDialer() override {
        // the pools must go before the clients and the TLS context they use;
        // the tunnels in particular own their outer legs
        tunnelsock.reset();
        udssock.reset();
        tlssock.reset();
        tcpsock.reset();
        proxy_tls.reset();
        proxy_tcp.reset();
        if (tls_ctx_ownership)
            delete tls_ctx;
    }

    ISocketStream* dial(const DialTarget& target, uint64_t timeout) override;

protected:
    // the resolver for one dial: the injected one, or a borrow of the shared cache
    SharedResolver::Ref get_resolver() {
        if (resolver) return {nullptr, resolver};
        return g_shared_resolver.borrow();
    }

    ISocketStream* dial_uds(std::string_view uds_path, uint64_t timeout);
    ISocketStream* dial_direct(std::string_view host, uint16_t port, bool secure, uint64_t timeout);
    ISocketStream* dial_tunnel(const DialTarget& target, uint64_t timeout);
    ISocketStream* connect_tunnel(const DialTarget& target, uint64_t timeout);
    int tunnel_handshake(ISocketStream* leg, const DialTarget& target);
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

    void ensure_fini_hook() {
        if (!fini_hook_registered) {
            fini_hook_registered = true;
            photon::fini_hook({this, &DialerRegistry::at_photon_fini});
        }
    }

    void at_photon_fini();   // defined after ClientImpl
};

static thread_local DialerRegistry g_dialer_registry;

ISocketStream* PooledDialer::dial(const DialTarget& t, uint64_t timeout) {
    if (!t.uds_path.empty()) return dial_uds(t.uds_path, timeout);
    if (t.need_tunnel()) return dial_tunnel(t, timeout);
    // a plaintext origin behind a proxy is served by forwarding an absolute-URI
    // request, so the connection goes to the proxy, in the proxy's own scheme
    return t.via_proxy() ?
        dial_direct(t.proxy_host, t.proxy_port, t.proxy_secure, timeout) :
        dial_direct(t.host, t.port, t.secure, timeout);
}

ISocketStream* PooledDialer::dial_direct(std::string_view host, uint16_t port, bool secure, uint64_t timeout) {
    LOG_DEBUG("Dialing to `:`", host, port);
    auto r = get_resolver();
    auto ipaddr = r->resolve(host);
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
    r->discard_cache(host, ipaddr);
    return nullptr;
}

ISocketStream* PooledDialer::dial_uds(std::string_view uds_path, uint64_t timeout) {
    udssock->timeout(timeout);
    auto stream = udssock->connect(uds_path.data());
    if (!stream)
        LOG_ERRNO_RETURN(0, nullptr, "failed to dial to unix socket `", uds_path);
    return stream;
}

ISocketStream* PooledDialer::dial_tunnel(const DialTarget& t, uint64_t timeout) {
    if (!tunnelsock) {
        // no client of its own: every tunnel is built by the connector below, so
        // timeout() / setsockopt() must never be called on this pool
        tunnelsock.reset(new_tcp_socket_pool(nullptr, -1, false));
        if (!tunnelsock)
            LOG_ERRNO_RETURN(0, nullptr, "failed to create the tunnel pool");
    }
    // A tunnel only leads to the origin it was opened for, and only through the
    // proxy that opened it; one authenticated with other credentials is not ours
    // to reuse either. All of them together make up its pooling key, with the
    // caller's own contribution last, where the colons it may contain cannot be
    // mistaken for the separators of the fields before it.
    estring key;
    key.appends(t.proxy_host, ":", t.proxy_port, t.proxy_secure ? ":s:" : ":p:", t.host, ":", t.port,
                estring::make_conditional_cat_list(!t.proxy_auth.empty(), ":", t.proxy_auth),
                estring::make_conditional_cat_list(!t.proxy_pool_key.empty(), ":", t.proxy_pool_key));
    return tunnelsock->connect(key, [&]() -> ISocketStream* {
        return connect_tunnel(t, timeout);
    });
}

// Ask the proxy to tunnel to the origin, then hand over the tunnel as a TLS
// stream handshaked with the origin itself -- which is the whole point of
// tunneling instead of forwarding: the proxy sees no plaintext.
ISocketStream* PooledDialer::connect_tunnel(const DialTarget& t, uint64_t timeout) {
    auto& cli = t.proxy_secure ? proxy_tls : proxy_tcp;
    if (!cli) {
        // not pooled: a tunnel is reused as a whole, never its outer leg alone
        auto tcp = new_tcp_socket_client(bind_ips.data(), bind_ips.size());
        if (!tcp)
            LOG_ERRNO_RETURN(0, nullptr, "failed to create a socket client for the proxy");
        cli.reset(t.proxy_secure ? new_tls_client(tls_ctx, tcp, true) : tcp);
    }
    auto r = get_resolver();
    auto ipaddr = r->resolve(t.proxy_host);
    if (ipaddr.undefined())
        LOG_ERROR_RETURN(ENOENT, nullptr, "DNS resolve failed for proxy, name = `", t.proxy_host);

    EndPoint ep(ipaddr, t.proxy_port);
    cli->timeout(timeout);
    auto leg = cli->connect(ep);
    if (!leg) {
        // let a retry pick another ip of the proxy
        r->discard_cache(t.proxy_host, ipaddr);
        LOG_ERRNO_RETURN(0, nullptr, "failed to connect to proxy `", ep);
    }
    bool ok = false;
    DEFER(if (!ok) delete leg);
    leg->timeout(timeout);
    if (t.proxy_secure)
        tls_stream_set_hostname(leg, estring_view(t.proxy_host).extract_c_str());
    if (tunnel_handshake(leg, t) < 0)
        return nullptr;

    // need_tunnel() implies a TLS origin, so the tunnel is always wrapped
    auto tunnel = new_tls_stream(tls_ctx, leg, SecurityRole::Client, true);
    if (!tunnel)
        LOG_ERRNO_RETURN(0, nullptr, "failed to wrap the tunnel to `:` in TLS", t.host, t.port);
    ok = true;   // owned by `tunnel` from here on
    tls_stream_set_hostname(tunnel, estring_view(t.host).extract_c_str());
    LOG_DEBUG("Tunneled to `:` through `", t.host, t.port, ep);
    return tunnel;
}

int PooledDialer::tunnel_handshake(ISocketStream* leg, const DialTarget& t) {
    // a request of its own, so that nothing of it can end up in the request the
    // caller is making -- the proxy reads these headers, the origin must not
    char buf[kTunnelReqSize];
    Request req(buf, sizeof(buf));
    req.keep_alive(true);
    if (req.reset(Verb::CONNECT, estring().appends("https://", t.host, ":", t.port)) < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to make a CONNECT for `:`", t.host, t.port);
    if (t.proxy_headers && req.headers.merge(*t.proxy_headers) < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to put the proxy headers into the CONNECT");
    if (!t.proxy_auth.empty()) {
        auto ret = req.headers.insert("Proxy-Authorization", t.proxy_auth);
        if (ret < 0 && ret != -EEXIST)   // the authenticator's own one wins
            LOG_ERRNO_RETURN(0, -1, "failed to set Proxy-Authorization on the CONNECT");
    }
    if (req.send_header(leg) < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to send CONNECT to proxy `:`", t.proxy_host, t.proxy_port);

    char resp[kTunnelRespSize];
    size_t n = 0, end;
    while (true) {
        auto ret = leg->recv(resp + n, sizeof(resp) - n);
        if (ret <= 0)
            LOG_ERRNO_RETURN(0, -1, "proxy `:` closed before answering CONNECT", t.proxy_host, t.proxy_port);
        n += ret;
        end = estring_view(resp, n).find("\r\n\r\n");
        if (end != estring_view::npos) break;
        if (n == sizeof(resp))
            LOG_ERROR_RETURN(ENOBUFS, -1, "CONNECT response header is too long");
    }
    // the client speaks first inside a tunnel, so nothing may trail the response
    if (end + 4 != n)
        LOG_ERROR_RETURN(EPROTO, -1, "proxy sent ` byte(s) past the CONNECT response", n - end - 4);

    estring_view status(resp, end);   // "HTTP/1.x SSS reason"
    if (status.size() < 12 || !status.starts_with("HTTP/1."))
        LOG_ERROR_RETURN(EPROTO, -1, "malformed CONNECT response from proxy `:`", t.proxy_host, t.proxy_port);
    auto code = status.substr(9, 3).to_uint64();
    if (code / 100 != 2)
        LOG_ERROR_RETURN(ECONNREFUSED, -1, "proxy refused to tunnel to `:`, ", t.host, t.port, VALUE(code));
    return 0;
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
        auto d = new PooledDialer(this, m_tls_ctx, m_resolver, m_bind_ips);
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
        auto s = acquire_dialer()->dial(t, tmo.timeout());
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
        return acquire_dialer()->dial(t, timeout);
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
    g_shared_resolver.at_photon_fini();
    fini_hook_registered = false;   // photon::fini() clears the hook vector
}

Client* new_http_client(ICookieJar *cookie_jar, TLSContext *tls_ctx) {
    return new ClientImpl(cookie_jar, tls_ctx);
}

} // namespace http
} // namespace net
} // namespace photon
