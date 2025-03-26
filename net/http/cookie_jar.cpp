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

#include <vector>
#include <time.h>
#include "client.h"
#include "parser.h"
#include <photon/common/string-keyed.h>
#include <photon/common/estring.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>

namespace photon {
namespace net {
namespace http {

using namespace std;

struct SimpleValue{
    uint64_t m_expire;
    string m_value;
};

class SimpleCookie {
public:
    unordered_map_string_key<SimpleValue> m_kv;
    int get_cookies_from_headers(Message* message)  {
        auto r = message->headers.equal_range("Set-Cookie");
        for (auto it = r.first; it != r.second; ++it) {
            LOG_DEBUG("get cookie");
            auto Cookies = it.second();
            Parser p(Cookies);
            p.skip_string("__Host-");
            p.skip_string("__Secure-");
            auto key = Cookies | p.extract_until_char('=');
            if (key.size() == 0) return -1;
            p.skip_chars('=');
            auto value = Cookies | p.extract_until_char(';');
            p.skip_chars(';'); p.skip_spaces();
            uint64_t expire = -1ULL;
            while(!p.is_done()) {
                auto arg = Cookies | p.extract_until_char(';');
                p.skip_chars(';'); p.skip_spaces();
                Parser ap(arg);
                auto k2 = arg | ap.extract_until_char('=');
                ap.skip_chars('=');
                auto v2 = arg | ap.extract_until_char('\0');
                if (k2 == "Max-Age") {
                    if (uint64_t age = v2.to_uint64())
                        expire = time(0) + age;
                } else if (k2 == "Expires") {
                    struct tm tm;
                    const char* ret = strptime(v2.data(), "%a, %d %b %Y %H:%M:%S", &tm);
                    if (ret != nullptr) // parsed successfully
                        expire = mktime(&tm);
                } else if (k2 == "Secure") {
                } else if (k2 == "HttpOnly") {
                }
            }
            m_kv[key].m_expire = expire;
            auto& v = m_kv[key].m_value;
            if (v != value) v = value;
        }
        return 0;
    }

    int set_cookies_to_headers(Request* request) {
        uint64_t now = time(0);
        auto& h = request->headers;
        for (auto it = m_kv.begin(); it != m_kv.end(); ) {
            if (now > it->second.m_expire) {
                it = m_kv.erase(it);
                continue;
            }
            size_t size = it->first.size() + 1 +
                          it->second.m_value.size();
            if (it == m_kv.begin()) {
                if (h.space_remain() < size + 10+6) return -1;
                h.insert("Cookie", "");
            } else {
                if (h.space_remain() < size + 2+6) return -1;
                h.value_append("; ");
            }
            h.value_append(it->first);
            h.value_append("=");
            h.value_append(it->second.m_value);
            ++it;
        }
        return 0;
    }
};
class SimpleCookiePtr : public std::unique_ptr<SimpleCookie> {
public:
    SimpleCookiePtr() {
        reset(new SimpleCookie());
    }
};
class SimpleCookieJar : public ICookieJar {
public:
    unordered_map_string_key<SimpleCookiePtr> m_cookie;

    int get_cookies_from_headers(string_view host, Message* message) override {
        if (host.empty()) return -1;
        return m_cookie[host]->get_cookies_from_headers(message);
    }
    int set_cookies_to_headers(Request* request) override {
        auto host = request->host();
        if (host.empty()) return -1;
        return m_cookie[host]->set_cookies_to_headers(request);
    }
};

ICookieJar* new_simple_cookie_jar() {
    return new SimpleCookieJar();
}

} // namespace http
} // namespace net
} // namespace photon