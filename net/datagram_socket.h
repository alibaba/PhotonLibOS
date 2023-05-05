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

#include <photon/common/message.h>
#include <photon/net/socket.h>

namespace photon {
namespace net {

class IDatagramSocket : public IMessage,
                        public ISocketBase,
                        public ISocketName {
protected:
    virtual int connect(const Addr* addr, size_t addr_len) = 0;
    virtual int bind(const Addr* addr, size_t addr_len) = 0;

    IMessage* cast() { return (IMessage*)this; }

    template <typename B, typename S>
    ssize_t sendto(B* buf, S count, const Addr* to_addr, size_t addr_len,
                   int flags = 0) {
        return cast()->send(buf, count, to_addr, addr_len, flags);
    }
    template <typename B, typename S>
    ssize_t recvfrom(B* buf, S count, Addr* from_addr, size_t* addr_len,
                     int flags = 0) {
        return cast()->recv(buf, count, from_addr, addr_len, flags);
    }

    template <typename B, typename S>
    ssize_t send(B* buf, S count, int flags = 0) {
        return cast()->send(buf, count, nullptr, 0, flags);
    }
    template <typename B, typename S>
    ssize_t recv(B* buf, S count, int flags = 0) {
        return cast()->recv(buf, count, nullptr, nullptr, flags);
    }
};

class UDPSocket : public IDatagramSocket {
protected:
    using base = IDatagramSocket;
    using base::bind;
    using base::connect;

public:
    using base::recv;
    using base::send;
    int connect(const EndPoint ep) { return connect((Addr*)&ep, sizeof(ep)); }
    int bind(const EndPoint ep) { return bind((Addr*)&ep, sizeof(ep)); }
    template <typename B, typename S>
    ssize_t sendto(B* buf, S count, const EndPoint ep, int flags = 0) {
        return base::sendto(buf, count, (Addr*)&ep, sizeof(ep), flags);
    }
    template <typename B, typename S>
    ssize_t recvfrom(B* buf, S count, EndPoint* from, int flags = 0) {
        size_t addr_len = sizeof(*from);
        return base::recvfrom(buf, count, (Addr*)from, &addr_len, flags);
    }
};

class UDS_DatagramSocket : public IDatagramSocket {
protected:
    using base = IDatagramSocket;
    using base::bind;
    using base::connect;

public:
    using base::recv;
    using base::send;
    int connect(const char* path) { return connect((Addr*)path, 0); }
    int bind(const char* path) { return bind((Addr*)path, 0); }
    template <typename B, typename S>
    ssize_t sendto(B* buf, S count, const char* path, int flags = 0) {
        return base::sendto(buf, count, (Addr*)path, 0, flags);
    }
    // Unix Domain Socket can not detect recvfrom address
    // just ignore it, and forward to `recv` method
    template <typename B, typename S>
    ssize_t recvfrom(B* buf, S count, char* from, size_t len, int flags = 0) {
        return base::recv(buf, count, flags);
    }
};

UDPSocket* new_udp_socket(int fd = -1);

UDS_DatagramSocket* new_uds_datagram_socket(int fd = -1);

}  // namespace net
}  // namespace photon
