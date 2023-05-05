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
// #include <stddef.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "object.h"

class IMessage : public Object {
public:
    const static int Reliable = (1 << 0);
    const static int PreserveOrder = (1 << 1);
    virtual uint64_t flags() = 0;

    virtual uint64_t max_message_size() = 0;

    struct Addr;  // type-erased representation of address

    virtual ssize_t send(const struct iovec* iov, int iovcnt,
                         const Addr* to_addr = nullptr, size_t addr_len = 0,
                         int flags = 0) = 0;

    ssize_t send(const void* buf, size_t count, const Addr* to_addr = nullptr,
                 size_t addr_len = 0, int flags = 0) {
        iovec v{(void*)buf, count};
        return send(&v, 1, to_addr, addr_len, flags);
    }

    virtual ssize_t recv(const struct iovec* iov, int iovcnt,
                         Addr* from_addr = nullptr, size_t* addr_len = nullptr,
                         int flags = 0) = 0;

    ssize_t recv(void* buf, size_t count, Addr* from_addr = nullptr,
                 size_t* addr_len = nullptr, int flags = 0) {
        iovec v{buf, count};
        return recv(&v, 1, from_addr, addr_len, flags);
    }
};
