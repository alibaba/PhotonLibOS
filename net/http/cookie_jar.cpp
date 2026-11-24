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
#include <photon/common/utility.h>
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
            // RFC 6265 5.2 steps 1-2: the name-value-pair ends at the first ';'
            // and must contain a '='
            auto pair = Cookies | p.extract_until_char(';');
            p.skip_spaces();
            auto eq = pair.find('=');
            if (eq == 0 || eq == pair.npos) continue;
            auto key = pair.substr(0, eq), value = pair.substr(eq + 1);
            bool HttpOnly = false;
            bool Partitioned = false;
            bool Secure = false;
            char SameSite = 0;
            // RFC 6265 5.3 step 3: the last Max-Age wins, and Max-Age wins over
            // Expires regardless of order
            bool has_max_age = false, has_expires = false;
            uint64_t max_age = 0, expires = 0;
            estring_view Path, Domain;
            while(!p.is_done()) {
                auto attr = Cookies | p.extract_until_char(';');
                p.skip_spaces();
                Parser ap(attr);
                auto k2 = attr | ap.extract_until_char('=');
                auto v2 = attr | ap.extract_until_char('\0');
                if (k2 == "Max-Age") {
                    // RFC 6265 5.2.2: the value is [-]DIGIT+, and zero or
                    // negative means "expire now"
                    auto v = v2.trim();
                    bool neg = v.starts_with("-");
                    auto digits = neg ? v.substr(1) : v;
                    if (!digits.empty() && digits.find_first_not_of(charset("0123456789")) == digits.npos) {
                        has_max_age = true;   // more than 19 significant digits saturates
                        auto nz = digits.find_first_not_of('0');
                        auto sig = (nz == digits.npos) ? estring_view() : digits.substr(nz);
                        max_age = neg ? 0 : (sig.size() > 19) ? -1ULL : sig.to_uint64();
                    } else LOG_DEBUG("invalid integer format for 'Max-Age': ", v2);
                } else if (k2 == "Expires") {
                    struct tm tm = {};
                    auto ok = strptime(v2.data(), "%a, %d %b %Y %H:%M:%S", &tm);
                    // RFC 6265 5.1.1: a year before 1601 fails to parse
                    if (!ok || tm.tm_year + 1900 < 1601) LOG_DEBUG("invalid time format for 'Expires': ", v2);
                    else {  // the date is in GMT, not local time
                        auto t = timegm(&tm);
                        has_expires = true;
                        expires = (t > 0) ? t : 0;
                    }
                }
                else if (k2 == "SameSite" && v2.size() > 0) SameSite = v2[0];
                else if (k2 == "Path") Path = v2;
                else if (k2 == "Domain") Domain = v2;
                else if (k2 == "Secure") Secure = true;
                else if (k2 == "HttpOnly") HttpOnly = true;
                else if (k2 == "Partitioned") Partitioned = true;
                else LOG_DEBUG("unknown cookie attribute ", attr);
            }
            uint64_t expire = -1ULL;
            if (has_max_age) expire = max_age ? sat_add(time(0), max_age) : 0;
            else if (has_expires) expire = expires;
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
        if (h.find("Cookie") != h.end()) return 0;
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
                if (h.insert("Cookie", "") < 0) return -1;
                first = false;
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
