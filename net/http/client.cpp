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
#include <atomic>
#include <bitset>
#include <algorithm>
#include <photon/common/alog-stdstring.h>
#include <photon/common/estring.h>
#include <photon/common/iovector.h>
#include <photon/common/string_view.h>
#include <photon/net/socket.h>
#include <photon/net/utils.h>
#include <photon/thread/thread.h>
#include <photon/thread/vcpu-local.h>
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

namespace {

struct ResolverOwnership {
    Resolver* resolver;
    bool adopted = false;
    std::atomic<bool> released{false};
    explicit ResolverOwnership(Resolver* resolver) : resolver(resolver) {}
};

struct OwnedResolverSlot {
    std::shared_ptr<ResolverOwnership> state;
    explicit OwnedResolverSlot(std::shared_ptr<ResolverOwnership> state)
        : state(std::move(state)) {}
    ~OwnedResolverSlot() {
        // If fini wins ownership of this slot, keep its vCPU alive until every
        // client/request lease is gone. No new lease can be issued after the
        // aliasing shared_ptr's control block reaches zero.
        while (!state->released.load(std::memory_order_acquire))
            photon::thread_usleep(1000);
        if (state->adopted) delete state->resolver;
    }
};

class OwnedResolverLease final : public photon::VCPULocal<OwnedResolverSlot> {
public:
    explicit OwnedResolverLease(std::shared_ptr<ResolverOwnership> state)
        : VCPULocal({this, &OwnedResolverLease::make_slot}),
          m_state(std::move(state)) {
        get(); // bind reclamation to the resolver's registering vCPU
    }
    ~OwnedResolverLease() {
        m_state->released.store(true, std::memory_order_release);
    }
private:
    std::shared_ptr<ResolverOwnership> m_state;
    OwnedResolverSlot* make_slot() { return new OwnedResolverSlot(m_state); }
};

} // namespace

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

void Client::set_resolver(Resolver* resolver, bool ownership) {
    // Keep the old lease outside the lock scope: resolver destruction can yield
    // or reenter set_resolver(), and must never run while the spinlock is held.
    std::shared_ptr<Resolver> current;
    {
        SCOPED_LOCK(m_resolver_lock);
        current = atomic_load_resolver(&m_resolver);
        if (current.get() == resolver) return;
    }
    current.reset();

    // Preparing a vCPU slot can reap an old slot at a reused address and yield
    // during its destruction. Do this outside the setter lock. Ownership is
    // adopted only after the pointer is rechecked under that lock.
    std::shared_ptr<ResolverOwnership> state;
    std::shared_ptr<Resolver> next;
    if (resolver) {
        if (!ownership) {
            next = std::shared_ptr<Resolver>(resolver, [](Resolver*) { });
        } else {
            state = std::make_shared<ResolverOwnership>(resolver);
            if (photon::CURRENT) {
                auto lease = std::make_shared<OwnedResolverLease>(state);
                next = std::shared_ptr<Resolver>(std::move(lease), resolver);
            } else {
                next = std::shared_ptr<Resolver>(resolver, [state](Resolver* p) {
                    if (state->adopted) delete p;
                });
            }
        }
    }
    {
        SCOPED_LOCK(m_resolver_lock);
        current = atomic_load_resolver(&m_resolver);
        if (current.get() == resolver) return;
        if (state) state->adopted = true;
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

    int compose_request_headers(Request& outgoing, const Request& configured,
                                const HeadersBase* proxyHeaders) {
        // Highest priority first: per-hop proxy headers, caller headers, common
        // headers, then defaults. Classify proxy input before inserting origin
        // fields into the outgoing Message; configuration buffers stay intact.
        auto appendSource = [&](const HeadersBase& source, bool originFields) {
            for (auto item = source.begin(); item != source.end(); ) {
                auto range = source.equal_range(item.first());
                item = range.second;
                if (originFields && estring_view(range.first.first()).icmp("Proxy-Authorization") == 0)
                    continue; // this configuration belongs to DialTarget's proxy
                if (outgoing.headers.find(range.first.first()) != outgoing.headers.end())
                    continue;
                // Keep all occurrences from the winning source. A lower
                // priority source cannot replace or append to that field. Equal
                // keys in the index can be reordered; use their buffer offsets
                // to retain wire order without allocating a temporary index.
                const char* previous = nullptr;
                for (auto remaining = range.first; remaining != range.second; ++remaining) {
                    auto next = range.second;
                    for (auto entry = range.first; entry != range.second; ++entry) {
                        auto address = entry.first().data();
                        if (previous && address <= previous) continue;
                        if (next == range.second || address < next.first().data()) next = entry;
                    }
                    if (outgoing.headers.insert(next.first(), next.second(), 1) < 0)
                        return -1;
                    previous = next.first().data();
                }
            }
            return 0;
        };
        if (proxyHeaders && appendSource(*proxyHeaders, false) < 0) return -1;
        for (auto source : {static_cast<const HeadersBase*>(&configured.headers),
                            static_cast<const HeadersBase*>(&m_common_headers)}) {
            if (appendSource(*source, true) < 0) return -1;
        }
        auto agent = m_user_agent.empty() ? std::string_view(USERAGENT) : std::string_view(m_user_agent);
        for (auto item : {std::make_pair(std::string_view("User-Agent"), agent),
                          std::make_pair(std::string_view("Connection"), std::string_view("keep-alive"))}) {
            if (outgoing.headers.find(item.first) == outgoing.headers.end() &&
                outgoing.headers.insert(item.first, item.second) < 0)
                return -1;
        }
        if (m_cookie_jar && m_cookie_jar->set_cookies_to_headers(&outgoing) != 0)
            LOG_ERROR_RETURN(0, -1, "failed to set cookies on outgoing request");
        if (outgoing.headers.content_length() != 0 && outgoing.headers.chunked())
            LOG_ERROR_RETURN(EINVAL, -1, "Content-Length and Transfer-Encoding conflicted");
        return 0;
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

        // Use configuration's unused buffer region for the outgoing Message.
        // Its bytes/index never overlap the retained configuration. Fall back
        // to an owned buffer when capacity is tight or a cookie jar can add an
        // unknown amount of header data. The original free region remains the
        // response buffer after sending, preserving the existing reuse path.
        auto space = req.get_remain_space();
        size_t required = req.m_buf_size + req.headers.size() + req.headers.kv_size() +
            m_common_headers.size() + m_common_headers.kv_size() + m_user_agent.size() +
            sizeof(USERAGENT) + 128;
        if (proxy_headers) required += proxy_headers->size() + proxy_headers->kv_size();
        std::unique_ptr<char, decltype(&free)> ownedBuffer(nullptr, &free);
        char* buffer = space.first;
        auto capacity = space.second;
        if (capacity < required || m_cookie_jar) {
            capacity = req.m_buf_capacity;
            ownedBuffer.reset((char*)malloc(capacity));
            if (!ownedBuffer)
                LOG_ERROR_RETURN(ENOMEM, ROUNDTRIP_FAILED, "failed to allocate outgoing request buffer");
            buffer = ownedBuffer.get();
        }
        Request outgoing(buffer, capacity);
        if (outgoing.copy_request_line(req) < 0 ||
            compose_request_headers(outgoing, req, proxy_headers) < 0)
            return ROUNDTRIP_FAILED;
        LOG_DEBUG("Sending request ` `", req.verb(), req.target());
        if (outgoing.send_header(sock.get()) < 0) {
            sock->close();
            req.reset_status();
            LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "send header failed, retry");
        }
        sock->timeout(tmo.timeout());
        {
            // Body callbacks keep their established Request* and see the final
            // headers/framing. Restore caller configuration on every return path.
            auto exchangeBuffers = [&] {
                std::swap(req.headers, outgoing.headers);
                std::swap(req.m_buf, outgoing.m_buf);
                std::swap(req.m_buf_size, outgoing.m_buf_size);
                std::swap(req.m_buf_capacity, outgoing.m_buf_capacity);
                std::swap(req.m_buf_ownership, outgoing.m_buf_ownership);
            };
            exchangeBuffers();
            DEFER(exchangeBuffers());
            req.m_stream = sock.get();
            req.m_body_stream = std::move(outgoing.m_body_stream);
            req.reset_status(HEADER_SENT);
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
        }

        LOG_DEBUG("Request sent, wait for response ` `", req.verb(), req.target());
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
        auto proxy_auth = proxy_auth_of(op);
        auto& proxy = op->proxy_url.empty() ? m_proxy_url : op->proxy_url;
        auto auth = op->req.headers.find("Proxy-Authorization");
        auto header_proxy_auth = auth == op->req.headers.end() ?
            m_common_headers["Proxy-Authorization"] : auth.second();
        if (op->enable_proxy && !proxy.empty() && !header_proxy_auth.empty()) {
            proxy_auth.assign(header_proxy_auth.data(), header_proxy_auth.size());
        }
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
