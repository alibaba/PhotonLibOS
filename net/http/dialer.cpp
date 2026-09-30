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

namespace photon {
namespace net {
namespace http {

static const uint64_t kDNSCacheLife = 3600ULL * 1000 * 1000;
static const uint64_t kResolverDrainTimeout = 3ULL * 1000 * 1000;
static constexpr size_t kTunnelRespSize = 4 * 1024;
static constexpr uint16_t kTunnelReqSize = 8 * 1024 - 1;

class SharedResolver {
public:
    struct Gen {
        Resolver* resolver;
        uint32_t users = 0;
    };

    class Ref {
    public:
        Ref(SharedResolver* owner, Gen* gen, Resolver* resolver)
            : m_owner(owner), m_gen(gen), m_resolver(resolver) {}
        explicit Ref(std::shared_ptr<Resolver> resolver)
            : m_owner(nullptr), m_gen(nullptr), m_resolver(resolver.get()),
              m_owned(std::move(resolver)) {}
        Ref(Ref&& rhs)
            : m_owner(rhs.m_owner), m_gen(rhs.m_gen),
              m_resolver(rhs.m_resolver), m_owned(std::move(rhs.m_owned)) {
            rhs.m_resolver = nullptr;
        }
        Ref(const Ref&) = delete;
        ~Ref() {
            if (m_owner && m_resolver) m_owner->put(m_gen);
        }
        Resolver* operator->() const { return m_resolver; }

    protected:
        SharedResolver* m_owner;
        Gen* m_gen;
        Resolver* m_resolver;
        std::shared_ptr<Resolver> m_owned;
    };

    Ref borrow() {
        {
            SCOPED_LOCK(m_lock);
            if (m_current)
                return ++m_current->users,
                       Ref(this, m_current, m_current->resolver);
        }
        auto resolver = new_default_resolver(kDNSCacheLife);
        Resolver* redundant = nullptr;
        Gen* gen;
        {
            SCOPED_LOCK(m_lock);
            if (m_current) {
                redundant = resolver;
            } else {
                m_current = new Gen{resolver, 0};
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
        Timeout timeout(kResolverDrainTimeout);
        while (true) {
            uint32_t users;
            {
                SCOPED_LOCK(m_lock);
                users = gen->users;
            }
            if (users == 0) break;
            if (timeout.expired())
                LOG_ERROR_RETURN(0, , "DNS cache is still borrowed by other vCPUs, leaking it, ", VALUE(users));
            photon::thread_usleep(1000);
        }
        delete gen->resolver;
        delete gen;
    }

protected:
    photon::spinlock m_lock;
    Gen* m_current = nullptr;
    vcpu_base* m_vcpu = nullptr;
    bool m_hook = false;

    void put(Gen* gen) {
        SCOPED_LOCK(m_lock);
        --gen->users;
    }
};

static SharedResolver g_shared_resolver;

class ForwardDialer : public IDialer {
public:
    ForwardDialer(IDialer* underlay, bool ownership)
        : m_underlay(underlay), m_ownership(ownership) {}
    explicit ForwardDialer(std::unique_ptr<IDialer> underlay)
        : ForwardDialer(underlay.release(), true) {}
    ~ForwardDialer() override {
        if (m_ownership) delete m_underlay;
    }

protected:
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

protected:
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

protected:
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

protected:
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
            request.headers.merge(*target.proxy_headers) < 0)
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
        estring_view status(response, end);
        if (status.size() < 12 || !status.starts_with("HTTP/1."))
            LOG_ERROR_RETURN(EPROTO, -1,
                             "malformed CONNECT response from proxy `:`",
                             target.proxy_host, target.proxy_port);
        auto code = status.substr(9, 3).to_uint64();
        if (code / 100 != 2)
            LOG_ERROR_RETURN(ECONNREFUSED, -1,
                             "proxy refused to tunnel to `:`, ", target.host,
                             target.port, VALUE(code));
        return 0;
    }
};

class PoolDialer : public ForwardDialer {
public:
    PoolDialer(IDialer* underlay, bool ownership, uint64_t expiration)
        : ForwardDialer(underlay, ownership),
          m_pool(new_tcp_socket_pool(nullptr, expiration, false)) {}
    PoolDialer(std::unique_ptr<IDialer> underlay, uint64_t expiration)
        : ForwardDialer(std::move(underlay)),
          m_pool(new_tcp_socket_pool(nullptr, expiration, false)) {}

    ISocketStream* dial(const DialTarget& target, uint64_t timeout) override {
        auto key = make_key(target);
        auto stream = m_pool->connect(key, [&]() {
            return m_underlay->dial(target, timeout);
        });
        if (stream) stream->timeout(timeout);
        return stream;
    }

protected:
    std::unique_ptr<ISocketPool> m_pool;

    static void append_u8(std::string& key, uint8_t value) {
        key.push_back((char)value);
    }
    static void append_u16(std::string& key, uint16_t value) {
        key.append((const char*)&value, sizeof(value));
    }
    static void append_string(std::string& key, std::string_view value) {
        uint64_t size = value.size();
        key.append((const char*)&size, sizeof(size));
        if (!value.empty()) key.append(value.data(), value.size());
    }
    static std::string make_key(const DialTarget& target) {
        std::string key;
        append_u8(key, 1); // key format version
        if (!target.uds_path.empty()) {
            append_u8(key, 1);
            append_string(key, target.uds_path);
            append_u8(key, target.secure);
            append_string(key, target.host);
            append_u16(key, target.port);
        } else if (target.need_tunnel()) {
            append_u8(key, 4);
            append_u8(key, target.proxy_secure);
            append_string(key, target.proxy_host);
            append_u16(key, target.proxy_port);
            append_string(key, target.host);
            append_u16(key, target.port);
            append_string(key, target.proxy_auth);
            append_string(key, target.proxy_pool_key);
        } else if (target.via_proxy()) {
            append_u8(key, 3);
            append_u8(key, target.proxy_secure);
            append_string(key, target.proxy_host);
            append_u16(key, target.proxy_port);
            append_string(key, target.proxy_auth);
            append_string(key, target.proxy_pool_key);
        } else {
            append_u8(key, 2);
            append_u8(key, target.secure);
            append_string(key, target.host);
            append_u16(key, target.port);
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

protected:
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
