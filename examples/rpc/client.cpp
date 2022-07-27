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

#include "client.h"

#include <photon/common/alog-stdstring.h>
#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/net/socket.h>

#include <ctime>

int64_t ExampleClient::RPCHeartbeat(photon::net::EndPoint ep) {
    Heartbeat::Request req;
    req.now = photon::now;
    Heartbeat::Response resp;
    int ret = 0;

    auto stub = pool->get_stub(ep, false);
    if (!stub) return -1;
    DEFER(pool->put_stub(ep, ret < 0));

    ret = stub->call<Heartbeat>(req, resp);

    if (ret < 0) return -errno;

    return resp.now;
}

std::string ExampleClient::RPCEcho(photon::net::EndPoint ep,
                                   const std::string& str) {
    Echo::Request req;
    req.str.assign(str);

    Echo::Response resp;
    // string or variable length fields should pre-set buffer
    char tmpbuf[4096];
    resp.str = {tmpbuf, 4096};
    int ret;

    auto stub = pool->get_stub(ep, false);
    if (!stub) return {};
    DEFER(pool->put_stub(ep, ret < 0));

    ret = stub->call<Echo>(req, resp);

    if (ret < 0) return {};
    std::string s(resp.str.c_str());
    return s;
}

ssize_t ExampleClient::RPCRead(photon::net::EndPoint ep, const std::string& fn,
                               const struct iovec* iovec, int iovcnt) {
    ReadBuffer::Request req;
    req.fn.assign(fn);
    ReadBuffer::Response resp;
    resp.buf.assign(iovec, iovcnt);
    int ret = 0;

    auto stub = pool->get_stub(ep, false);
    if (!stub) return -1;
    DEFER(pool->put_stub(ep, ret < 0));
    ret = stub->call<ReadBuffer>(req, resp);
    if (ret < 0) return ret;
    return resp.ret;
}

ssize_t ExampleClient::RPCWrite(photon::net::EndPoint ep, const std::string& fn,
                                struct iovec* iovec, int iovcnt) {
    WriteBuffer::Request req;
    req.fn.assign(fn);
    req.buf.assign(iovec, iovcnt);
    WriteBuffer::Response resp;
    int ret = 0;

    auto stub = pool->get_stub(ep, false);
    if (!stub) return -1;
    DEFER(pool->put_stub(ep, ret < 0));

    ret = stub->call<WriteBuffer>(req, resp);
    if (ret < 0) return ret;
    return resp.ret;
}

void ExampleClient::RPCTestrun(photon::net::EndPoint ep) {
    Testrun::Request req;

    // prepare some data
    int iarr[] = {1, 2, 3, 4};

    // sample of
    char buf1[] = "some";
    char buf2[] = "buf";
    char buf3[] = "content";
    IOVector iov;
    iov.push_back(buf1, 4);
    iov.push_back(buf2, 3);
    iov.push_back(buf3, 7);

    // set up request
    req.someuint = 2233U;
    req.someint64 = 4455LL;
    req.somestruct = Testrun::SomePODStruct{.foo = true, .bar = 32767};
    req.intarray.assign(iarr, 4);
    req.somestr.assign("Hello");
    req.buf.assign(iov.iovec(), iov.iovcnt());

    // make room for response
    Testrun::Response resp;
    // iovector should pre_allocated
    IOVector riov;
    riov.push_back(1024);
    resp.buf.assign(riov.iovec(), riov.iovcnt());

    int ret = 0;
    auto stub = pool->get_stub(ep, false);
    if (!stub) return;
    DEFER(pool->put_stub(ep, ret < 0));
    // Single step call
    ret = stub->call<Testrun>(req, resp);
    // ret < 0 means RPC failed on send or receive
    if (ret < 0) {
        LOG_INFO("RPC fail");
    } else {
        LOG_INFO("RPC succ: ", VALUE(resp.len),
                 VALUE((char*)riov.begin()->iov_base));
    }
}
