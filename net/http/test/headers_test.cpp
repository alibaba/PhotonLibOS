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
#include "../client.cpp"
#include "../headers.cpp"
#undef protected
#undef private

#include <fcntl.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <gflags/gflags.h>

#include "../../curl.h"
#include "../../socket.h"
#include "../../base_socket.h"
#include <photon/common/alog-stdstring.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/common/stream.h>

using namespace std;
using namespace photon;
using namespace photon::net;
using namespace photon::net::http;

template<uint16_t BUF_CAPACITY = 64*1024 - 1>
class RequestHeadersStored : public Request
{
public:
    RequestHeadersStored(Verb v, std::string_view url, bool enable_proxy = false) :
        Request(_buffer, BUF_CAPACITY, v, url, enable_proxy) { }

protected:
    char _buffer[BUF_CAPACITY];
};
TEST(headers, req_header) {
    char std_req_stream[] = "GET /targetName HTTP/1.1\r\n"
                             "Host: HostName\r\n"
                             "Content-Length: 0\r\n\r\n";
    RequestHeadersStored<> req(Verb::GET, "http://HostName:80/targetName");
    req.headers.content_length(0);
    EXPECT_EQ(false, req.headers.empty());
    EXPECT_EQ(Verb::GET, req.verb());
    EXPECT_EQ(true, "/targetName" == req.target());
    LOG_DEBUG(VALUE(req.target()));
    EXPECT_EQ(true, req.headers["Content-Length"] == "0");
    LOG_DEBUG(req.headers["Content-Length"]);
    EXPECT_EQ(true, req.headers["Host"] == "HostName");
    EXPECT_EQ(true, req.headers.find("noexist") == req.headers.end());
    LOG_DEBUG(req.headers["Host"]);
    string capacity_overflow;
    capacity_overflow.resize(100000);
    auto ret = req.headers.insert("overflow_test", capacity_overflow);
    EXPECT_EQ(-1, ret);
    RequestHeadersStored<> req_proxy(Verb::GET, "http://HostName:80/targetName", true);
    LOG_DEBUG(VALUE(req_proxy.target()));
    EXPECT_EQ(true, req_proxy.target() == "http://HostName/targetName");
}

class test_stream : public net::SocketStreamBase {
public:
    string rand_stream;
    size_t remain;
    char* ptr;
    int kv_count;
    test_stream(int kv_count) : kv_count(kv_count) {
        rand_stream = "HTTP/1.1 200 ok\r\n";
        for (auto i = 0; i < kv_count; i++) rand_stream += "key" + to_string(i) + ": value" + to_string(i) + "\r\n";
        rand_stream += "\r\n0123456789";
        ptr = (char*)rand_stream.data();
        remain = rand_stream.size();
    }
    virtual ssize_t recv(void *buf, size_t count, int flags = 0) override {
        // assert(count > remain);
        // LOG_DEBUG(remain);
        if (remain > 200) {
            auto len = rand() % 100 + 1;
            // cout << string(ptr, len);
            memcpy(buf, ptr, len);
            ptr += len;
            remain -= len;
            return len;
        }
        // cout << string(ptr, remain);
        memcpy(buf, ptr, remain);
        ptr += remain;
        auto ret = remain;
        remain = 0;
        return ret;
    }
    virtual ssize_t recv(const struct iovec *iov, int iovcnt, int flags = 0) override {
        ssize_t ret = 0;
        auto iovec = IOVector(iov, iovcnt);
        while (!iovec.empty()) {
            auto tmp = recv(iovec.front().iov_base, iovec.front().iov_len);
            if (tmp < 0) return tmp;
            if (tmp == 0) break;
            iovec.extract_front(tmp);
            ret += tmp;
        }
        return ret;
    }
    bool done() {
        return remain == 0;
    }
    int get_kv_count() {
        return kv_count;
    }

    void reset() {
        ptr = (char*)rand_stream.data();
        remain = rand_stream.size();
    }
};

TEST(headers, resp_header) {
    char of_buf[128 * 1024 - 1];
    Response of_header(of_buf, sizeof(of_buf));
    string of_stream = "HTTP/1.1 123 status_message\r\n";
    for (auto i = 0; i < 10; i++) of_stream += "key" + to_string(i) + ": value" + to_string(i) + "\r\n";
    of_stream += "\r\n0123456789";
    memcpy(of_buf, of_stream.data(), of_stream.size());
    auto ret = of_header.append_bytes(of_stream.size());
    EXPECT_EQ(0, ret);
    ret = of_header.append_bytes(of_stream.size());
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(true, of_header.version() == "1.1");
    EXPECT_EQ(123, of_header.status_code());
    EXPECT_EQ(true, of_header.status_message() == "status_message");
    EXPECT_EQ(true, of_header.partial_body() == "0123456789");
    of_header.reset(of_buf, sizeof(of_buf));
    ret = of_header.append_bytes(of_stream.size());
    EXPECT_EQ(0, ret);

    char rand_buf[64 * 1024 - 1];
    Response rand_header(rand_buf, sizeof(rand_buf));
    srand(time(0));
    test_stream stream(2000);
    do {
        auto ret = rand_header.receive_bytes(&stream);
        if (stream.done()) EXPECT_EQ(0, ret); else
            EXPECT_EQ(2, ret);
    } while (!stream.done());
    EXPECT_EQ(true, rand_header.version() == "1.1");
    EXPECT_EQ(200, rand_header.status_code());
    EXPECT_EQ(true, rand_header.status_message() == "ok");
    EXPECT_EQ(true, rand_header.partial_body() == "0123456789");
    auto kv_count = stream.get_kv_count();
    for (int i = 0; i < kv_count; i++) {
        string key = "key" + to_string(i);
        string value = "value" + to_string(i);
        EXPECT_EQ(true, rand_header.headers[key] == value);
    }

    char exceed_buf[64 * 1024 - 1];
    Response exceed_header(exceed_buf, sizeof(exceed_buf));
    srand(time(0));
    test_stream exceed_stream(3000);
    do {
        auto ret = exceed_header.receive_bytes(&exceed_stream);
        if (exceed_stream.done()) EXPECT_EQ(-1, ret); else
            EXPECT_EQ(2, ret);
    } while (!exceed_stream.done());
}
TEST(headers, url) {
    RequestHeadersStored<> headers(Verb::UNKNOWN, "https://domain.com:8888/dir1/dir2/file?key1=value1&key2=value2");
    EXPECT_EQ(true, headers.target() =="/dir1/dir2/file?key1=value1&key2=value2");
    EXPECT_EQ(true, headers.host() == "domain.com:8888");
    EXPECT_EQ(headers.port(), 8888);
    EXPECT_EQ(true, headers.host_no_port() == "domain.com");
    EXPECT_EQ(headers.secure(), 1);
    EXPECT_EQ(true, headers.query() == "key1=value1&key2=value2");
    RequestHeadersStored<> new_headers(Verb::UNKNOWN, "");
    if (headers.secure())
        new_headers.headers.insert("Referer", http_url_scheme);
    else
        new_headers.headers.insert("Referer", https_url_scheme);
    new_headers.headers.value_append(headers.host());
    new_headers.headers.value_append(headers.target());
    auto Referer_value = new_headers.headers["Referer"];
    LOG_DEBUG(VALUE(Referer_value));
    EXPECT_EQ(true, Referer_value == "http://domain.com:8888/dir1/dir2/file?key1=value1&key2=value2");
}

TEST(ReqHeaders, redirect) {
    RequestHeadersStored<> req(Verb::PUT, "http://domain1.com:1234/target1?param1=x1");
    req.headers.content_length(0);
    req.headers.insert("test_key", "test_value");
    req.redirect(Verb::GET, "https://domain2asjdhuyjabdhcuyzcbvjankdjcniaxnkcnkn.com:4321/target2?param2=x2");
    LOG_DEBUG(VALUE(req.query()));
    LOG_DEBUG(VALUE(req.port()));
    EXPECT_EQ(4321, req.port());
    EXPECT_EQ(true, req.headers["Host"] == "domain2asjdhuyjabdhcuyzcbvjankdjcniaxnkcnkn.com:4321");
    EXPECT_EQ(true, req.headers["test_key"] == "test_value");
    auto value = req.headers["Host"];
    LOG_DEBUG(VALUE(value));
    req.redirect(Verb::DELETE, "https://domain.redirect1/targetName", true);
    EXPECT_EQ(true, req.target() == "https://domain.redirect1/targetName");
    EXPECT_EQ(true, req.headers["Host"] == "domain.redirect1");
    LOG_DEBUG(VALUE(req.target()));
    req.redirect(Verb::GET, "/redirect_test", true);
    EXPECT_EQ(true, req.target() == "https://domain.redirect1/redirect_test");
    EXPECT_EQ(true, req.headers["Host"] == "domain.redirect1");
    LOG_DEBUG(VALUE(req.target()));
    req.redirect(Verb::GET, "/redirect_test1", false);
    EXPECT_EQ(true, req.target() == "/redirect_test1");
    EXPECT_EQ(true, req.headers["Host"] == "domain.redirect1");
    LOG_DEBUG(VALUE(req.target()));
}
TEST(debug, debug) {
    RequestHeadersStored<> req(Verb::PUT, "http://domain2asjdhuyjabdhcuyzcbvjankdjcniaxnkcnkn.com:80/target1?param1=x1");
    req.headers.content_length(0);
    req.headers.insert("test_key", "test_value");
    req.redirect(Verb::GET, "https://domain.com:442/target2?param2=x2", true);
}

int main(int argc, char** arg) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
#ifdef __linux
    if (net::et_poller_init() < 0) {
        LOG_ERROR("net::et_poller_init failed");
        exit(EAGAIN);
    }
    DEFER(net::et_poller_fini());
#endif
    set_log_output_level(ALOG_DEBUG);
    ::testing::InitGoogleTest(&argc, arg);
    LOG_DEBUG("test result:`", RUN_ALL_TESTS());
    return 0;
}
