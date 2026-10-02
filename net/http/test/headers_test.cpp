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
#include <photon/common/memory-stream/memory-stream.h>
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
TEST(headers, send_without_extra_headers) {
    auto send = [](const HeadersBase* extra) {
        RequestHeadersStored<> req(Verb::GET, "http://example.com/");
        req.headers.content_length(0);
        std::unique_ptr<StringSocketStream> stream(new_string_socket_stream());
        EXPECT_EQ(0, req.send_header(stream.get(), extra));
        EXPECT_NE(std::string::npos, stream->output().find("\r\n\r\n"));
    };

    send(nullptr);
    CommonHeaders<64> empty;
    send(&empty);
}

TEST(headers, req_header) {
    // char std_req_stream[] = "GET /targetName HTTP/1.1\r\n"
    //                          "Host: HostName\r\n"
    //                          "Content-Length: 0\r\n\r\n";
    RequestHeadersStored<> req(Verb::GET, "http://HostName:80/targetName");
    req.headers.content_length(0);
    EXPECT_EQ(false, req.headers.empty());
    EXPECT_EQ(Verb::GET, req.verb());
    EXPECT_EQ("/targetName", req.target());
    LOG_DEBUG(VALUE(req.target()));
    EXPECT_EQ(req.headers["Content-Length"], "0");
    LOG_DEBUG(req.headers["Content-Length"]);
    EXPECT_EQ(req.headers["Host"], "HostName");
    EXPECT_EQ(req.headers.find("noexist"), req.headers.end());
    ASSERT_EQ(0, req.headers.insert("Proxy-Authorization", "secret"));
    ASSERT_EQ(0, req.headers.insert("X-After", "retained"));
    ASSERT_EQ(1, req.headers.erase("Proxy-Authorization"));
    EXPECT_TRUE(req.headers["Proxy-Authorization"].empty());
    EXPECT_EQ(req.headers["Host"], "HostName");
    EXPECT_EQ(req.headers["Content-Length"], "0");
    EXPECT_EQ(req.headers["X-After"], "retained");
    LOG_DEBUG(req.headers["Host"]);
    string capacity_overflow;
    capacity_overflow.resize(100000);
    auto ret = req.headers.insert("overflow_test", capacity_overflow);
    EXPECT_EQ(-1, ret);
    RequestHeadersStored<> req_proxy(Verb::GET, "http://HostName:80/targetName", true);
    LOG_DEBUG(VALUE(req_proxy.target()));
    EXPECT_EQ(req_proxy.target(), "http://HostName/targetName");
}

TEST(headers, erase_preserves_index_offsets_and_serialization) {
    // Insertion order deliberately differs from the sorted index order.
    for (auto victim : {"A", "M", "Z"}) {
        CommonHeaders<256> headers;
        ASSERT_EQ(0, headers.insert("M", "middle"));
        ASSERT_EQ(0, headers.insert("Z", "last"));
        ASSERT_EQ(0, headers.insert("A", "first"));
        ASSERT_EQ(1, headers.erase(victim));
        std::string expected;
        for (auto item : {std::make_pair("M", "middle"),
                          std::make_pair("Z", "last"),
                          std::make_pair("A", "first")}) {
            if (std::string_view(item.first) == victim) continue;
            EXPECT_EQ(item.second, headers[item.first]);
            expected += std::string(item.first) + ": " + item.second + "\r\n";
        }
        EXPECT_EQ(expected, headers.serialized());
        ASSERT_EQ(0, headers.insert("B", "new"));
        EXPECT_EQ("new", headers["B"]);
        EXPECT_EQ(0, headers.erase("missing"));
    }
    CommonHeaders<256> headers;
    ASSERT_EQ(0, headers.insert("X-Dup", "one", 1));
    ASSERT_EQ(0, headers.insert("Keep", "value"));
    ASSERT_EQ(0, headers.insert("x-dup", "two", 1));
    ASSERT_EQ(2, headers.erase("X-DUP"));
    EXPECT_EQ("Keep: value\r\n", headers.serialized());
    ASSERT_EQ(1, headers.erase("Keep"));
    EXPECT_TRUE(headers.empty());
    EXPECT_TRUE(headers.serialized().empty());
    ASSERT_EQ(0, headers.insert("Again", "works"));
    EXPECT_EQ("Again: works\r\n", headers.serialized());
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
    EXPECT_EQ(of_header.version(), "1.1");
    EXPECT_EQ(of_header.status_code(), 123);
    EXPECT_EQ(of_header.status_message(), "status_message");
    EXPECT_EQ(of_header.partial_body(), "0123456789");
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
    EXPECT_EQ(rand_header.version(), "1.1");
    EXPECT_EQ(rand_header.status_code(), 200);
    EXPECT_EQ(rand_header.status_message(), "ok");
    EXPECT_EQ(rand_header.partial_body(), "0123456789");
    auto kv_count = stream.get_kv_count();
    for (int i = 0; i < kv_count; i++) {
        string key = "key" + to_string(i);
        string value = "value" + to_string(i);
        EXPECT_EQ(rand_header.headers[key], value);
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
    EXPECT_EQ(headers.target(), "/dir1/dir2/file?key1=value1&key2=value2");
    EXPECT_EQ(headers.host(), "domain.com:8888");
    EXPECT_EQ(headers.port(), 8888);
    EXPECT_EQ(headers.host_no_port(), "domain.com");
    EXPECT_EQ(headers.secure(), 1);
    EXPECT_EQ(headers.query(), "key1=value1&key2=value2");
    RequestHeadersStored<> new_headers(Verb::UNKNOWN, "");
    if (headers.secure())
        new_headers.headers.insert("Referer", http_url_scheme);
    else
        new_headers.headers.insert("Referer", https_url_scheme);
    new_headers.headers.value_append(headers.host());
    new_headers.headers.value_append(headers.target());
    auto Referer_value = new_headers.headers["Referer"];
    LOG_DEBUG(VALUE(Referer_value));
    EXPECT_EQ(Referer_value, "http://domain.com:8888/dir1/dir2/file?key1=value1&key2=value2");
}

TEST(ReqHeaders, redirect) {
    RequestHeadersStored<> req(Verb::PUT, "http://domain1.com:1234/target1?param1=x1");
    req.headers.content_length(0);
    req.headers.insert("test_key", "test_value");
    req.redirect(Verb::GET, "https://domain2asjdhuyjabdhcuyzcbvjankdjcniaxnkcnkn.com:4321/target2?param2=x2");
    LOG_DEBUG(VALUE(req.query()));
    LOG_DEBUG(VALUE(req.port()));
    EXPECT_EQ(4321, req.port());
    EXPECT_EQ(req.headers["Host"], "domain2asjdhuyjabdhcuyzcbvjankdjcniaxnkcnkn.com:4321");
    EXPECT_EQ(req.headers["test_key"], "test_value");
    auto value = req.headers["Host"];
    LOG_DEBUG(VALUE(value));
    // a plaintext origin is forwarded by the proxy, in absolute-URI form
    req.redirect(Verb::DELETE, "http://domain.redirect1/targetName", true);
    EXPECT_EQ(req.target(), "http://domain.redirect1/targetName");
    EXPECT_EQ(req.headers["Host"], "domain.redirect1");
    LOG_DEBUG(VALUE(req.target()));
    req.redirect(Verb::GET, "/redirect_test", true);
    EXPECT_EQ(req.target(), "http://domain.redirect1/redirect_test");
    EXPECT_EQ(req.headers["Host"], "domain.redirect1");
    LOG_DEBUG(VALUE(req.target()));
    // a TLS origin is reached through a CONNECT tunnel, so the request inside it
    // is in origin-form, just like a direct one
    req.redirect(Verb::GET, "https://domain.redirect2/targetName", true);
    EXPECT_EQ(req.target(), "/targetName");
    EXPECT_EQ(req.headers["Host"], "domain.redirect2");
    LOG_DEBUG(VALUE(req.target()));
    req.redirect(Verb::GET, "/redirect_test1", false);
    EXPECT_EQ(req.target(), "/redirect_test1");
    EXPECT_EQ(req.headers["Host"], "domain.redirect2");
    LOG_DEBUG(VALUE(req.target()));
}

// A CONNECT names its target in authority-form: it asks for a tunnel to a host,
// not for a resource, so it carries neither scheme nor path.
TEST(ReqHeaders, connect_is_in_authority_form) {
    RequestHeadersStored<> req(Verb::CONNECT, "https://origin:4321/ignored?q=1");
    EXPECT_EQ(req.target(), "origin:4321");
    EXPECT_EQ(req.headers["Host"], "origin:4321");
    EXPECT_EQ(req.query(), "");
    EXPECT_EQ(4321, req.port());

    RequestHeadersStored<> default_port(Verb::CONNECT, "https://origin:443/");
    EXPECT_EQ(default_port.target(), "origin:443");
    EXPECT_EQ(default_port.headers["Host"], "origin:443");

    char buf[128];
    Request bounded(buf, sizeof(buf));
    std::string long_host(256, 'a');
    auto url = estring().appends("https://", long_host, ":443/");
    EXPECT_EQ(-1, bounded.reset(Verb::CONNECT, url));
    EXPECT_EQ(ENOBUFS, errno);
}

TEST(ReqHeaders, connect_redirect_preserves_headers_and_explicit_port) {
    for (bool proxy : {false, true}) {
        RequestHeadersStored<> req(Verb::CONNECT, "https://origin:8080/");
        ASSERT_EQ(0, req.headers.insert("X-Preserved", "value"));
        std::string host(220, 'a');
        auto url = estring().appends("https://", host, "/?ignored=1");
        ASSERT_EQ(0, req.redirect(Verb::CONNECT, url, proxy));
        auto authority = estring().appends(host, ":443");
        EXPECT_EQ(authority, req.target());
        EXPECT_EQ(authority, req.headers["Host"]);
        EXPECT_EQ("value", req.headers["X-Preserved"]);
        EXPECT_TRUE(req.query().empty());
        EXPECT_EQ(443, req.port());
        std::unique_ptr<StringSocketStream> stream(new_string_socket_stream());
        ASSERT_EQ(0, req.send_header(stream.get()));
        EXPECT_EQ(0U, stream->output().find(estring().appends(
            "CONNECT ", authority, " HTTP/1.1\r\nHost: ", authority, "\r\n")));
    }
}

TEST(ReqHeaders, connect_redirect_rejects_insufficient_space_without_changes) {
    char buf[128];
    Request req(buf, sizeof(buf), Verb::CONNECT, "https://origin:8080/");
    ASSERT_EQ(0, req.headers.insert("X-Preserved", "value"));
    // The new request line fits alone, but not with its Host and other headers.
    auto url = estring().appends("https://", std::string(80, 'a'), "/");
    EXPECT_EQ(-1, req.redirect(Verb::CONNECT, url));
    EXPECT_EQ(ENOBUFS, errno);
    EXPECT_EQ("origin:8080", req.target());
    EXPECT_EQ("origin:8080", req.headers["Host"]);
    EXPECT_EQ("value", req.headers["X-Preserved"]);
    url = estring().appends("https://", std::string(256, 'a'), "/");
    EXPECT_EQ(-1, req.redirect(Verb::CONNECT, url, true));
    EXPECT_EQ(ENOBUFS, errno);
    EXPECT_EQ("origin:8080", req.target());
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
