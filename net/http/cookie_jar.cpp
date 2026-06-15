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

namespace photon {
namespace net {
namespace http {

using namespace std;

static uint64_t local_gmt_gap_us = 0;
static uint64_t time_gmt_to_local(uint64_t local_now) {
    if (local_gmt_gap_us == 0) {
        time_t now = time(nullptr);
        tm* gmt = gmtime(&now);
        auto now_s = mktime(gmt);
        local_gmt_gap_us = (now - now_s) * 1000ULL * 1000ULL;
    }
    return local_now + local_gmt_gap_us;
}

static uint64_t date_to_stamp(const string& date) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    strptime(date.data(), "%a, %d %b %Y %H:%M:%S", &tm);
    return time_gmt_to_local(mktime(&tm) * 1000 * 1000);
}

struct SimpleValue{
    uint64_t m_expire;
    string m_value;
};

class SimpleCookie {
public:
    unordered_map_string_key<SimpleValue> m_kv;
    int get_cookies_from_headers(Message* message)  {
        auto it = message->headers.find("Set-Cookie");
        while (it != message->headers.end() && it.first() == "Set-Cookie") {
            LOG_INFO("get cookie");
            auto Cookies = it.second();
            Parser p(Cookies);
            uint64_t expire = -1UL;
            p.skip_string("__Host-");
            p.skip_string("__Secure-");
            auto key = Cookies | p.extract_until_char('=');
            if (key.size() == 0) return -1;
            p.skip_chars('=');
            auto value = Cookies | p.extract_until_char(';');
            p.skip_until_string("Expires=");
            if (!p.is_done()) {
                p.skip_string("Expires=");
                auto date = Cookies | p.extract_until_char(';');
                expire = date_to_stamp(date);
            }
            m_kv[key] = {expire, value};
            ++it;
        }
        return 0;
    }

    int set_cookies_to_headers(Request* request) {
<<<<<<< HEAD
        bool first_kv = true;
        vector<string_view> eliminate;
        if (request->headers.insert("Cookie", "") != 0) return -1;
        uint64_t now = time(0) * 1000ULL * 1000ULL;
        for (auto& it : m_kv) {
            if (it.second.m_expire <= now) {
                eliminate.emplace_back(it.first);
                continue;
            }
            if (!first_kv) {
                if (!request->headers.value_append("; ")) return -1;
            } else first_kv = false;
            if (!request->headers.value_append(it.first) ||
                !request->headers.value_append("=") ||
                !request->headers.value_append(it.second.m_value))
                return -1;
        }
        for (auto key : eliminate) {
            m_kv.erase(key);
=======
        uint64_t now = time(0);
        auto& h = request->headers;
        bool first = true;
        for (auto it = m_cookies.begin(); it != m_cookies.end(); ) {
            if (now > it->second.expire) {
                it = m_cookies.erase(it);
                continue;
            }
            DEFER(++it);
            if (!request->secure() && starts_with(it->first, "__Secure-")) continue;
            if (!starts_with(request->abs_path(), it->second.get_path())) continue;
            auto d = it->second.get_domain();
            if (!d.empty() && d != request->host()) continue;
            size_t size = it->first.size() + 1 + it->second.value.size();
            if (first) {
                if (h.space_remain() < size + 10+6) return -1;
                h.insert("Cookie", "");
                first = false;
            } else {
                if (h.space_remain() < size + 2+6) return -1;
                h.value_append("; ");
            }
            h.value_append(it->first);
            h.value_append("=");
            h.value_append(it->second.get_value());
>>>>>>> 4aecbb9 (Fix HTTP range parsing, cookie jar, and body writev cap (#1288) (#1370))
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