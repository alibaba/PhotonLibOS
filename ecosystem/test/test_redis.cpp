// The MIT License (MIT)
//
// Copyright (c) 2015-2017 Simon Ninon <simon.ninon@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "../redis.h"
#include <photon/photon.h>
#include <photon/common/estring.h>
#include <photon/common/alog-stdstring.h>
#include <photon/net/socket.h>
#include <gtest/gtest.h>
#include "../../test/ci-tools.h"
using namespace photon;
using namespace photon::net;
using namespace photon::redis;
using namespace std;

class FakeStream : public ISocketStream {
public:
    estring_view in;
    // estring_view in = "48293\r\n";
    // estring_view out_truth = "*2\r\n$4\r\nLLEN\r\n$6\r\nmylist\r\n";
    std::string output;
    virtual ssize_t recv(void *buf, size_t count, int flags = 0) override {
        size_t n = rand() % 8 + 1;
        if (n > in.size()) n = in.size();
        if (n > count) n = count;
        memcpy(buf, in.data(), n);
        in = in.substr(n);
        return n;
    }
    virtual ssize_t recv(const struct iovec *iov, int iovcnt, int flags = 0) override {
        while(!iov->iov_base || !iov->iov_len)
            if (iovcnt) { ++iov, --iovcnt; }
            else return -1;
        return recv(iov->iov_base, iov->iov_len);
    }
    virtual ssize_t send(const void *buf, size_t count, int flags = 0) override {
        size_t n = rand() % 8 + 1;
        if (n > count) n = count;
        auto p = (const char*)buf;
        output.append(p, p + n);
        return n;
    }
    virtual ssize_t send(const struct iovec *iov, int iovcnt, int flags = 0) override {
        while(!iov->iov_base || !iov->iov_len)
            if (iovcnt) { ++iov, --iovcnt; }
            else return -1;
        return send(iov->iov_base, iov->iov_len);
    }
    virtual ssize_t sendfile(int in_fd, off_t offset, size_t count) override { return 0; }
    virtual int close() override { return 0; }
    virtual ssize_t read(void *buf, size_t count) override {
        if (count > in.size()) count = in.size();
        memcpy(buf, in.data(), count);
        return count;
    }
    virtual ssize_t readv(const struct iovec *iov, int iovcnt) override {
        ssize_t s = 0;
        for (int i = 0; i < iovcnt; ++i) {
            ssize_t ret = read(iov[i].iov_base, iov[i].iov_len);
            if (ret < 0) return ret;
            s += ret;
            if (ret < (ssize_t)iov[i].iov_len) break;
        }
        return s;
    }
    virtual ssize_t write(const void *buf, size_t count) override {
        auto p = (const char*)buf;
        output.append(p, p + count);
        return count;
    }
    virtual ssize_t writev(const struct iovec *iov, int iovcnt) override {
        ssize_t s = 0;
        for (int i = 0; i < iovcnt; ++i) {
            ssize_t ret = write(iov[i].iov_base, iov[i].iov_len);
            if (ret < 0) return ret;
            s += ret;
            if (ret < (ssize_t)iov[i].iov_len) break;
        }
        return s;
    }
    virtual Object* get_underlay_object(uint64_t recursion = 0) override { return 0; }
    virtual int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override { return 0; }
    virtual int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override { return 0; }
    virtual int getsockname(EndPoint& addr) override { return 0; }
    virtual int getpeername(EndPoint& addr) override { return 0; }
    virtual int getsockname(char* path, size_t count) override { return 0; }
    virtual int getpeername(char* path, size_t count) override { return 0; }
};


#define SSTR(s)     "+" #s CRLF
#define BSTR(n, s)  "$" #n CRLF #s CRLF
#define INTEGER(n)  ":" #n CRLF
#define ARRAY_HEADER(n) "*" #n CRLF

struct __BS : public _BufferedStream<> {
public:
    using _BufferedStream::_strint;
};

TEST(redis, serialization) {
    FakeStream s;
    _BufferedStream<> bs(&s);
    bs  << "asldkfjasfkd"
        << __BS::_strint{234}
        << "this-is-another-string"
        << __BS::_strint{-1234234}
    ;

    bs.flush();
    puts(s.output.c_str());
    const static char RESP[] = BSTR(12,asldkfjasfkd) BSTR(03,234)
                     BSTR(22,this-is-another-string) BSTR(08,-1234234);
    EXPECT_EQ(s.output, RESP);
}

void print_resp(const std::string_view s) {
    for (char c: s) {
        if (c == '\r') printf("\\r\\n");
        else if (c == '\n') { }
        else putchar(c);
    }
    puts("");
}

TEST(redis, deserialization) {
    FakeStream s;
    s.in = SSTR(asldkfjasfkd) INTEGER(234) BSTR(21,this-is-a-bulk_string)
        ARRAY_HEADER(3) SSTR(asdf) INTEGER(75) BSTR(3,jkl) INTEGER(-1234234);
    print_resp(s.in);
    _BufferedStream<> bs(&s);
    auto a = bs.parse_response_item();
    EXPECT_EQ(a.mark, simple_string::mark());
    EXPECT_EQ(a.get<simple_string>(), "asldkfjasfkd");

    auto b = bs.parse_response_item();
    EXPECT_EQ(b.mark, integer::mark());
    EXPECT_EQ(b.get<integer>().val, 234);

    auto c = bs.parse_response_item();
    EXPECT_EQ(c.mark, bulk_string::mark());
    EXPECT_EQ(c.get<bulk_string>(), "this-is-a-bulk_string");

    auto d = bs.parse_response_item();
    EXPECT_EQ(d.mark, array_header::mark());
    EXPECT_EQ(d.get<array_header>().val, 3);

    auto e = bs.parse_response_item();
    EXPECT_EQ(e.mark, simple_string::mark());
    EXPECT_EQ(e.get<simple_string>(), "asdf");

    auto f = bs.parse_response_item();
    EXPECT_EQ(f.mark, integer::mark());
    EXPECT_EQ(f.get<integer>().val, 75);

    auto g = bs.parse_response_item();
    EXPECT_EQ(g.mark, bulk_string::mark());
    EXPECT_EQ(g.get<bulk_string>(), "jkl");

    auto h = bs.parse_response_item();
    EXPECT_EQ(h.mark, integer::mark());
    EXPECT_EQ(h.get<integer>().val, -1234234);
}

__attribute__((used))
void asdfjkl(_BufferedStream<>& bs) {
    bs.BLMOVE("src", "dest", "LEFT", "RIGHT", "234");
}

TEST(redis, cmd_serialization) {
    FakeStream s;
    _BufferedStream<> bs(&s);
#define TEST_CMD(cmd, truth) {                  \
    s.in = "-ERR unknown command 'asdf'\r\n";   \
    auto r = cmd;                               \
    EXPECT_EQ(s.output, truth);                 \
    print_resp(s.output);                       \
    EXPECT_TRUE(r.is_type<error_message>());    \
    EXPECT_EQ(r.get<error_message>(), "ERR unknown command 'asdf'");      \
    s.output.clear();                           \
}
#define AKey BSTR(4, akey)
#define Key1 BSTR(4, key1)
#define Key2 BSTR(4, key2)
#define Key3 BSTR(4, key3)
#define Key4 BSTR(4, key4)
#define N(n) "*"#n"\r\n"
#define F1 BSTR(2, f1)
#define F2 BSTR(2, f2)
#define F3 BSTR(2, f3)
#define F4 BSTR(2, f4)
#define F5 BSTR(2, f5)
#define V1 BSTR(2, v1)
#define V2 BSTR(2, v2)
#define V3 BSTR(2, v3)
#define V4 BSTR(2, v4)
#define V5 BSTR(2, v5)
#define nFIELDS(n) BSTR(6,FIELDS) BSTR(01,n)

    TEST_CMD(bs.execute("SDIFF", "asdf", "jkl", "hahaha"),
        N(4) BSTR(5,SDIFF) BSTR(4,asdf) BSTR(3,jkl) BSTR(6,hahaha));

    // DEFINE_COMMAND1 (SCARD, key);
    TEST_CMD(bs.SCARD("akey"), N(2) BSTR(5,SCARD) AKey);

    // DEFINE_COMMAND3 (SMOVE, source, destination, member);
    TEST_CMD(bs.SMOVE("source", "dest", "member"),
        N(4) BSTR(5,SMOVE) BSTR(6,source) BSTR(4,dest) BSTR(6,member));

    // DEFINE_COMMAND3s(HMSET, key, field, value);
    TEST_CMD(bs.HMSET("akey", "f1", "v1", "f2", "v2", "f3", "v3"),
        N(8) BSTR(5,HMSET) AKey F1 V1 F2 V2 F3 V3);

    // DEFINE_COMMAND4 (LINSERT, key, BEFORE_or_AFTER, pivot, element);
    TEST_CMD(bs.LINSERT("akey", "BEFORE", "pivot", "element"),
        N(5) BSTR(7,LINSERT) AKey BSTR(6,BEFORE) BSTR(5,pivot) BSTR(7,element));

    // DEFINE_COMMAND2fs(HTTL, key);
    TEST_CMD(bs.HTTL("akey", "f1", "f2", "f3", "f4"),
        N(8) BSTR(4,HTTL) AKey nFIELDS(4) F1 F2 F3 F4);

    // DEFINE_COMMAND2sn(ZUNIONSTORE, destination, key);
    TEST_CMD(bs.ZUNIONSTORE("destination", "key1", "key2", "key3"),
        N(6) BSTR(11,ZUNIONSTORE) BSTR(11,destination) BSTR(01,3) Key1 Key2 Key3);

    // DEFINE_COMMAND2m(BITPOS, key, bit, opt_start_end_BYTE_BIT);
    TEST_CMD(bs.BITPOS("akey", "bit", "start", "end", "BIT"),
        N(6) BSTR(6,BITPOS) AKey BSTR(3,bit) BSTR(5,start) BSTR(3,end) BSTR(3,BIT));
}

TEST(redis, cmd) {
    if (!photon::is_using_default_engine()) return;
    photon::init(INIT_EVENT_EPOLL | INIT_EVENT_KQUEUE, 0);
    DEFER(photon::fini());
    auto client = new_tcp_socket_client();
    DEFER(delete client);
    auto s = client->connect({IPAddr::V4Loopback(), 6379});
    if (!s) {
        #ifdef __APPLE__
        return;
        #endif
    }
    DEFER(delete s);
    _BufferedStream<> bs(s);
    const char key[] = "zvxbhm";
    bs.DEL(key);
    DEFER(bs.DEL(key));
    auto r = bs.HSET(key, "hahaha", "qwer", "key2", "value2");
    EXPECT_TRUE(r.is_type<integer>());
    EXPECT_EQ(r.get<integer>().val, 2);
    r = bs.LLEN(key);
    EXPECT_TRUE(r.is_failed());
    LOG_DEBUG(r.get_error_message());
}
