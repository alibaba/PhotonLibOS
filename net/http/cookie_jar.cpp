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
#include <memory>
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

struct CookieItem{
    uint64_t expire;
    unique_ptr<char[]> _contents;
    rstring_view16 value, Domain, Path;
    bool HttpOnly, Partitioned, Secure;
    char SameSite;
    estring_view get_value()  { return value  | _contents.get(); }
    estring_view get_domain() { return Domain | _contents.get(); }
    estring_view get_path()   { return Path   | _contents.get(); }
    void set_contents(estring_view val, estring_view domain,
                                        estring_view path) {
        uint16_t vl = (uint16_t)val.size(),
                 dl = (uint16_t)domain.size(),
                 pl = (uint16_t)path.size();
        auto ptr = new char[0ULL + vl + dl + pl];
        memcpy(ptr, val.data(), vl);
        memcpy(ptr + vl, domain.data(), dl);
        memcpy(ptr + vl + dl, path.data(), pl);
        _contents.reset(ptr);
        Path   = {(uint16_t)(vl + dl), pl};
        Domain = {vl, dl};
        value  = {0, vl};
    }
};

class SimpleCookie {
public:
    unordered_map_string_key<CookieItem> m_cookies;
    int get_cookies_from_headers(Message* message)  {
        auto r = message->headers.equal_range("Set-Cookie");
        for (auto it = r.first; it != r.second; ++it) {
            auto Cookies = it.second();
            Parser p(Cookies);
            auto key = Cookies | p.extract_until_char('=');
            if (key.size() == 0) continue;
            auto value = Cookies | p.extract_until_char(';');
            if (value.size() == 0) continue;
            p.skip_spaces();
            bool HttpOnly = false;
            bool Partitioned = false;
            bool Secure = false;
            char SameSite = 0;
            uint64_t expire = -1ULL;
            estring_view Path, Domain;
            while(!p.is_done()) {
                auto attr = Cookies | p.extract_until_char(';');
                p.skip_spaces();
                Parser ap(attr);
                auto k2 = attr | ap.extract_until_char('=');
                auto v2 = attr | ap.extract_until_char('\0');
                if (k2 == "Max-Age") {
                    if (uint64_t age = v2.to_uint64()) expire = time(0) + age;
                    else LOG_DEBUG("invalid integer format for 'Max-Age': ", v2);
                } else if (k2 == "Expires") {
                    struct tm tm;
                    auto ok = strptime(v2.data(), "%a, %d %b %Y %H:%M:%S", &tm);
                    if (!ok) LOG_DEBUG("invalid time format for 'Expires': ", v2);
                    else expire = mktime(&tm);
                }
                else if (k2 == "SameSite" && v2.size() > 0) SameSite = v2[0];
                else if (k2 == "Path") Path = v2;
                else if (k2 == "Domain") Domain = v2;
                else if (k2 == "Secure") Secure = true;
                else if (k2 == "HttpOnly") HttpOnly = true;
                else if (k2 == "Partitioned") Partitioned = true;
                else LOG_DEBUG("unknown cookie attribute ", attr);
            }
            #define LOG_DEBUG_CONTINUE(...) { LOG_DEBUG(__VA_ARGS__); continue; }
            if (!Secure) {
                if (Partitioned)
                    LOG_DEBUG_CONTINUE(VALUE(Secure), VALUE(Partitioned));
                if (key.starts_with("__Secure-"))
                    LOG_DEBUG_CONTINUE(VALUE(Secure), VALUE(key));
                if (key.starts_with("__Host-") && (!Domain.empty() || Path != '/'))
                    LOG_DEBUG_CONTINUE(VALUE(Secure), VALUE(key), VALUE(Domain), VALUE(Path));
            }
            auto& cookie = m_cookies[key];
            if (value  != cookie.get_value() || Domain != cookie.get_domain() ||
                Path   != cookie.get_path()) cookie.set_contents(value, Domain, Path);
            cookie.expire      = expire;
            cookie.HttpOnly    = HttpOnly;
            cookie.Partitioned = Partitioned;
            cookie.Secure      = Secure;
            cookie.SameSite    = SameSite;
        }
        return 0;
    }

    bool starts_with(std::string_view s, std::string_view x) {
        return estring_view(s).starts_with(x);
    }
    int set_cookies_to_headers(Request* request) {
        uint64_t now = time(0);
        auto& h = request->headers;
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
            if (it == m_cookies.begin()) {
                if (h.space_remain() < size + 10+6) return -1;
                h.insert("Cookie", "");
            } else {
                if (h.space_remain() < size + 2+6) return -1;
                h.value_append("; ");
            }
            h.value_append(it->first);
            h.value_append("=");
            h.value_append(it->second.get_value());
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