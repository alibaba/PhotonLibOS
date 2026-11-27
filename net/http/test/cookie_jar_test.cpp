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

#define protected public
#define private public
#include "../cookie_jar.cpp"
#undef protected
#undef private

#include <fcntl.h>
#include <time.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <gflags/gflags.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <iostream>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include "../../../test/gtest.h"

using namespace photon;
using namespace photon::net;
using namespace photon::net::http;
using namespace std;

class ResponseHeaderAdaptor : public Response {
public:
    char m_buf[1024 * 64 - 1];
    ResponseHeaderAdaptor() {
        reset(m_buf, sizeof(m_buf));
    }
};

ResponseHeaderAdaptor* new_resp(std::string text) {
    text = "HTTP/1.1 200 ok\r\nSet-Cookie: " + text + "\r\n\r\n";
    ResponseHeaderAdaptor *ret = new ResponseHeaderAdaptor();
    memcpy(ret->m_buf, text.data(), text.size());
    ret->append_bytes(text.size());
    return ret;
}

std::string get_cookie(SimpleCookieJar *jar, std::string host) {
    char buf[1024];
    Request tmp_headers(buf, 1024, Verb::UNKNOWN, "http://" + host + "/");
    jar->set_cookies_to_headers(&tmp_headers);
    auto cookie = tmp_headers.headers["Cookie"];
    LOG_DEBUG(VALUE(cookie));
    return string(cookie);
}

TEST(cookie_jar, basic) {
    SimpleCookieJar c;
    auto resp1 = new_resp("key=value; para1; para2");
    DEFER(delete resp1);
    c.get_cookies_from_headers("host1", resp1);
    auto s = get_cookie(&c, "host1");
    LOG_INFO(VALUE(s));
    EXPECT_EQ(true, s == "key=value");
    auto resp2 = new_resp("__Host-key1=value1; para1; para2; Secure");
    DEFER(delete resp2);
    c.get_cookies_from_headers("host1", resp2);
    s = get_cookie(&c, "host1");
    estring_view esv(s);
    EXPECT_EQ(true, esv.find("__Host-key1=value1") != string_view::npos);
    EXPECT_EQ(true, esv.find("key=value") != string_view::npos);
    auto resp3 = new_resp("__Host-key1=value1_new; para1; para2; Secure; Expires=Wed, 24 Nov 3040 09:55:55");
    DEFER(delete resp3);
    auto resp4 = new_resp("key1=value1_host2");
    DEFER(delete resp4);
    c.get_cookies_from_headers("host1", resp3);
    c.get_cookies_from_headers("host2", resp4);
    s = get_cookie(&c, "host1");
    esv = estring_view(s);
    EXPECT_EQ(true, esv.find("__Host-key1=value1_new") != string_view::npos);
    EXPECT_EQ(true, esv.find("key=value") != string_view::npos);
    EXPECT_EQ(true, esv.find("key1=value1_host2") == string_view::npos);
    auto resp5 = new_resp("__Host-key1=value1_expired; para1; para2; Secure; Expires=Wed, 24 Nov 2020 09:55:55");
    DEFER(delete resp5);
    c.get_cookies_from_headers("host1", resp5);
    s = get_cookie(&c, "host1");
    LOG_DEBUG(VALUE(s));
    EXPECT_EQ(true, s == "key=value");
}

// Sends `set_cookie` to a jar that already holds "sid=abc; Path=/", unless
// `fresh`, and returns the Cookie header the jar would then produce.
static std::string jar_sends(const char* set_cookie, bool fresh) {
    SimpleCookieJar jar;
    if (!fresh) {
        std::unique_ptr<ResponseHeaderAdaptor> r(new_resp("sid=abc; Path=/"));
        jar.get_cookies_from_headers("host", r.get());
    }
    std::unique_ptr<ResponseHeaderAdaptor> r(new_resp(set_cookie));
    jar.get_cookies_from_headers("host", r.get());
    return get_cookie(&jar, "host");
}

struct Row { const char* set_cookie; const char* expect; bool fresh; const char* clause; };

static void check_rows(const Row* rows, size_t count) {
    for (size_t i = 0; i < count; i++) {
        auto& r = rows[i];
        EXPECT_EQ(std::string(r.expect), jar_sends(r.set_cookie, r.fresh))
                << "row " << i << " [" << r.clause << "]: " << r.set_cookie;
    }
}
#define CHECK_ROWS(rows) check_rows(rows, sizeof(rows) / sizeof(rows[0]))

TEST(cookie_jar, server_deletes_cookie) {
    static const Row rows[] = {
        {"sid=deleted; Max-Age=0", "", false, "5.2.2 step 4"},
        {"sid=deleted; Max-Age=-1", "", false, "5.2.2 step 4"},
        {"sid=; Path=/; Max-Age=0", "", false, "5.2.2 step 4, Go MaxAge:-1"},
        {"sid=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT", "", false, "Express clearCookie"},
        {"sid=\"\"; expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=0; Path=/", "", false, "Django delete_cookie"},
        // the two below delete on their own attribute, not on a Max-Age in the same header
        {"sid=deleted; expires=Thu, 01 Jan 1970 00:00:00 GMT", "", false, "5.2.1 case-insensitive"},
        {"sid=; Path=/; expires=Thu, 01-Jan-1970 00:00:00 GMT", "", false, "5.1.1 dashed date"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, max_age_is_strict_about_digits) {
    static const Row rows[] = {
        {"sid=x; Max-Age=0",    "",      false, "5.2.2 step 4: zero"},
        {"sid=x; Max-Age=-1",   "",      false, "5.2.2 step 4: negative"},
        {"sid=x; Max-Age=60",   "sid=x", false, "5.2.2 step 4: still alive"},
        {"sid=x; Max-Age= 0",   "",      false, "attribute step 5 trims WSP"},
        {"sid=x; Max-Age=0 ",   "",      false, "attribute step 5 trims WSP"},
        {"sid=x;  Max-Age=0",   "",      false, "attribute step 5 trims repeated WSP"},
        {"sid=x; Max-Age=0abc", "sid=x", false, "5.2.2 step 2: non-DIGIT in the remainder"},
        {"sid=x; Max-Age=1 2",  "sid=x", false, "5.2.2 step 2"},
        {"sid=x; Max-Age=abc",  "sid=x", false, "5.2.2 step 1: first character"},
        {"sid=x; Max-Age=x0",   "sid=x", false, "5.2.2 step 1"},
        {"sid=x; Max-Age=+1",   "sid=x", false, "5.2.2 step 1: '+' is neither DIGIT nor '-'"},
        {"sid=x; Max-Age=--1",  "sid=x", false, "5.2.2 step 2"},
        {"sid=x; Max-Age=",     "sid=x", false, "5.2.2 step 1: empty value"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, max_age_takes_precedence_over_expires) {
    static const Row rows[] = {
        {"sid=x; Max-Age=0; Expires=Wed, 24 Nov 3040 09:55:55 GMT", "", false, "5.3 step 3"},
        {"sid=x; Expires=Wed, 24 Nov 3040 09:55:55 GMT; Max-Age=0", "", false, "5.3 step 3"},
        {"sid=x; Max-Age=100; Expires=Thu, 01 Jan 1970 00:00:00 GMT", "sid=x", false, "5.3 step 3"},
        {"sid=x; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=100", "sid=x", false, "5.3 step 3"},
        {"sid=x; Max-Age=0; Max-Age=100", "sid=x", false, "5.3 step 3: last Max-Age wins"},
        {"sid=x; Max-Age=100; Max-Age=0", "", false, "5.3 step 3: last Max-Age wins"},
        {"sid=x; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Expires=Wed, 24 Nov 3040 09:55:55 GMT",
         "sid=x", false, "5.3 step 3: last Expires wins"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, attribute_names_are_case_insensitive) {
    static const Row rows[] = {
        {"sid=x; max-age=0", "", false, "5.2.2"},
        {"sid=x; MAX-AGE=0", "", false, "5.2.2"},
        {"sid=x; MaX-aGe=0", "", false, "5.2.2"},
        {"sid=x; expires=Thu, 01 Jan 1970 00:00:00 GMT", "", false, "5.2.1"},
        {"sid=x; EXPIRES=Thu, 01 Jan 1970 00:00:00 GMT", "", false, "5.2.1"},
        // Path and Domain are observable through the filters in set_cookies_to_headers:
        // an unrecognised attribute leaves them empty, which matches every request.
        {"sid=x; path=/nomatch", "", false, "5.2.4"},
        {"sid=x; PATH=/nomatch", "", false, "5.2.4"},
        {"sid=x; domain=other", "", false, "5.2.3"},
        {"sid=x; DOMAIN=other", "", false, "5.2.3"},
        {"sid=x; path=/", "sid=x", false, "5.2.4: control, the path does match"},
        {"sid=x; domain=host", "sid=x", false, "5.2.3: control, the domain does match"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, name_value_pair_is_the_first_segment) {
    static const Row rows[] = {
        {"sid; Path=/", "sid=abc", false, "5.2 step 2: ignore the set-cookie-string entirely"},
        {"sid", "sid=abc", false, "5.2 step 2"},
        {"=v", "sid=abc", false, "5.2 step 5: empty name"},
        {"  =v", "sid=abc", false, "5.2 steps 4 and 5"},
        {"sid; Path=/nomatch", "sid=abc", false, "5.2 step 2: no cookie is created at all"},
        {"sid", "", true, "5.2 step 2"},
        {"=v", "", true, "5.2 step 5"},
        {"sid=x", "sid=x", false, "control: a well-formed pair does replace"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, name_and_value_whitespace_is_trimmed) {
    static const Row rows[] = {
        {"sid =x", "sid=x", true, "5.2 step 4: name"},
        {"sid= x", "sid=x", true, "5.2 step 4: value"},
        {"sid = x ", "sid=x", true, "5.2 step 4: both"},
        {"sid=x\t", "sid=x", true, "5.2 step 4: HTAB is WSP"},
        {"sid =x; Path=/", "sid=x", true, "5.2 step 4: the name keys the store"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, empty_value_is_a_valid_cookie) {
    static const Row rows[] = {
        {"sid=", "sid=", true, "5.2 step 3: the value may be empty"},
        {"sid=; Path=/", "sid=", true, "5.2 step 3"},
        {"sid=; Max-Age=60", "sid=", true, "5.2 step 3"},
        {"sid=", "sid=", false, "5.2 step 3: an empty value replaces a non-empty one"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, cookie_date_formats) {
    static const Row rows[] = {
        {"sid=x; Expires=Thu, 01 Jan 1970 00:00:00 GMT", "", false, "rfc1123-date"},
        {"sid=x; Expires=Thu, 01-Jan-1970 00:00:00 GMT", "", false, "5.1.1: '-' is a delimiter"},
        {"sid=x; Expires=Thu Jan  1 00:00:00 1970", "", false, "5.1.1: asctime form"},
        {"sid=x; Expires=Tue, 06-Jan-70 00:00:00 GMT", "", false, "5.1.1 step 3: 70 becomes 1970"},
        {"sid=x; Expires=Sat, 06-Jan-69 00:00:00 GMT", "sid=x", false, "5.1.1 step 4: 69 becomes 2069, which is in the future"},
        {"sid=x; Expires=01 Jan 1970 00:00:00 GMT", "", false, "5.1.1: the weekday is just a token"},
        {"sid=x; Expires=Thu, 1 Jan 1970 0:0:0 GMT", "", false, "5.1.1: time-field is 1*2DIGIT"},
        {"sid=x; Expires=Sun, 06 Nov 1994 08:49:37 GMT", "", false, "the RFC's own example"},
        {"sid=x; Expires=Mon, 01 Jan 1900 00:00:00 GMT", "", false, "5.2.1 MAY clamp a pre-epoch date"},
        {"sid=x; Expires=Thu, 29 Feb 2024 00:00:00 GMT", "", false, "5.1.1 step 6: that leap day exists"},
        {"sid=x; Expires=Wed, 24 Nov 3040 09:55:55 GMT", "sid=x", false, "a future date is kept"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, cookie_date_rejects_invalid_values) {
    static const Row rows[] = {
        {"sid=x; Expires=Mon, 01 Jan 1500 00:00:00 GMT", "sid=x", false, "5.1.1 step 5: year < 1601"},
        {"sid=x; Expires=Sat, 30 Feb 1970 00:00:00 GMT", "sid=x", false, "5.1.1 step 6: no Feb 30"},
        {"sid=x; Expires=Sat, 29 Feb 1900 00:00:00 GMT", "sid=x", false, "5.1.1 step 6: 1900 is not a leap year"},
        {"sid=x; Expires=Thu, 32 Jan 1970 00:00:00 GMT", "sid=x", false, "5.1.1 step 5: day > 31"},
        {"sid=x; Expires=Thu, 00 Jan 1970 00:00:00 GMT", "sid=x", false, "5.1.1 step 5: day < 1"},
        {"sid=x; Expires=Thu, 01 Jan 1970 24:00:00 GMT", "sid=x", false, "5.1.1 step 5: hour > 23"},
        {"sid=x; Expires=Thu, 01 Jan 1970 00:60:00 GMT", "sid=x", false, "5.1.1 step 5: minute > 59"},
        {"sid=x; Expires=Thu, 01 Jan 1970 00:00:60 GMT", "sid=x", false, "5.1.1 step 5: second > 59"},
        {"sid=x; Expires=Thu, 01 Jan 19700 00:00:00 GMT", "sid=x", false, "5.1.1: year is 2*4DIGIT"},
        {"sid=x; Expires=Thu, 01 Jan 0 00:00:00 GMT", "sid=x", false, "5.1.1: year needs at least 2 digits"},
        {"sid=x; Expires=Thu, 01 Jan 00000 00:00:00 GMT", "sid=x", false, "5.1.1: year is at most 4 digits, leading zeros do not shorten it"},
        {"sid=x; Expires=Thu, 012 Jan 1970 00:00:00 GMT", "sid=x", false, "5.1.1: day-of-month is at most 2 digits"},
        {"sid=x; Expires=Thu, 01 Jan 1970 00x00x00 GMT", "sid=x", false, "5.1.1: hms-time needs ':' separators"},
        {"sid=x; Expires=Thu, 01 Jan 1970 00:00:000 GMT", "sid=x", false, "5.1.1: time is hms-time then a non-digit"},
        {"sid=x; Expires=Thu, 01 Jan 1970 GMT", "sid=x", false, "5.1.1 step 5: found-time is not set"},
        {"sid=x; Expires=Thu, 01 1970 00:00:00 GMT", "sid=x", false, "5.1.1 step 5: found-month is not set"},
        {"sid=x; Expires=Thu, 01 Jan 00:00:00 GMT", "sid=x", false, "5.1.1 step 5: found-year is not set"},
        {"sid=x; Expires=not a date", "sid=x", false, "5.1.1 step 5"},
        {"sid=x; Expires=", "sid=x", false, "5.2.1: failed to parse, ignore the cookie-av"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, max_age_extreme_values) {
    static const Row rows[] = {
        // (uint64_t)time(0) + (uint64_t)age keeps this well defined; the signed
        // form overflowed. The cookie stays, which is the only sane reading of
        // "the current date and time plus delta-seconds" at this magnitude.
        {"sid=x; Max-Age=9223372036854775807", "sid=x", false, "5.2.2 step 4"},
        {"sid=x; Max-Age=-9223372036854775808", "", false, "5.2.2 step 4: <= 0"},
    };
    CHECK_ROWS(rows);
}

TEST(cookie_jar, expires_is_gmt) {
    // Expires is a UTC date (5.1.1 step 6); it must not depend on the local zone.
    const char* old = getenv("TZ");
    std::string saved = old ? old : "";
    setenv("TZ", "Asia/Shanghai", 1);
    tzset();
    DEFER({ if (old) setenv("TZ", saved.c_str(), 1); else unsetenv("TZ"); tzset(); });
    char date[64];
    struct tm g;
    for (auto ahead : {(time_t)3600, (time_t)-3600}) {
        time_t when = time(0) + ahead;
        gmtime_r(&when, &g);
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &g);
        std::string text = estring().appends("sid=abc; Expires=", estring_view(date));
        std::unique_ptr<ResponseHeaderAdaptor> set(new_resp(text));
        SimpleCookieJar jar;
        jar.get_cookies_from_headers("host", set.get());
        EXPECT_EQ(ahead > 0 ? "sid=abc" : "", get_cookie(&jar, "host")) << date;
    }
}

int main(int argc, char** arg) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    set_log_output_level(ALOG_DEBUG);
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
