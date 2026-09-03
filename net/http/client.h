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

#pragma once

#include <memory>
#include <photon/net/http/verb.h>
#include <photon/net/http/message.h>
#include <photon/net/http/url.h>
#include <photon/common/object.h>
#include <photon/common/string_view.h>
#include <photon/common/stream.h>
#include <photon/common/timeout.h>
#include <photon/net/socket.h>
#include <vector>

namespace photon {
namespace net {
class TLSContext;
class Resolver;
namespace http {

class IWebSocketStream;  // Forward declaration for websocket_connect

class ICookieJar : public Object {
public:
    virtual int get_cookies_from_headers(std::string_view host, Message* message) = 0;
    virtual int set_cookies_to_headers(Request* request) = 0;
};

// Where the connection of one HTTP request has to go. `host`/`port`/`secure`
// always describe the origin server; when a proxy is in effect the connection
// is made to the proxy instead, and the request is either forwarded in
// absolute-URI form (a plaintext origin) or tunneled with CONNECT (a TLS
// origin, which must be handshaked with the origin inside the tunnel).
struct DialTarget {
    std::string_view host;        // origin host, without port
    uint16_t port = 0;            // origin port
    bool secure = false;          // the origin speaks TLS
    std::string_view uds_path;    // if set, connect here instead of TCP
    std::string_view proxy_host;  // empty: connect to the origin directly
    uint16_t proxy_port = 0;
    bool proxy_secure = false;    // the proxy itself speaks TLS
    std::string_view proxy_auth;  // Proxy-Authorization value, may be empty
    // Headers for the proxy itself to read, and the part of the connection
    // pool key that they imply. Both are produced by the client's
    // ProxyAuthenticator, once per dial. Never shown to the origin.
    const HeadersBase* proxy_headers = nullptr;
    std::string_view proxy_pool_key;

    bool via_proxy() const { return !proxy_host.empty(); }
    // a TLS origin behind a proxy is reached by tunneling with CONNECT
    bool need_tunnel() const { return via_proxy() && secure; }
};

// What a ProxyAuthenticator produces for one dial through a proxy.
struct ProxyAuth {
    CommonHeaders<4 * 1024 - 1> headers;   // put into the CONNECT, or into a forwarded request
    estring pool_key;                      // connections differing here are never shared
};

// Called once per dial that goes through a proxy, just before connecting, so
// that the headers may be refreshed and may differ from one request to the next.
// Returns 0 on success, or a negative number to fail the request.
//
// Whatever identity the headers carry must be reflected in `pool_key`: a tunnel
// is authenticated once, by the CONNECT that opened it, so it may only be reused
// by the identity that opened it. Only the caller can tell which of its headers
// mean identity (a tenant, a route) and which are mere per-request noise (a
// trace id) -- keying on all of them would open a tunnel per request, and on
// none of them would let one identity ride another's tunnel.
using ProxyAuthenticator = Delegate<int, const DialTarget&, ProxyAuth&>;

// Establishes socket streams for HTTP clients. The built-in implementation
// maintains a connection pool per (client, vCPU), and one DNS cache shared by
// the whole process. A user-provided dialer is shared by all vCPUs the client
// runs on, so it must be safe for concurrent use across vCPUs.
class IDialer : public Object {
public:
    virtual ISocketStream* dial(const DialTarget& target, uint64_t timeout = -1ULL) = 0;
};

class Client : public Object {
public:
    class Operation;
    Operation* new_operation(Verb v, std::string_view url, uint16_t buf_size = UINT16_MAX) {
        return Operation::create(this, v, url, buf_size);
    }
    Operation* new_operation(uint16_t buf_size = UINT16_MAX) {
        return Operation::create(this, buf_size);
    }
    void destroy_operation(Operation* op) {
        op->destroy();
    }

    class Operation {
    public:
        Request req;                              // request
        Timeout timeout = {-1ULL};                 // default timeout: unlimited
        uint16_t follow = 8;                      // default follow: 8 at most
        uint16_t retry = 5;                       // default retry: 5 at most
        Response resp;                            // response
        int status_code = -1;                     // status code in response
        bool enable_proxy = false;
        std::string_view uds_path;                // If set, Unix Domain Socket will be used instead of TCP.
                                                  // URL should still be the format of http://localhost/xxx

        IStream* body_stream = nullptr;           // priority: set_body > body_stream > body_writer
        using BodyWriter = Delegate<ssize_t, Request*>;
        BodyWriter body_writer = {};

        static Operation* create(Client* c, Verb v, std::string_view url,
                            uint16_t buf_size = 64 * 1024 - 1) {
            auto ptr = malloc(sizeof(Operation) + buf_size);
            return new (ptr) Operation(c, v, url, buf_size);
        }
        static Operation* create(Client* c, uint16_t buf_size = 64 * 1024 - 1) {
            auto ptr = malloc(sizeof(Operation) + buf_size);
            return new (ptr) Operation(c, buf_size);
        }
        void destroy() {
            this->~Operation();
            free(this);
        }
        void set_enable_proxy(bool enable) {
            enable_proxy = enable;
        }
        // Set per-operation proxy URL, takes precedence over client-level proxy.
        // Automatically enables proxy and rebuilds request line to absolute URI format.
        void set_proxy(std::string_view proxy) {
            proxy_url.from_string(proxy);
            if (!enable_proxy) {
                // redirect() handles scheme+host completion and absolute URI rebuild
                req.redirect(req.verb(), req.target(), true);
                enable_proxy = true;
            }
        }
        const StoredURL* get_proxy() {
            return &proxy_url;
        }
        // Clear per-operation proxy, fall back to client-level proxy.
        // Does not change enable_proxy or request line format.
        void clear_proxy() {
            proxy_url.clear();
        }
        int call() {
            if (!_client) return -1;
            return _client->call(this);
        }
        int call(std::string_view unix_socket_path) {
            if (!_client) return -1;
            uds_path = unix_socket_path;
            return _client->call(this);
        }
        // set body buffer and set content length automatically
        void set_body(const void *buf, size_t size) {
            body_buffer = buf;
            body_buffer_size = size;
            req.headers.content_length(size);
        }
        void set_body(std::string_view buf) {
            set_body(buf.data(), buf.length());
        }


    protected:
        Client* _client;
        StoredURL proxy_url; // Per-operation proxy URL (takes precedence over
                             // client-level proxy)
        const void *body_buffer = nullptr;
        size_t body_buffer_size = 0;

        char _buf[0];
        Operation(Client* c, Verb v, std::string_view url, uint16_t buf_size)
            : req(_buf, buf_size, v, url, c->has_proxy()),
              enable_proxy(c->has_proxy()),
              _client(c) {}
        Operation(Client* c, uint16_t buf_size)
            : req(_buf, buf_size),
              enable_proxy(c->has_proxy()),
              _client(c) {}
        explicit Operation(uint16_t buf_size) : req(_buf, buf_size), _client(nullptr) {}
        Operation() = delete;
        ~Operation() = default;

        friend class ClientImpl;
    };

    template<uint16_t BufferSize = UINT16_MAX>
    class OperationOnStack : public Operation {
        char _buf[BufferSize];
    public:
        OperationOnStack(Client* c, Verb v, std::string_view url):
            Operation(c, v, url, BufferSize) {}
        explicit OperationOnStack(Client* c): Operation(c, BufferSize) {};
        OperationOnStack(): Operation(BufferSize) {}
    };

    virtual int call(Operation* /*IN, OUT*/ op) = 0;
    // get common headers, to manipulate
    virtual Headers* common_headers() = 0;

    void set_proxy(std::string_view proxy);
    // Take over the headers sent to the proxy, and the pooling of the connections
    // they authenticate. Not owned. See ProxyAuthenticator above.
    void set_proxy_authenticator(ProxyAuthenticator authenticator) {
        m_proxy_authenticator = authenticator;
    }
    void set_user_agent(std::string_view user_agent) {
        m_user_agent = std::string(user_agent);
    }
    void set_bind_ips(std::vector<IPAddr> &ips) {
        m_bind_ips = ips;
    }
    StoredURL* get_proxy() {
        return &m_proxy_url;
    }
    void enable_proxy() {
        m_proxy = true;
    }
    void disable_proxy() {
        m_proxy = false;
    }
    bool has_proxy() {
        return m_proxy;
    }
    void timeout(uint64_t timeout) { m_timeout = timeout; }
    void timeout_ms(uint64_t tmo) { timeout(tmo * 1000ULL); }
    void timeout_s(uint64_t tmo) { timeout(tmo * 1000ULL * 1000ULL); }

    // Inject a dialer to take over connection establishment (and pooling, if
    // any), replacing the built-in per-vCPU pooled dialer. Not owned; must
    // outlive the client, and must be safe for concurrent use across vCPUs.
    void set_dialer(IDialer* dialer) { m_dialer = dialer; }

    // Inject a DNS resolver, replacing the process-wide default one that the
    // built-in dialers of this client would otherwise share. Not owned; must
    // outlive the client, and must be safe for concurrent use across vCPUs.
    // No effect on a dialer set by set_dialer().
    void set_resolver(Resolver* resolver) { m_resolver = resolver; }

    virtual ISocketStream* native_connect(std::string_view host, uint16_t port,
                                          bool secure = false, uint64_t timeout = -1ULL) = 0;

    /**
     * @brief Connect to a WebSocket server
     * 
     * @param url Full URL for the WebSocket endpoint (e.g., "http://example.com/ws")
     * @param timeout Timeout in microseconds (-1 for infinite)
     * @return Pointer to IWebSocketStream on success, nullptr on failure
     */
    IWebSocketStream* websocket_connect(std::string_view url, uint64_t timeout = -1ULL);

protected:
    StoredURL m_proxy_url;
    std::string m_proxy_auth;
    ProxyAuthenticator m_proxy_authenticator;
    std::string m_user_agent;
    uint64_t m_timeout = -1ULL;
    bool m_proxy = false;
    std::vector<IPAddr> m_bind_ips;
    IDialer* m_dialer = nullptr;
    Resolver* m_resolver = nullptr;
};

// Create an HTTP client. Without cookie_jar, "Set-Cookies" headers are ignored.
// Each client owns its connection pools (created lazily, one per vCPU used),
// destroyed with the client, or at photon::fini() of the respective vCPU. The
// DNS cache behind the pools is shared by the whole process.
Client* new_http_client(ICookieJar *cookie_jar = nullptr, TLSContext *tls_ctx = nullptr);

ICookieJar* new_simple_cookie_jar();

} // namespace http
} // namespace net
} // namespace photon
