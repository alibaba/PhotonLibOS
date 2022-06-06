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
#include <photon/net/http/headers.h>
#include <photon/net/http/url.h>
#include <photon/common/callback.h>
#include <photon/common/object.h>
#include <photon/common/string_view.h>
#include <photon/common/stream.h>
#include <photon/common/timeout.h>

namespace photon {
namespace net {

class ICookieJar : public Object {
public:
    virtual int get_cookies_from_headers(std::string_view host, ResponseHeaders* headers) = 0;
    virtual int set_cookies_to_headers(RequestHeaders* headers) = 0;
};

class Client : public Object
{
public:
    class Operation
    {
    public:
        RequestHeaders req;                       // request headers
        Callback<IStream*> req_body_writer = {};  // request body stream
        Timeout timeout = {-1UL};                 // default timeout: unlimited
        uint16_t follow = 8;                      // default follow: 8 at most
        uint16_t retry = 5;                       // default retry: 5 at most
        ResponseHeaders resp;                     // response headers
        std::unique_ptr<IStream> resp_body;       // response body stream
        int status_code = -1;                     // status code in response
        bool enable_proxy;
        static Operation* create(Client* c, Verb v, std::string_view url,
                            uint16_t buf_size = 64 * 1024 - 1) {
            auto ptr = malloc(sizeof(Operation) + buf_size);
            return new (ptr) Operation(c, v, url, buf_size);
        }
        void set_enable_proxy(bool enable) { enable_proxy = enable; }

        int call() {
            if (!_client) return -1;
            return _client->call(this);
        }

    protected:
        Client* _client;
        char _buf[0];
        Operation(Client* c, Verb v, std::string_view url, uint16_t buf_size)
            : req(_buf, buf_size, v, url),
              enable_proxy(c->has_proxy()),
              _client(c) {}
        Operation(uint16_t buf_size) : req(_buf, buf_size) {}
    };

    Operation* new_operation(Verb v, std::string_view url,
                             uint16_t buf_size = 64 * 1024 - 1)
    {
        return Operation::create(this, v, url, buf_size);
    }

    template<uint16_t BufferSize>
    class OperationOnStack : public Operation
    {
        char _buf[BufferSize];
    public:
        OperationOnStack(Client* c, Verb v, std::string_view url) :
            Operation(c, v, url, BufferSize) { }
        OperationOnStack() : Operation(BufferSize) {}
    };

    virtual int call(Operation* /*IN, OUT*/ op) = 0;
    // get common headers, to manipulate
    virtual Headers* common_headers() = 0;

    void timeout(uint64_t timeout) { m_timeout = timeout; }
    void set_proxy(std::string_view proxy) {
        m_proxy_url.from_string(proxy);
        m_proxy = true;
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
    void timeout_ms(uint64_t tmo) { timeout(tmo * 1000UL); }
    void timeout_s(uint64_t tmo) { timeout(tmo * 1000UL * 1000UL); }
protected:
    StoredURL m_proxy_url;
    uint64_t m_timeout = -1UL;
    bool m_proxy = false;
};

//A Client without cookie_jar would ignore all response-header "Set-Cookies"
Client* new_http_client(ICookieJar *cookie_jar = nullptr);

ICookieJar* new_simple_cookie_jar();

} //namespace net
}
