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
#include <photon/net/socket.h>
#include <gtest/gtest.h>
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
        output += std::string_view{(const char*)buf, n};
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
        output += std::string_view{(char*)buf, count};
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

const static char RESP[] = "+asldkfjasfkd\r\n:234\r\n$21\r\nthis-is-a-bulk_string\r\n*3\r\n+asdf\r\n:75\r\n$3\r\njkl\r\n:-1234234\r\n";

TEST(redis, serialization) {
    FakeStream s;
    _BufferedStream<> bs(&s);
    bs  << simple_string("asldkfjasfkd")
        << integer(234)
        << bulk_string("this-is-a-bulk_string")
        << make_array(simple_string("asdf"), integer(75), bulk_string("jkl"))
        << integer(-1234234);

    bs.flush();
    puts(s.output.c_str());
    EXPECT_EQ(s.output, RESP);
}

TEST(redis, deserialization) {
    FakeStream s;
    s.in = RESP;
    _BufferedStream<> bs(&s);
    auto a = bs.parse_item();
    EXPECT_EQ(a.mark, simple_string::mark());
    EXPECT_EQ(a.get<simple_string>(), "asldkfjasfkd");

    auto b = bs.parse_item();
    EXPECT_EQ(b.mark, integer::mark());
    EXPECT_EQ(b.get<integer>().val, 234);

    auto c = bs.parse_item();
    EXPECT_EQ(c.mark, bulk_string::mark());
    EXPECT_EQ(c.get<bulk_string>(), "this-is-a-bulk_string");

    auto d = bs.parse_item();
    EXPECT_EQ(d.mark, redis::array<>::mark());
    EXPECT_EQ(d.get<integer>().val, 3);

    auto e = bs.parse_item();
    EXPECT_EQ(e.mark, simple_string::mark());
    EXPECT_EQ(e.get<simple_string>(), "asdf");

    auto f = bs.parse_item();
    EXPECT_EQ(f.mark, integer::mark());
    EXPECT_EQ(f.get<integer>().val, 75);

    auto g = bs.parse_item();
    EXPECT_EQ(g.mark, bulk_string::mark());
    EXPECT_EQ(g.get<bulk_string>(), "jkl");

    auto h = bs.parse_item();
    EXPECT_EQ(h.mark, integer::mark());
    EXPECT_EQ(h.get<integer>().val, -1234234);
}
