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

// RFC 6265 5.1.1: delimiter = %x09 / %x20-2F / %x3B-40 / %x5B-60 / %x7B-7E
static bool date_delimiter(unsigned char c) {
    return (c == 0x09) ||
           (c >= 0x20 && c <= 0x2F) || (c >= 0x3B && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E) ;
}
// Returns the number of characters consumed, or 0 when the token does not match.
// to_uint64_check() eats the whole digit run, so the "( non-digit *OCTET )" tail
// of the productions needs no separate check.
static size_t match_digits(estring_view t, size_t least, size_t most, int16_t* v) {
    uint64_t val;
    auto n = t.to_uint64_check(&val);
    if (n < least || n > most) return 0;
    *v = (int16_t)val;
    return n;
}
// month = ( "jan" / "feb" / ... / "dec" ) *OCTET
static bool match_month(estring_view t, int16_t* v) {
    static const char names[][4] = {"jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
    for (int16_t i = 0; i < 12; i++)
        if (t.istarts_with(names[i])) {
            *v = i;
            return true;
        }
    return false;
}
// time = hms-time ( non-digit *OCTET ), hms-time = time-field ":" time-field ":" time-field
static bool match_time(estring_view t, int16_t* h, int16_t* m, int16_t* s) {
    int16_t f[3];
    size_t i = 0;
    for (int k = 0; k < 3; k++) {
        auto n = match_digits(t.substr(i), 1, 2, &f[k]);
        if (!n) return false;
        i += n;
        if (k < 2) {
            if (i >= t.size() || t[i] != ':') return false;
            i++;
        }
    }
    *h = f[0]; *m = f[1]; *s = f[2];
    return true;
}
// RFC 6265 5.1.1, the deliberately tolerant cookie-date parser.
static bool parse_cookie_date(estring_view sv, uint64_t* expiry) {
    int16_t hour = -1, minute = -1, second = -1, day = -1, month = -1, year = -1;
    for (size_t i = 0; i < sv.size(); ) {
        while (i < sv.size() && date_delimiter(sv[i])) i++;
        size_t j = i;
        while (j < sv.size() && !date_delimiter(sv[j])) j++;
        if (j == i) break;
        auto token = sv.substr(i, j - i);
        i = j;
        if (hour  < 0 && match_time(token, &hour, &minute, &second)) continue;
        if (day   < 0 && match_digits(token, 1, 2, &day))            continue;
        if (month < 0 && match_month(token, &month))                 continue;
        if (year  < 0 && match_digits(token, 2, 4, &year))           continue;
    }
    if (year >= 70 && year <= 99) year += 1900;
    else if (year >= 0 && year <= 69) year += 2000;
    if (hour < 0 || day < 0 || month < 0 || year < 0) return false;
    if (day < 1 || day > 31 || year < 1601) return false;
    if (hour > 23 || minute > 59 || second > 59) return false;
    // step 6: fail when no such date exists, e.g. Feb 30
    static const uint8_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int16_t limit = month_days[month];
    if (month == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) limit = 29;
    if (day > limit) return false;
    struct tm tm = {};
    tm.tm_year = (int)year - 1900;
    tm.tm_mon  = (int)month;
    tm.tm_mday = (int)day;
    tm.tm_hour = (int)hour;
    tm.tm_min  = (int)minute;
    tm.tm_sec  = (int)second;
    auto t = timegm(&tm); // step 6: the parsed-cookie-date is in UTC
    // 5.2.1 MAY replace an expiry earlier than the earliest representable date
    *expiry = (t > 0) ? (uint64_t)t : 0;
    return true;
}

class SimpleCookie {
public:
    unordered_map_string_key<CookieItem> m_cookies;
    int get_cookies_from_headers(Message* message)  {
        auto r = message->headers.equal_range("Set-Cookie");
        uint64_t now = time(0);
        for (auto it = r.first; it != r.second; ++it) {
            auto Cookies = it.second();
            Parser p(Cookies);
            // 5.2 step 1: the name-value pair ends at the first ';'
            auto nv = Cookies | p.extract_until_char(';');
            // 5.2 step 2
            auto eq = nv.find('=');
            if (eq == nv.npos) continue;
            // 5.2 steps 3 and 4: split at the first '=', then trim WSP
            auto key   = nv.substr(0, eq).trim();
            auto value = nv.substr(eq + 1).trim();
            // 5.2 step 5
            if (key.empty()) continue;
            bool HttpOnly = false;
            bool Partitioned = false;
            bool Secure = false;
            char SameSite = 0;
            uint64_t expire = -1ULL;
            uint64_t max_age_expire = -1ULL;
            estring_view Path, Domain;
            while(!p.is_done()) {
                auto attr = Cookies | p.extract_until_char(';');
                // 5.2 unparsed-attributes step 4: no '=' means an empty value
                auto eq2 = attr.find('=');
                auto k2 = (eq2 == attr.npos) ? attr : attr.substr(0, eq2);
                auto v2 = (eq2 == attr.npos) ? estring_view() : attr.substr(eq2 + 1);
                // 5.2 unparsed-attributes step 5
                k2 = k2.trim();
                v2 = v2.trim();
                if (k2.icmp("Max-Age") == 0) {
                    int64_t age = 0;
                    // 5.2.2 steps 1 and 2: the value is exactly an optional '-' plus digits
                    if (v2.empty() || v2.to_int64_check(&age) != v2.size()) {
                        LOG_DEBUG("invalid integer format for 'Max-Age': ", v2);
                    } else {
                        // 5.2.2 step 4: not positive means the earliest representable date
                        max_age_expire = (age <= 0) ? 0 : sat_add(now, age);
                    }
                } else if (k2.icmp("Expires") == 0) {
                    uint64_t t;
                    if (!parse_cookie_date(v2, &t)) {
                        LOG_DEBUG("invalid time format for 'Expires': ", v2);
                    } else {
                        expire = t;
                    }
                }
                else if (k2.icmp("SameSite") == 0 && v2.size() > 0) SameSite = v2[0];
                else if (k2.icmp("Path") == 0) Path = v2;
                else if (k2.icmp("Domain") == 0) Domain = v2;
                else if (k2.icmp("Secure") == 0) Secure = true;
                else if (k2.icmp("HttpOnly") == 0) HttpOnly = true;
                else if (k2.icmp("Partitioned") == 0) Partitioned = true;
                else LOG_DEBUG("unknown cookie attribute ", attr);
            }
            // 5.3 step 3: Max-Age wins over Expires no matter which comes first,
            // so it is applied after the loop rather than where it is seen
            if (max_age_expire != -1ULL) expire = max_age_expire;
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