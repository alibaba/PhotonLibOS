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
#include <sys/uio.h>

#include "protocol.h"

struct ExampleClient {
    std::unique_ptr<photon::rpc::StubPool> pool;

    // create a tcp rpc connection pool
    // unused connections will be drop after 10 seconds(10UL*1000*1000)
    // TCP connection will failed in 1 second(1UL*1000*1000) if not accepted
    // and connection send/recv will take 5 socneds(5UL*1000*1000) as timedout
    ExampleClient()
        : pool(photon::rpc::new_stub_pool(10UL * 1000 * 1000, 1UL * 1000 * 1000,
                                          5UL * 1000 * 1000)) {}

    int64_t RPCHeartbeat(photon::net::EndPoint ep);

    std::string RPCEcho(photon::net::EndPoint ep, const std::string& str);

    ssize_t RPCRead(photon::net::EndPoint ep, const std::string& fn,
                    const struct iovec* iovec, int iovcnt);

    ssize_t RPCWrite(photon::net::EndPoint ep, const std::string& fn,
                     struct iovec* iovec, int iovcnt);

    void RPCTestrun(photon::net::EndPoint ep);
};