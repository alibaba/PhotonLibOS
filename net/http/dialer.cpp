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

#include "dialer.h"
#include "message.h"

#include <memory>
#include <pthread.h>
#include <string>
#include <utility>

#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/estring.h>
#include <photon/common/timeout.h>
#include <photon/net/security-context/tls-stream.h>
#include <photon/net/utils.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/vcpu-local.h>
#include "../base_socket.h"

namespace photon {
namespace net {

// Internal hook implemented alongside DefaultResolver in net/utils.cpp.
void abandon_default_resolver_after_fork(Resolver* resolver);

namespace http {

static const uint64_t kDNSCacheLife = 3600ULL * 1000 * 1000;
static constexpr size_t kTunnelRespSize = 4 * 1024;
static constexpr uint16_t kTunnelReqSize = 8 * 1024 - 1;

namespace {

class SharedResolver;
static photon::spinlock g_resolvers_lock;
static SharedResolver* g_resolvers = nullptr;
static bool g_resolvers_atfork_registered = false;

class SharedResolver {
public:
    struct Gen {
        Resolver* resolver;
        uint32_t users = 0;
        uint64_t epoch = 0;
        vcpu_base* owner = nullptr;
        Gen* next = nullptr;
    };

    SharedResolver()
        : SharedResolver({nullptr, &make_default_resolver},
                         {nullptr, &abandon_default_resolver}) {}

    explicit SharedResolver(Delegate<Resolver*> factory,
                            Delegate<void, Resolver*> abandon = {})
        : m_factory(factory), m_abandon(abandon) {
        SCOPED_LOCK(g_resolvers_lock);
        if (!g_resolvers_atfork_registered) {
            auto ret = pthread_atfork(&atfork_prepare, &atfork_parent,
                                      &atfork_child);
            if (ret == 0)
                g_resolvers_atfork_registered = true;
            else
                LOG_ERROR("resolver pthread_atfork failed, ", VALUE(ret));
        }
        m_next = g_resolvers;
        g_resolvers = this;
    }

    ~SharedResolver() {
        SCOPED_LOCK(g_resolvers_lock);
        for (auto p = &g_resolvers; *p; p = &(*p)->m_next) {
            if (*p != this) continue;
            *p = m_next;
            break;
        }
    }

    class Ref {
    public:
        Ref(SharedResolver* owner, Gen* gen, Resolver* resolver)
            : m_owner(owner), m_gen(gen), m_resolver(resolver),
              m_epoch(gen->epoch) {}
        explicit Ref(std::shared_ptr<Resolver> resolver)
            : m_owner(nullptr), m_gen(nullptr), m_resolver(resolver.get()),
              m_owned(std::move(resolver)) {}
        Ref(Ref&& rhs)
            : m_owner(rhs.m_owner), m_gen(rhs.m_gen),
              m_resolver(rhs.m_resolver), m_owned(std::move(rhs.m_owned)),
              m_epoch(rhs.m_epoch) {
            rhs.m_resolver = nullptr;
        }
        Ref(const Ref&) = delete;
        ~Ref() {
            if (m_owner && m_resolver) m_owner->put(m_gen, m_epoch);
        }
        Resolver* operator->() const { return m_resolver; }

    protected:
        SharedResolver* m_owner;
        Gen* m_gen;
        Resolver* m_resolver;
        std::shared_ptr<Resolver> m_owned;
        uint64_t m_epoch = 0;
    };

    Ref borrow() {
        {
            SCOPED_LOCK(m_lock);
            if (m_current)
                return ++m_current->users,
                       Ref(this, m_current, m_current->resolver);
        }
        auto resolver = m_factory();
        Resolver* redundant = nullptr;
        Gen* gen;
        {
            SCOPED_LOCK(m_lock);
            if (m_current) {
                redundant = resolver;
            } else {
                m_current = new Gen{resolver, 0, m_epoch, photon::get_vcpu(),
                                    m_generations};
                m_generations = m_current;
                m_vcpu = photon::get_vcpu();
                if (!m_hook) {
                    m_hook = true;
                    photon::fini_hook({this, &SharedResolver::at_photon_fini});
                }
            }
            gen = m_current;
            ++gen->users;
        }
        delete redundant;
        return {this, gen, gen->resolver};
    }

    void at_photon_fini() {
        Gen* gen;
        {
            SCOPED_LOCK(m_lock);
            if (!m_current || m_vcpu != photon::get_vcpu()) return;
            gen = m_current;
            m_current = nullptr;
            m_vcpu = nullptr;
            m_hook = false;
        }
        // Keep the owning vCPU alive until this generation's final lease is
        // returned. New borrowers use a new generation and cannot extend this
        // drain. A finite timeout would permanently leak a cache and its timer.
        while (true) {
            uint32_t users;
            {
                SCOPED_LOCK(m_lock);
                if (gen->epoch != m_epoch) return; // inherited drain after fork
                users = gen->users;
            }
            if (users == 0) break;
            photon::thread_usleep(1000);
        }
        delete gen->resolver;
        {
            SCOPED_LOCK(m_lock);
            for (auto p = &m_generations; *p; p = &(*p)->next) {
                if (*p != gen) continue;
                *p = gen->next;
                break;
            }
        }
        delete gen;
    }

public:
    photon::spinlock m_lock;
    Gen* m_current = nullptr;
    Gen* m_generations = nullptr; // includes unpublished generations draining
    vcpu_base* m_vcpu = nullptr;
    bool m_hook = false;
    uint64_t m_epoch = 1;
    Delegate<Resolver*> m_factory;
    Delegate<void, Resolver*> m_abandon;
    SharedResolver* m_next = nullptr;

    void put(Gen* gen, uint64_t epoch) {
        SCOPED_LOCK(m_lock);
        // A Ref inherited across fork must not touch its parent generation.
        if (epoch != m_epoch) return;
        --gen->users;
    }

    static Resolver* make_default_resolver(void*) {
        return new_default_resolver(kDNSCacheLife);
    }

    static void abandon_default_resolver(void*, Resolver* resolver) {
        abandon_default_resolver_after_fork(resolver);
    }

    static void atfork_prepare() {
        g_resolvers_lock.lock();
        for (auto p = g_resolvers; p; p = p->m_next) p->m_lock.lock();
    }

    static void atfork_parent() {
        for (auto p = g_resolvers; p; p = p->m_next) p->m_lock.unlock();
        g_resolvers_lock.unlock();
    }

    static void atfork_child() {
        for (auto p = g_resolvers; p; p = p->m_next) {
            ++p->m_epoch;
            if (p->m_abandon && photon::CURRENT) {
                for (auto gen = p->m_generations; gen; gen = gen->next) {
                    if (gen->owner == photon::get_vcpu())
                        p->m_abandon(gen->resolver);
                }
            }
            // Abandon parent caches and leases without invoking destructors;
            // inherited workers on this vCPU have been stopped above.
            p->m_current = nullptr;
            p->m_generations = nullptr;
            p->m_vcpu = nullptr;
            p->m_hook = false;
        }
        atfork_parent();
    }
};

static SharedResolver g_shared_resolver;

} // namespace

class ForwardDialer : public IDialer {
public:
    ForwardDialer(IDialer* underlay, bool ownership)
        : m_underlay(underlay), m_ownership(ownership) {}
    explicit ForwardDialer(std::unique_ptr<IDialer> underlay)
        : ForwardDialer(underlay.release(), true) {}
    ~ForwardDialer() override {
        if (m_ownership) delete m_underlay;
    }

public:
    IDialer* m_underlay;
    bool m_ownership;
};

class TransportDialer : public IDialer {
public:
    explicit TransportDialer(const std::vector<IPAddr>& bind_ips)
        : m_bind_ips(bind_ips),
          m_tcp(new_tcp_socket_client(m_bind_ips.data(), m_bind_ips.size())),
          m_uds(new_uds_client()) {}

    ISocketStream* dial(const DialTarget& target, uint64_t timeout) override {
        if (!target.uds_path.empty()) {
            if (!m_uds)
                LOG_ERROR_RETURN(ENOMEM, nullptr, "failed to create the UDS client");
            m_uds->timeout(timeout);
            auto stream = m_uds->connect(target.uds_path.data(),
                                         target.uds_path.size());
            if (!stream)
                LOG_ERRNO_RETURN(0, nullptr, "failed to dial unix socket `",
                                 target.uds_path);
            return stream;
        }
        if (!m_tcp)
            LOG_ERROR_RETURN(ENOMEM, nullptr, "failed to create the TCP client");

        auto host = target.via_proxy() ? target.proxy_host : target.host;
        auto port = target.via_proxy() ? target.proxy_port : target.port;
        auto resolver = get_resolver(target);
        auto address = resolver->resolve(host);
        if (address.undefined())
            LOG_ERROR_RETURN(ENOENT, nullptr, "DNS resolve failed, name = `", host);

        EndPoint endpoint(address, port);
        m_tcp->timeout(timeout);
        auto stream = m_tcp->connect(endpoint);
        if (stream) {
            LOG_DEBUG("Connected ` ", endpoint, VALUE(host));
            return stream;
        }
        resolver->discard_cache(host, address);
        LOG_ERRNO_RETURN(0, nullptr, "connection failed, ` ", endpoint,
                         VALUE(host));
    }

public:
    std::vector<IPAddr> m_bind_ips;
    std::unique_ptr<ISocketClient> m_tcp;
    std::unique_ptr<ISocketClient> m_uds;

    SharedResolver::Ref get_resolver(const DialTarget& target) {
        if (target.resolver)
            return SharedResolver::Ref(target.resolver);
        return g_shared_resolver.borrow();
    }
};

class TLSDialer : public IDialer {
public:
    TLSDialer(TLSContext* context, IDialer* underlay, TLSLayer layer,
              bool ownership, bool context_ownership)
        : m_context(context), m_underlay(underlay), m_layer(layer),
          m_ownership(ownership), m_context_ownership(context_ownership) {}
    TLSDialer(std::unique_ptr<IDialer> underlay, TLSContext* context,
              TLSLayer layer)
        : TLSDialer(context, underlay.release(), layer, true, false) {}
    TLSDialer(std::unique_ptr<IDialer> underlay,
              std::unique_ptr<TLSContext> context, TLSLayer layer)
        : TLSDialer(context.release(), underlay.release(), layer, true, true) {}
    ~TLSDialer() override {
        if (m_ownership) delete m_underlay;
        if (m_context_ownership) delete m_context;
    }

    ISocketStream* dial(const DialTarget& target, uint64_t timeout) override {
        auto stream = m_underlay->dial(target, timeout);
        if (!stream || !applies(target)) return stream;
        std::unique_ptr<ISocketStream> owner(stream);
        auto tls = new_tls_stream(m_context, stream, SecurityRole::Client, true);
        if (!tls)
            LOG_ERRNO_RETURN(0, nullptr, "failed to wrap the dialed stream in TLS");
        auto transferred = owner.release();
        (void)transferred;
        auto host = m_layer == TLSLayer::PROXY ? target.proxy_host : target.host;
        if (set_identity(tls, host) < 0) {
            delete tls;
            LOG_ERROR_RETURN(0, nullptr, "failed to set TLS identity to `", host);
        }
        return tls;
    }

public:
    TLSContext* m_context;
    IDialer* m_underlay;
    TLSLayer m_layer;
    bool m_ownership;
    bool m_context_ownership;

    bool applies(const DialTarget& target) const {
        if (!target.uds_path.empty()) return false;
        if (m_layer == TLSLayer::PROXY)
            return target.via_proxy() && target.proxy_secure;
        return target.secure;
    }

    int set_identity(ISocketStream* stream, std::string_view host) {
        auto name = estring().appends(host);
        auto verifying = ((int)m_context->get_verify_mode() &
                          (int)VerifyMode::PEER) != 0;
        return verifying ? tls_stream_set_hostname(stream, name.c_str())
                         : tls_stream_set_sni(stream, name.c_str());
    }
};

class ConnectTunnelDialer : public ForwardDialer {
public:
    using ForwardDialer::ForwardDialer;

    ISocketStream* dial(const DialTarget& target, uint64_t timeout) override {
        auto stream = m_underlay->dial(target, timeout);
        if (!stream || !target.need_tunnel()) return stream;
        std::unique_ptr<ISocketStream> owner(stream);
        stream->timeout(timeout);
        if (handshake(stream, target) < 0) return nullptr;
        return owner.release();
    }

public:
    int handshake(ISocketStream* stream, const DialTarget& target) {
        char buf[kTunnelReqSize];
        Request request(buf, sizeof(buf));
        request.keep_alive(true);
        if (request.reset(Verb::CONNECT,
                          estring().appends("https://", target.host, ":",
                                            target.port)) < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to make a CONNECT for `:`",
                             target.host, target.port);
        if (target.proxy_headers &&
            request.headers.merge(*target.proxy_headers, 1) < 0)
            LOG_ERRNO_RETURN(0, -1,
                             "failed to put proxy headers into CONNECT");
        if (!target.proxy_auth.empty()) {
            auto ret = request.headers.insert("Proxy-Authorization",
                                              target.proxy_auth);
            if (ret < 0 && ret != -EEXIST)
                LOG_ERRNO_RETURN(0, -1,
                                 "failed to set Proxy-Authorization on CONNECT");
        }
        if (request.send_header(stream) < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to send CONNECT to proxy `:`",
                             target.proxy_host, target.proxy_port);

        char response[kTunnelRespSize];
        size_t size = 0;
        size_t end;
        while (true) {
            auto ret = stream->recv(response + size, sizeof(response) - size);
            if (ret <= 0)
                LOG_ERRNO_RETURN(0, -1,
                                 "proxy closed before answering CONNECT, `:`",
                                 target.proxy_host, target.proxy_port);
            size += ret;
            end = estring_view(response, size).find("\r\n\r\n");
            if (end != estring_view::npos) break;
            if (size == sizeof(response))
                LOG_ERROR_RETURN(ENOBUFS, -1,
                                 "CONNECT response header is too long");
        }
        if (end + 4 != size)
            LOG_ERROR_RETURN(EPROTO, -1,
                             "proxy sent ` byte(s) past CONNECT response",
                             size - end - 4);
        auto response_headers = estring_view(response, end);
        auto status = response_headers.substr(0, response_headers.find("\r\n"));
        auto is_digit = [](char ch) { return ch >= '0' && ch <= '9'; };
        if (status.size() < 12 || !status.starts_with("HTTP/1.") ||
            !is_digit(status[7]) || status[8] != ' ' ||
            !is_digit(status[9]) || !is_digit(status[10]) ||
            !is_digit(status[11]) ||
            (status.size() > 12 && status[12] != ' '))
            LOG_ERROR_RETURN(EPROTO, -1,
                             "malformed CONNECT response from proxy `:`",
                             target.proxy_host, target.proxy_port);
        auto code = (status[9] - '0') * 100 + (status[10] - '0') * 10 +
                    status[11] - '0';
        if (code / 100 != 2)
            LOG_ERROR_RETURN(ECONNREFUSED, -1,
                             "proxy refused to tunnel to `:`, ", target.host,
                             target.port, VALUE(code));
        return 0;
    }
};

struct PoolDialerState {
    // Member order keeps the entire underlay (including owned TLS contexts)
    // alive until the pool has closed its idle sockets and stopped collecting.
    std::unique_ptr<IDialer> ownedUnderlay;
    IDialer* underlay;
    std::unique_ptr<ISocketPool> pool;

    PoolDialerState(IDialer* underlay, bool ownership, uint64_t expiration)
        : ownedUnderlay(ownership ? underlay : nullptr), underlay(underlay),
          pool(new_tcp_socket_pool(nullptr, expiration, false)) {}
    PoolDialerState(std::unique_ptr<IDialer> underlay, uint64_t expiration)
        : PoolDialerState(underlay.release(), true, expiration) {}
};

class LeasedPoolStream : public ForwardSocketStream {
public:
    LeasedPoolStream(ISocketStream* stream, std::shared_ptr<PoolDialerState> state)
        : ForwardSocketStream(stream, true), m_state(std::move(state)) {}
    ~LeasedPoolStream() override {
        // Return/drop the checked-out socket while its pool is still alive,
        // before releasing our state member and then running the base dtor.
        safe_delete(m_underlay);
    }
    int close() override { return m_underlay->close(); }
    int shutdown(ShutdownHow how) override { return m_underlay->shutdown(how); }
    ssize_t read(void* buf, size_t count) override { return m_underlay->read(buf, count); }
    ssize_t readv(const iovec* iov, int count) override { return m_underlay->readv(iov, count); }
    ssize_t readv_mutable(iovec* iov, int count) override { return m_underlay->readv_mutable(iov, count); }
    ssize_t write(const void* buf, size_t count) override { return m_underlay->write(buf, count); }
    ssize_t writev(const iovec* iov, int count) override { return m_underlay->writev(iov, count); }
    ssize_t writev_mutable(iovec* iov, int count) override { return m_underlay->writev_mutable(iov, count); }
    ssize_t recv(void* buf, size_t count, int flags = 0) override { return m_underlay->recv(buf, count, flags); }
    ssize_t recv(const iovec* iov, int count, int flags = 0) override { return m_underlay->recv(iov, count, flags); }
    ssize_t send(const void* buf, size_t count, int flags = 0) override { return m_underlay->send(buf, count, flags); }
    ssize_t send(const iovec* iov, int count, int flags = 0) override { return m_underlay->send(iov, count, flags); }
    ssize_t sendfile(int fd, off_t offset, size_t count) override { return m_underlay->sendfile(fd, offset, count); }
    Object* get_underlay_object(uint64_t recursion = 0) override {
        // Preserve the pooled stream's original introspection/FD semantics.
        return m_underlay->get_underlay_object(recursion);
    }
public:
    std::shared_ptr<PoolDialerState> m_state;
};

class PoolDialer : public IDialer {
public:
    PoolDialer(IDialer* underlay, bool ownership, uint64_t expiration)
        : m_state(std::make_shared<PoolDialerState>(underlay, ownership, expiration)) {}
    PoolDialer(std::unique_ptr<IDialer> underlay, uint64_t expiration)
        : m_state(std::make_shared<PoolDialerState>(std::move(underlay), expiration)) {}

    ISocketStream* dial(const DialTarget& target, uint64_t timeout) override {
        auto key = make_key(target);
        // The socket pool stores key lengths in uint16_t and reserves the
        // maximum value. Reject before either lookup or connection creation.
        if (key.size() >= UINT16_MAX)
            LOG_ERROR_RETURN(ENAMETOOLONG, nullptr,
                             "HTTP route key is too long: ` bytes", key.size());
        std::unique_ptr<ISocketStream> stream(m_state->pool->connect(key, [&]() {
            return m_state->underlay->dial(target, timeout);
        }));
        if (!stream) return nullptr;
        stream->timeout(timeout);
        auto result = new LeasedPoolStream(stream.get(), m_state);
        stream.release();
        return result;
    }

public:
    std::shared_ptr<PoolDialerState> m_state;

    static std::string make_key(const DialTarget& target) {
        estring key;
        key.appends(uint32_t(2), "/"); // key format version
        if (!target.uds_path.empty()) {
            key.appends(uint32_t(1), "/", estring::length_prefixed(target.uds_path),
                        uint32_t(target.secure), "/",
                        estring::length_prefixed(target.host), uint32_t(target.port), "/");
        } else if (target.need_tunnel()) {
            key.appends(uint32_t(4), "/", uint32_t(target.proxy_secure), "/",
                        estring::length_prefixed(target.proxy_host), uint32_t(target.proxy_port), "/",
                        estring::length_prefixed(target.host), uint32_t(target.port), "/",
                        estring::length_prefixed(target.proxy_auth),
                        estring::length_prefixed(target.proxy_pool_key));
        } else if (target.via_proxy()) {
            key.appends(uint32_t(3), "/", uint32_t(target.proxy_secure), "/",
                        estring::length_prefixed(target.proxy_host), uint32_t(target.proxy_port), "/",
                        estring::length_prefixed(target.proxy_auth),
                        estring::length_prefixed(target.proxy_pool_key));
        } else {
            key.appends(uint32_t(2), "/", uint32_t(target.secure), "/",
                        estring::length_prefixed(target.host), uint32_t(target.port), "/");
        }
        return key;
    }
};

class VCPULocalDialer : public IDialer {
public:
    explicit VCPULocalDialer(Delegate<IDialer*> factory) : m_local(factory) {}

    ISocketStream* dial(const DialTarget& target, uint64_t timeout) override {
        auto dialer = m_local.get();
        if (!dialer)
            LOG_ERROR_RETURN(ENOMEM, nullptr,
                             "failed to create the vCPU-local dialer");
        return dialer->dial(target, timeout);
    }

public:
    VCPULocal<IDialer> m_local;
};

IDialer* new_transport_dialer(const std::vector<IPAddr>& bind_ips) {
    return new TransportDialer(bind_ips);
}

IDialer* new_tls_dialer(TLSContext* context, IDialer* underlay,
                        TLSLayer layer, bool ownership,
                        bool context_ownership) {
    if (!context || !underlay)
        LOG_ERROR_RETURN(EINVAL, nullptr, "TLS context and underlay are required");
    return new TLSDialer(context, underlay, layer, ownership,
                         context_ownership);
}

IDialer* new_connect_tunnel_dialer(IDialer* underlay, bool ownership) {
    if (!underlay)
        LOG_ERROR_RETURN(EINVAL, nullptr, "CONNECT tunnel underlay is required");
    return new ConnectTunnelDialer(underlay, ownership);
}

IDialer* new_pool_dialer(IDialer* underlay, bool ownership,
                         uint64_t expiration) {
    if (!underlay)
        LOG_ERROR_RETURN(EINVAL, nullptr, "pool underlay is required");
    return new PoolDialer(underlay, ownership, expiration);
}

template <typename Layer, typename... Args>
static void add_layer(std::unique_ptr<IDialer>& chain, Args&&... args) {
    chain = std::make_unique<Layer>(std::move(chain),
                                    std::forward<Args>(args)...);
}

IDialer* new_http_dialer(TLSContext* context,
                         const std::vector<IPAddr>& bind_ips) {
    std::unique_ptr<TLSContext> owned_context;
    if (!context) {
        owned_context.reset(new_tls_context(nullptr, nullptr, nullptr));
        if (!owned_context) return nullptr;
        owned_context->set_verify_mode(VerifyMode::PEER);
        context = owned_context.get();
    }

    std::unique_ptr<IDialer> chain(new_transport_dialer(bind_ips));
    if (!chain) return nullptr;
    add_layer<TLSDialer>(chain, context, TLSLayer::PROXY);
    add_layer<ConnectTunnelDialer>(chain);
    if (owned_context)
        add_layer<TLSDialer>(chain, std::move(owned_context),
                             TLSLayer::ORIGIN);
    else
        add_layer<TLSDialer>(chain, context, TLSLayer::ORIGIN);
    add_layer<PoolDialer>(chain, -1ULL);
    return chain.release();
}

IDialer* new_vcpu_local_dialer(Delegate<IDialer*> factory) {
    if (!factory)
        LOG_ERROR_RETURN(EINVAL, nullptr, "vCPU-local Dialer factory is required");
    return new VCPULocalDialer(factory);
}

} // namespace http
} // namespace net
} // namespace photon
