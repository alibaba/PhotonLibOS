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
#include <gtest/gtest.h>
#include <time.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <gflags/gflags.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <iostream>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>

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
    text = "HTTP/1.1 200 ok\r\nSet-Cookies: " + text + "\r\n\r\n";
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
    return std::string(tmp_headers.headers["Cookie"]);
}

TEST(cookie_jar, basic) {
    SimpleCookieJar c;
    auto resp1 = new_resp("key=value; para1; para2");
    DEFER(delete resp1);
    c.get_cookies_from_headers("host1", resp1);
    auto s = get_cookie(&c, "host1");
    LOG_INFO(VALUE(s));
    EXPECT_EQ(true, s == "key=value");
    auto resp2 = new_resp("__Host-key1=value1; para1; para2");
    DEFER(delete resp2);
    c.get_cookies_from_headers("host1", resp2);
    s = get_cookie(&c, "host1");
    estring_view esv(s);
    EXPECT_EQ(true, esv.find("key1=value1") != string_view::npos);
    EXPECT_EQ(true, esv.find("key=value") != string_view::npos);
    auto resp3 = new_resp("__Host-key1=value1_new; para1; para2; Expires=Wed, 24 Nov 3040 09:55:55");
    DEFER(delete resp3);
    auto resp4 = new_resp("key1=value1_host2");
    DEFER(delete resp4);
    c.get_cookies_from_headers("host1", resp3);
    c.get_cookies_from_headers("host2", resp4);
    s = get_cookie(&c, "host1");
    esv = estring_view(s);
    EXPECT_EQ(true, esv.find("key1=value1_new") != string_view::npos);
    EXPECT_EQ(true, esv.find("key=value") != string_view::npos);
    EXPECT_EQ(true, esv.find("key1=value1_host2") == string_view::npos);
    auto resp5 = new_resp("__Host-key1=value1_expired; para1; para2; Expires=Wed, 24 Nov 2020 09:55:55");
    DEFER(delete resp5);
    c.get_cookies_from_headers("host1", resp5);
    s = get_cookie(&c, "host1");
    EXPECT_EQ(true, s == "key=value");
}

int main(int argc, char** arg) {
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    photon::fd_events_init();
    DEFER(photon::fd_events_fini());
    set_log_output_level(ALOG_DEBUG);
    ::testing::InitGoogleTest(&argc, arg);
    LOG_DEBUG("test result:`", RUN_ALL_TESTS());
    return 0;
}
