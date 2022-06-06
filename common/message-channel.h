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
#include <cinttypes>
#include <photon/common/iovector.h>

class IMessageChannel
{
public:
    virtual ~IMessageChannel() { }

    // send a message as a whole, returns # of bytes sent, or < 0 if failed
    virtual ssize_t send(iovector& msg) = 0;
    ssize_t send(const void* buf, uint64_t size)
    {
        IOVector msg;
        msg.push_back((void*)buf, size);
        return send(msg);
    }

    // recv a message as a whole, returns # of bytes received,
    // or < 0 if failed, and set proper errno indicating reason
    //     (errno == ENOBUFS) indicating insufficient buffer space,
    //     and the return value is negative of total space needed; users should
    //     reallocate the buffer and receive it again.
    virtual ssize_t recv(iovector& msg) = 0;
    ssize_t recv(void* buf, uint64_t size)
    {
        IOVector msg;
        msg.push_back(buf, size);
        return recv(msg);
    }

    const static uint64_t OUT_OF_ORDER   = 1 << 0;  // messages may be transferred out-of-order
    const static uint64_t DUPLICATION    = 1 << 1;  // messages may be duplicated
    const static uint64_t LOSSY          = 1 << 2;  // messages may be lost
    const static uint64_t ROUND_TRIP     = 1 << 3;  // sending a message results in receiving a response message
    const static uint64_t ALLOCATION     = 1 << 4;  // allocate buffer internally


    // Multiple threads can send and recv messages to/from this channel concurrently.
    // If `OUT_OF_ORDER` is also set, responses may physically arrive in an order
    // different from that when sending the request messages.
    const static uint64_t CONCURRENT_RT  = 1 << 5;  // concurrent round trip

    // Send next message before receiving the response
    const static uint64_t PIPELINING     = 1 << 6;

    // the send and recv in one step, assuming ROUND_TRIP
    virtual ssize_t send_recv(iovector& request, iovector& response)
    {
        auto ret = send(request);
        return (ret <= 0) ? ret : recv(response);
    }

    // get current flags, the bit-ored constants
    virtual uint64_t flags() = 0;

    // set flags, the bit-ored constants
    // returning 0 for success, or the flags that failed to set
    virtual uint64_t flags(uint64_t f) = 0;
};
