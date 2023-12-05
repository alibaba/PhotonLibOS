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
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <cstring>

#include <photon/common/stream.h>
#include <photon/common/callback.h>
#include <photon/common/object.h>

#ifdef __linux__
#define _in_addr_field s6_addr32
#else // macOS
#define _in_addr_field __u6_addr.__u6_addr32
#endif

struct LogBuffer;
LogBuffer& operator << (LogBuffer& log, const in_addr& iaddr);
LogBuffer& operator << (LogBuffer& log, const sockaddr_in& addr);
LogBuffer& operator << (LogBuffer& log, const in6_addr& iaddr);
LogBuffer& operator << (LogBuffer& log, const sockaddr_in6& addr);
LogBuffer& operator << (LogBuffer& log, const sockaddr& addr);

namespace photon {
namespace net {

    struct __attribute__ ((packed)) IPAddr {
    public:
        union {
            in6_addr addr = {};
            struct { uint16_t _1, _2, _3, _4, _5, _6; uint8_t a, b, c, d; };
        };
        // For compatibility, the default constructor is still 0.0.0.0 (IPv4)
        IPAddr() {
            map_v4(htonl(INADDR_ANY));
        }
        // V6 constructor (Internet Address)
        explicit IPAddr(in6_addr internet_addr) {
            addr = internet_addr;
        }
        // V6 constructor (Network byte order)
        IPAddr(uint32_t nl1, uint32_t nl2, uint32_t nl3, uint32_t nl4) {
            addr._in_addr_field[0] = nl1;
            addr._in_addr_field[1] = nl2;
            addr._in_addr_field[2] = nl3;
            addr._in_addr_field[3] = nl4;
        }
        // V4 constructor (Internet Address)
        explicit IPAddr(in_addr internet_addr) {
            map_v4(internet_addr);
        }
        // V4 constructor (Network byte order)
        explicit IPAddr(uint32_t nl) {
            map_v4(nl);
        }
        // String constructor
        explicit IPAddr(const char* s) {
            if (inet_pton(AF_INET6, s, &addr) > 0) {
                return;
            }
            in_addr v4_addr;
            if (inet_pton(AF_INET, s, &v4_addr) > 0) {
                map_v4(v4_addr);
                return;
            }
            // Invalid string, make it a default value
            *this = IPAddr();
        }
        // Check if it's actually an IPv4 address mapped in IPV6
        bool is_ipv4() const {
            if (ntohl(addr._in_addr_field[2]) != 0x0000ffff) {
                return false;
            }
            if (addr._in_addr_field[0] != 0 || addr._in_addr_field[1] != 0) {
                return false;
            }
            return true;
        }
        // We regard the default IPv4 0.0.0.0 as undefined
        bool undefined() const {
            return *this == V4Any();
        }
        // Should ONLY be used for IPv4 address
        uint32_t to_nl() const {
            return addr._in_addr_field[3];
        }
        bool is_loopback() const {
            return is_ipv4() ? (*this == V4Loopback()) : (*this == V6Loopback());
        }
        bool is_broadcast() const {
            // IPv6 does not support broadcast
            return is_ipv4() && (*this == V4Broadcast());
        }
        bool is_link_local() const {
            if (is_ipv4()) {
                return (to_nl() & htonl(0xffff0000)) == htonl(0xa9fe0000);
            } else {
                return (addr._in_addr_field[0] & htonl(0xffc00000)) == htonl(0xfe800000);
            }
        }
        bool operator==(const IPAddr& rhs) const {
            return memcmp(this, &rhs, sizeof(rhs)) == 0;
        }
        bool operator!=(const IPAddr& rhs) const {
            return !(*this == rhs);
        }
    public:
        static IPAddr V6None() {
            return IPAddr(htonl(0xffffffff), htonl(0xffffffff), htonl(0xffffffff), htonl(0xffffffff));
        }
        static IPAddr V6Any() { return IPAddr(in6addr_any); }
        static IPAddr V6Loopback() { return IPAddr(in6addr_loopback); }
        static IPAddr V4Broadcast() { return IPAddr(htonl(INADDR_BROADCAST)); }
        static IPAddr V4Any() { return IPAddr(htonl(INADDR_ANY)); }
        static IPAddr V4Loopback() { return IPAddr(htonl(INADDR_LOOPBACK)); }
    private:
        void map_v4(in_addr addr_) {
            map_v4(addr_.s_addr);
        }
        void map_v4(uint32_t nl) {
            addr._in_addr_field[0] = 0x00000000;
            addr._in_addr_field[1] = 0x00000000;
            addr._in_addr_field[2] = htonl(0xffff);
            addr._in_addr_field[3] = nl;
        }
    };

    static_assert(sizeof(IPAddr) == 16, "IPAddr size incorrect");

    struct __attribute__ ((packed)) EndPoint {
        IPAddr addr;
        uint16_t port = 0;
        EndPoint() = default;
        EndPoint(IPAddr ip, uint16_t port) : addr(ip), port(port) {}
        explicit EndPoint(const char* s);
        bool is_ipv4() const {
            return addr.is_ipv4();
        };
        bool operator==(const EndPoint& rhs) const {
            return rhs.addr == addr && rhs.port == port;
        }
        bool operator!=(const EndPoint& rhs) const {
            return !operator==(rhs);
        }
        bool undefined() const {
            return addr.undefined() && port == 0;
        }
    };

    static_assert(sizeof(EndPoint) == 18, "Endpoint size incorrect");

    // operators to help with logging IP addresses
    LogBuffer& operator << (LogBuffer& log, const IPAddr addr);
    LogBuffer& operator << (LogBuffer& log, const EndPoint ep);

    class ISocketBase {
    public:
        virtual ~ISocketBase() = default;

        virtual Object* get_underlay_object(uint64_t recursion = 0) = 0;
        int get_underlay_fd() {
            auto ret = get_underlay_object(-1);
            return ret ? (int) (uint64_t) ret : -1;
        }

        virtual int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) = 0;
        virtual int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) = 0;
        template<typename T>
        int setsockopt(int level, int option_name, T value) {
            return setsockopt(level, option_name, &value, sizeof(value));
        }
        template<typename T>
        int getsockopt(int level, int option_name, T* value) {
            socklen_t len = sizeof(*value);
            return getsockopt(level, option_name, value, &len);
        }

        // get/set timeout, in us, (default +∞)
        virtual uint64_t timeout() const = 0;
        virtual void timeout(uint64_t tm) = 0;
    };

    class ISocketName {
    public:
        virtual ~ISocketName() = default;
        virtual int getsockname(EndPoint& addr) = 0;
        virtual int getpeername(EndPoint& addr) = 0;
        virtual int getsockname(char* path, size_t count) = 0;
        virtual int getpeername(char* path, size_t count) = 0;
        EndPoint getsockname() { EndPoint ep; getsockname(ep); return ep; }
        EndPoint getpeername() { EndPoint ep; getpeername(ep); return ep; }
    };

    class ISocketStream : public IStream, public ISocketBase, public ISocketName {
    public:
        static const int ZEROCOPY_FLAG = 0x4000000;
        // recv some bytes from the socket;
        // return actual # of bytes recvd, which may be LESS than `count`;
        // may block once at most, when there's no data yet in the socket;
        virtual ssize_t recv(void *buf, size_t count, int flags = 0) = 0;
        virtual ssize_t recv(const struct iovec *iov, int iovcnt, int flags = 0) = 0;

        // recv at `least` bytes to buffer (`buf`, `count`)
        ssize_t recv_at_least(void* buf, size_t count, size_t least, int flags = 0);
        ssize_t recv_at_least_mutable(struct iovec *iov, int iovcnt, size_t least, int flags = 0);

        // read count bytes and drop them
        // return true/false for success/failure
        bool skip_read(size_t count);

        // send some bytes to the socket;
        // return actual # of bytes sent, which may be LESS than `count`;
        // may block once at most, when there's no free space in the socket's internal buffer;
        virtual ssize_t send(const void *buf, size_t count, int flags = 0) = 0;
        virtual ssize_t send(const struct iovec *iov, int iovcnt, int flags = 0) = 0;

        virtual ssize_t sendfile(int in_fd, off_t offset, size_t count) = 0;
    };

    class ISocketClient : public ISocketBase, public Object {
    public:
        // Connect to a remote IPv4 endpoint.
        // If `local` endpoint is not empty, its address will be bind to the socket before connecting to the `remote`.
        virtual ISocketStream* connect(EndPoint remote, EndPoint local = EndPoint()) = 0;
        // Connect to a Unix Domain Socket.
        virtual ISocketStream* connect(const char* path, size_t count = 0) = 0;
    };

    class ISocketServer : public ISocketBase, public ISocketName, public Object {
    public:
        virtual int bind(uint16_t port = 0, IPAddr addr = IPAddr()) = 0;
        virtual int bind(const char* path, size_t count) = 0;
        int bind(const char* path) { return bind(path, strlen(path)); }
        virtual int listen(int backlog = 1024) = 0;
        virtual ISocketStream* accept(EndPoint* remote_endpoint = nullptr) = 0;

        using Handler = Callback<ISocketStream*>;
        virtual ISocketServer* set_handler(Handler handler) = 0;
        virtual int start_loop(bool block = false) = 0;
        // Close the listening fd. It's the user's responsibility to close the active connections.
        virtual void terminate() = 0;
    };

    extern "C" ISocketClient* new_tcp_socket_client();
    extern "C" ISocketServer* new_tcp_socket_server();
    extern "C" ISocketClient* new_tcp_socket_client_ipv6();
    extern "C" ISocketServer* new_tcp_socket_server_ipv6();
    extern "C" ISocketClient* new_uds_client();
    extern "C" ISocketServer* new_uds_server(bool autoremove = false);
    extern "C" ISocketClient* new_tcp_socket_pool(ISocketClient* client, uint64_t expiration = -1UL,
                                                  bool client_ownership = false);

    extern "C" ISocketClient* new_zerocopy_tcp_client();
    extern "C" ISocketServer* new_zerocopy_tcp_server();
    extern "C" ISocketClient* new_iouring_tcp_client();
    extern "C" ISocketServer* new_iouring_tcp_server();
    extern "C" int et_poller_init();
    extern "C" int et_poller_fini();
    extern "C" ISocketClient* new_et_tcp_socket_client();
    extern "C" ISocketServer* new_et_tcp_socket_server();
    extern "C" ISocketClient* new_smc_socket_client();
    extern "C" ISocketServer* new_smc_socket_server();
    extern "C" ISocketClient* new_fstack_dpdk_socket_client();
    extern "C" ISocketServer* new_fstack_dpdk_socket_server();
}
}

namespace std {

template<>
struct hash<photon::net::EndPoint> {
    size_t operator()(const photon::net::EndPoint& x) const {
        hash<std::string_view> hasher;
        return hasher(std::string_view((const char*) &x, sizeof(x)));
    }
};

template<>
struct hash<photon::net::IPAddr> {
    size_t operator()(const photon::net::IPAddr& x) const {
        hash<std::string_view> hasher;
        return hasher(std::string_view((const char*) &x, sizeof(x)));
    }
};

}
