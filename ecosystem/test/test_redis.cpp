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
#include <photon/common/memory-stream/memory-stream.h>
#include <photon/net/socket.h>
#include <gtest/gtest.h>
#include "../../test/ci-tools.h"
using namespace photon;
using namespace photon::net;
using namespace photon::redis;
using namespace std;

#define SSTR(s)     "+" #s CRLF
#define BSTR(n, s)  "$" #n CRLF #s CRLF
#define INTEGER(n)  ":" #n CRLF
#define ARRAY_HEADER(n) "*" #n CRLF

struct __RC : public RedisClient {
public:
    using RedisClient::_strint;
};

TEST(redis, serialization) {
    auto s = new_string_socket_stream();
    DEFER(delete s);
    RedisClient rc(s, false);
    rc  << "asldkfjasfkd"
        << __RC::_strint{234}
        << "this-is-another-string"
        << __RC::_strint{-1234234}
    ;
    rc.flush();
    puts(s->output().c_str());
    const static char RESP[] = BSTR(12,asldkfjasfkd) BSTR(03,234)
                     BSTR(22,this-is-another-string) BSTR(08,-1234234);
    EXPECT_EQ(s->output(), RESP);
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
    auto s = new_string_socket_stream();
    DEFER(delete s);
    auto RESP = SSTR(asldkfjasfkd) INTEGER(234) BSTR(21,this-is-a-bulk_string)
        ARRAY_HEADER(3) SSTR(asdf) INTEGER(75) BSTR(3,jkl) INTEGER(-1234234);
    s->set_input(RESP, false);
    print_resp(s->input());
    RedisClient rc(s, false);
    auto a = rc.parse_response_item();
    EXPECT_EQ(a.mark, simple_string::mark());
    EXPECT_EQ(a.get<simple_string>(), "asldkfjasfkd");

    auto b = rc.parse_response_item();
    EXPECT_EQ(b.mark, integer::mark());
    EXPECT_EQ(b.get<integer>().val, 234);

    auto c = rc.parse_response_item();
    EXPECT_EQ(c.mark, bulk_string::mark());
    EXPECT_EQ(c.get<bulk_string>(), "this-is-a-bulk_string");

    auto d = rc.parse_response_item();
    EXPECT_EQ(d.mark, array_header::mark());
    EXPECT_EQ(d.get<array_header>().val, 3);

    auto e = rc.parse_response_item();
    EXPECT_EQ(e.mark, simple_string::mark());
    EXPECT_EQ(e.get<simple_string>(), "asdf");

    auto f = rc.parse_response_item();
    EXPECT_EQ(f.mark, integer::mark());
    EXPECT_EQ(f.get<integer>().val, 75);

    auto g = rc.parse_response_item();
    EXPECT_EQ(g.mark, bulk_string::mark());
    EXPECT_EQ(g.get<bulk_string>(), "jkl");

    auto h = rc.parse_response_item();
    EXPECT_EQ(h.mark, integer::mark());
    EXPECT_EQ(h.get<integer>().val, -1234234);
}

__attribute__((used))
void asdfjkl(RedisClient& bs) {
    bs.BLMOVE("src", "dest", "LEFT", "RIGHT", "234");
}


TEST(redis, cmd_serialization) {
    auto s = new_string_socket_stream();
    DEFER(delete s);
    RedisClient rc(s, false);
#define ERRMSG "ERR unknown command 'asdf'"
#define TEST_CMD(cmd, truth) {                  \
    s->set_input("-" ERRMSG CRLF, false);       \
    auto r = cmd;                               \
    EXPECT_EQ(s->output(), truth);              \
    print_resp(s->output());                    \
    EXPECT_TRUE(r.is_type<error_message>());    \
    EXPECT_EQ(r.get<error_message>(), ERRMSG);  \
    s->output().clear();                        \
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

    TEST_CMD(rc.execute("SDIFF", "asdf", "jkl", "hahaha"),
        N(4) BSTR(5,SDIFF) BSTR(4,asdf) BSTR(3,jkl) BSTR(6,hahaha));

    // DEFINE_COMMAND1 (SCARD, key);
    TEST_CMD(rc.SCARD("akey"), N(2) BSTR(5,SCARD) AKey);

    // DEFINE_COMMAND3 (SMOVE, source, destination, member);
    TEST_CMD(rc.SMOVE("source", "dest", "member"),
        N(4) BSTR(5,SMOVE) BSTR(6,source) BSTR(4,dest) BSTR(6,member));

    // DEFINE_COMMAND3s(HMSET, key, field, value);
    TEST_CMD(rc.HMSET("akey", "f1", "v1", "f2", "v2", "f3", "v3"),
        N(8) BSTR(5,HMSET) AKey F1 V1 F2 V2 F3 V3);

    // DEFINE_COMMAND4 (LINSERT, key, BEFORE_or_AFTER, pivot, element);
    TEST_CMD(rc.LINSERT("akey", "BEFORE", "pivot", "element"),
        N(5) BSTR(7,LINSERT) AKey BSTR(6,BEFORE) BSTR(5,pivot) BSTR(7,element));

    // DEFINE_COMMAND2fs(HTTL, key);
    TEST_CMD(rc.HTTL("akey", "f1", "f2", "f3", "f4"),
        N(8) BSTR(4,HTTL) AKey nFIELDS(4) F1 F2 F3 F4);

    // DEFINE_COMMAND2sn(ZUNIONSTORE, destination, key);
    TEST_CMD(rc.ZUNIONSTORE("destination", "key1", "key2", "key3"),
        N(6) BSTR(11,ZUNIONSTORE) BSTR(11,destination) BSTR(01,3) Key1 Key2 Key3);

    // DEFINE_COMMAND2m(BITPOS, key, bit, opt_start_end_BYTE_BIT);
    TEST_CMD(rc.BITPOS("akey", "bit", "start", "end", "BIT"),
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
    RedisClient rc(s, false);
    const char key[] = "zvxbhm";
    rc.DEL(key);
    DEFER(rc.DEL(key));
    auto r = rc.HSET(key, "hahaha", "qwer", "key2", "value2");
    EXPECT_TRUE(r.is_type<integer>());
    EXPECT_EQ(r.get<integer>().val, 2);
    r = rc.LLEN(key);
    EXPECT_TRUE(r.is_failed());
    LOG_DEBUG(r.get_error_message());
}
