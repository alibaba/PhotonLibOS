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

#pragma once

#include <photon/rpc/rpc.h>
#include <photon/net/socket.h>
#include <sys/uio.h>

#include "protocol.h"

struct ExampleClient {
    std::unique_ptr<photon::rpc::StubPool> pool;

    // Create a tcp rpc connection pool.
    // Unused connections will be dropped after 10 seconds(10UL*1000*1000).
    // Socket I/O timeout is 1 second(1UL*1000*1000).
    // Socket type could be tcp/udp/zerocopy/rdma ...
    ExampleClient() {
        auto sock_client = std::shared_ptr<photon::net::ISocketClient>(photon::net::new_tcp_socket_client());
        pool.reset(photon::rpc::new_stub_pool(10UL * 1000 * 1000, 1UL * 1000 * 1000, sock_client));
    }

    int64_t RPCHeartbeat(photon::net::EndPoint ep);

    std::string RPCEcho(photon::net::EndPoint ep, const std::string& str);

    void RPCEchoPerf(photon::net::EndPoint ep, void* req_buf, void* resp_buf, size_t buf_size);

    ssize_t RPCRead(photon::net::EndPoint ep, const std::string& fn,
                    const struct iovec* iovec, int iovcnt);

    ssize_t RPCWrite(photon::net::EndPoint ep, const std::string& fn,
                     struct iovec* iovec, int iovcnt);

    void RPCTestrun(photon::net::EndPoint ep);
};