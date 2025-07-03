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

#include <gtest/gtest.h>
#define protected public
#define private public
#include "../client.cpp"
#include "../headers.cpp"
#undef protected
#undef private

#include <fcntl.h>
#include "../../../test/gtest.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <gflags/gflags.h>

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
    // char std_req_stream[] = "GET /targetName HTTP/1.1\r\n"
    //                          "Host: HostName\r\n"
    //                          "Content-Length: 0\r\n\r\n";
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
    char of_buf[64 * 1024 - 1];
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

TEST(status, status) {
    EXPECT_STREQ(photon::net::http::obsolete_reason(100).data(), "Continue");
    EXPECT_STREQ(photon::net::http::obsolete_reason(101).data(), "Switching Protocols");
    EXPECT_STREQ(photon::net::http::obsolete_reason(102).data(), "Processing");
    EXPECT_STREQ(photon::net::http::obsolete_reason(103).data(), "Early Hints");

    EXPECT_STREQ(photon::net::http::obsolete_reason(200).data(), "OK");
    EXPECT_STREQ(photon::net::http::obsolete_reason(201).data(), "Created");
    EXPECT_STREQ(photon::net::http::obsolete_reason(202).data(), "Accepted");
    EXPECT_STREQ(photon::net::http::obsolete_reason(203).data(), "Non-Authoritative Information");
    EXPECT_STREQ(photon::net::http::obsolete_reason(204).data(), "No Content");
    EXPECT_STREQ(photon::net::http::obsolete_reason(205).data(), "Reset Content");
    EXPECT_STREQ(photon::net::http::obsolete_reason(206).data(), "Partial Content");
    EXPECT_STREQ(photon::net::http::obsolete_reason(207).data(), "Multi-Status");
    EXPECT_STREQ(photon::net::http::obsolete_reason(208).data(), "Already Reported");

    EXPECT_STREQ(photon::net::http::obsolete_reason(300).data(), "Multiple Choices");
    EXPECT_STREQ(photon::net::http::obsolete_reason(301).data(), "Moved Permanently");
    EXPECT_STREQ(photon::net::http::obsolete_reason(302).data(), "Found");
    EXPECT_STREQ(photon::net::http::obsolete_reason(303).data(), "See Other");
    EXPECT_STREQ(photon::net::http::obsolete_reason(304).data(), "Not Modified");
    EXPECT_STREQ(photon::net::http::obsolete_reason(305).data(), "Use Proxy");
    EXPECT_STREQ(photon::net::http::obsolete_reason(306).data(), "");
    EXPECT_STREQ(photon::net::http::obsolete_reason(307).data(), "Temporary Redirect");
    EXPECT_STREQ(photon::net::http::obsolete_reason(308).data(), "Permanent Redirect");

    EXPECT_STREQ(photon::net::http::obsolete_reason(400).data(), "Bad Request");
    EXPECT_STREQ(photon::net::http::obsolete_reason(401).data(), "Unauthorized");
    EXPECT_STREQ(photon::net::http::obsolete_reason(402).data(), "Payment Required");
    EXPECT_STREQ(photon::net::http::obsolete_reason(403).data(), "Forbidden");
    EXPECT_STREQ(photon::net::http::obsolete_reason(404).data(), "Not Found");
    EXPECT_STREQ(photon::net::http::obsolete_reason(405).data(), "Method Not Allowed");
    EXPECT_STREQ(photon::net::http::obsolete_reason(406).data(), "Not Acceptable");
    EXPECT_STREQ(photon::net::http::obsolete_reason(407).data(), "Proxy Authentication Required");
    EXPECT_STREQ(photon::net::http::obsolete_reason(408).data(), "Request Timeout");
    EXPECT_STREQ(photon::net::http::obsolete_reason(409).data(), "Conflict");
    EXPECT_STREQ(photon::net::http::obsolete_reason(410).data(), "Gone");
    EXPECT_STREQ(photon::net::http::obsolete_reason(411).data(), "Length Required");
    EXPECT_STREQ(photon::net::http::obsolete_reason(412).data(), "Precondition Failed");
    EXPECT_STREQ(photon::net::http::obsolete_reason(413).data(), "Content Too Large");
    EXPECT_STREQ(photon::net::http::obsolete_reason(414).data(), "URI Too Long");
    EXPECT_STREQ(photon::net::http::obsolete_reason(415).data(), "Unsupported Media Type");
    EXPECT_STREQ(photon::net::http::obsolete_reason(416).data(), "Range Not Satisfiable");
    EXPECT_STREQ(photon::net::http::obsolete_reason(417).data(), "Expectation Failed");
    EXPECT_STREQ(photon::net::http::obsolete_reason(418).data(), "I'm a teapot");
    EXPECT_STREQ(photon::net::http::obsolete_reason(419).data(), "");
    EXPECT_STREQ(photon::net::http::obsolete_reason(420).data(), "");
    EXPECT_STREQ(photon::net::http::obsolete_reason(421).data(), "Misdirected Request");
    EXPECT_STREQ(photon::net::http::obsolete_reason(422).data(), "Unprocessable Content");
    EXPECT_STREQ(photon::net::http::obsolete_reason(423).data(), "Locked");
    EXPECT_STREQ(photon::net::http::obsolete_reason(424).data(), "Failed Dependency");
    EXPECT_STREQ(photon::net::http::obsolete_reason(425).data(), "Too Early");
    EXPECT_STREQ(photon::net::http::obsolete_reason(426).data(), "Upgrade Required");
    EXPECT_STREQ(photon::net::http::obsolete_reason(427).data(), "");
    EXPECT_STREQ(photon::net::http::obsolete_reason(428).data(), "Precondition Required");
    EXPECT_STREQ(photon::net::http::obsolete_reason(429).data(), "Too Many Requests");
    EXPECT_STREQ(photon::net::http::obsolete_reason(430).data(), "");
    EXPECT_STREQ(photon::net::http::obsolete_reason(431).data(), "Request Header Fields Too Large");

    EXPECT_STREQ(photon::net::http::obsolete_reason(500).data(), "Internal Server Error");
    EXPECT_STREQ(photon::net::http::obsolete_reason(501).data(), "Not Implemented");
    EXPECT_STREQ(photon::net::http::obsolete_reason(502).data(), "Bad Gateway");
    EXPECT_STREQ(photon::net::http::obsolete_reason(503).data(), "Service Unavailable");
    EXPECT_STREQ(photon::net::http::obsolete_reason(504).data(), "Gateway Timeout");
    EXPECT_STREQ(photon::net::http::obsolete_reason(505).data(), "HTTP Version Not Supported");
    EXPECT_STREQ(photon::net::http::obsolete_reason(506).data(), "Variant Also Negotiates");
    EXPECT_STREQ(photon::net::http::obsolete_reason(507).data(), "Insufficient Storage");
    EXPECT_STREQ(photon::net::http::obsolete_reason(508).data(), "Loop Detected");
    EXPECT_STREQ(photon::net::http::obsolete_reason(509).data(), "");
    EXPECT_STREQ(photon::net::http::obsolete_reason(510).data(), "Not Extended");
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
    return RUN_ALL_TESTS();
}
