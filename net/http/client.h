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
namespace http {

class ICookieJar : public Object {
public:
    virtual int get_cookies_from_headers(std::string_view host, Message* message) = 0;
    virtual int set_cookies_to_headers(Request* request) = 0;
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
        Timeout timeout = {-1UL};                 // default timeout: unlimited
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

    void set_proxy(std::string_view proxy) {
        m_proxy_url.from_string(proxy);
        m_proxy = true;
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
    void timeout_ms(uint64_t tmo) { timeout(tmo * 1000UL); }
    void timeout_s(uint64_t tmo) { timeout(tmo * 1000UL * 1000UL); }

    virtual ISocketStream* native_connect(std::string_view host, uint16_t port,
                                          bool secure = false, uint64_t timeout = -1UL) = 0;
protected:
    StoredURL m_proxy_url;
    std::string m_user_agent;
    uint64_t m_timeout = -1UL;
    bool m_proxy = false;
    std::vector<IPAddr> m_bind_ips;
};

//A Client without cookie_jar would ignore all response-header "Set-Cookies"
Client* new_http_client(ICookieJar *cookie_jar = nullptr, TLSContext *tls_ctx = nullptr);

ICookieJar* new_simple_cookie_jar();

} // namespace http
} // namespace net
} // namespace photon
