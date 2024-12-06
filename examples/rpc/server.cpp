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

#include "server.h"

#include <fcntl.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog.h>
#include <sys/uio.h>
#include <unistd.h>

// Server side rpc handler
int ExampleServer::do_rpc_service(Testrun::Request* req,
                                  Testrun::Response* resp, IOVector* iov,
                                  IStream* conn) {
    LOG_INFO(VALUE(req->someuint));
    LOG_INFO(VALUE(req->someint64));
    LOG_INFO(VALUE(req->somestruct.foo), VALUE(req->somestruct.bar));
    LOG_INFO(VALUE(req->somestr.c_str()));
    LOG_INFO(VALUE((char*)req->buf.begin()->iov_base));

    iov->push_back((void*)"some response", 14);

    resp->len = 14;
    resp->buf.assign(iov->iovec(), iov->iovcnt());

    return 0;
}

int ExampleServer::do_rpc_service(Echo::Request* req, Echo::Response* resp,
                                  IOVector*, IStream*) {
    resp->str = req->str;

    return 0;
}

int ExampleServer::do_rpc_service(Heartbeat::Request* req,
                                  Heartbeat::Response* resp, IOVector*,
                                  IStream*) {
    resp->now = photon::now;

    return 0;
}

int ExampleServer::do_rpc_service(ReadBuffer::Request* req,
                                  ReadBuffer::Response* resp, IOVector* iov,
                                  IStream*) {
    auto fd = ::open(req->fn.c_str(), O_RDONLY, 0644);
    if (fd < 0) {
        resp->ret = -errno;
        return 0;
    }
    DEFER(::close(fd));
    iov->push_back(4096);
    resp->ret = ::preadv(fd, iov->iovec(), iov->iovcnt(), 0);
    if (resp->ret < 0) {
        resp->ret = -errno;
        resp->buf.assign(nullptr, 0);
    } else {
        iov->shrink_to(resp->ret);
        resp->buf.assign(iov->iovec(), iov->iovcnt());
    }
    return 0;
}

int ExampleServer::do_rpc_service(WriteBuffer::Request* req,
                                  WriteBuffer::Response* resp, IOVector* iov,
                                  IStream*) {
    auto fd = ::open(req->fn.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        resp->ret = -errno;
        return 0;
    }
    DEFER(::close(fd));
    iov->push_back(4096);
    resp->ret = ::pwritev(fd, req->buf.begin(), req->buf.size(), 0);
    if (resp->ret < 0) {
        resp->ret = -errno;
    }
    return 0;
}

int ExampleServer::run(int port) {
    if (server->bind(port) < 0)
        LOG_ERRNO_RETURN(0, -1, "Failed to bind port `", port)
    if (server->listen() < 0) LOG_ERRNO_RETURN(0, -1, "Failed to listen");
    server->set_handler({this, &ExampleServer::serve});
    LOG_INFO("Started rpc server at `", server->getsockname());
    return server->start_loop(true);
}