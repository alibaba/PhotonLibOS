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

struct LogBuffer;
LogBuffer& operator << (LogBuffer& log, const in_addr& iaddr);
LogBuffer& operator << (LogBuffer& log, const sockaddr_in& addr);
LogBuffer& operator << (LogBuffer& log, const sockaddr& addr);
namespace photon {
namespace net
{
    union IPAddr
    {
        uint32_t addr = 0;
        struct { uint8_t a, b, c, d; };
        explicit IPAddr(uint32_t nl)
        {
            from_nl(nl);
        }
        explicit IPAddr(const char* s)
        {
            struct in_addr addr;
            if (inet_aton(s, &addr) == 0)
                return;  // invalid IPv4 address
            from_nl(addr.s_addr);
        }
        IPAddr() = default;
        IPAddr(const IPAddr& rhs) = default;
        uint32_t to_nl() const
        {
            return htonl(addr);
        }
        void from_nl(uint32_t nl)
        {
            addr = ntohl(nl);
        }
    };

    struct EndPoint
    {
        IPAddr addr;
        uint16_t port = 0;
        EndPoint() = default;
        EndPoint(IPAddr ip, uint16_t port): addr(ip), port(port) {}
        EndPoint(const struct sockaddr_in& addr_in)
        {
            from(addr_in);
        }
        sockaddr_in to_sockaddr_in() const
        {
            struct sockaddr_in addr_in;
            addr_in.sin_family = AF_INET;
            addr_in.sin_addr.s_addr = addr.to_nl();
            addr_in.sin_port = htons(port);
            return addr_in;
        }
        void from_sockaddr_in(const struct sockaddr_in& addr_in)
        {
            addr.from_nl(addr_in.sin_addr.s_addr);
            port = ntohs(addr_in.sin_port);
        }
        void from(const struct sockaddr_in& addr_in)
        {
            from_sockaddr_in(addr_in);
        }
        bool operator==(const EndPoint& rhs) const {
            return rhs.addr.addr == addr.addr && rhs.port == port;
        }
        bool operator!=(const EndPoint& rhs) const {
            return !operator==(rhs);
        }
        bool empty() const {
            return addr.addr == 0 && port == 0;
        }
    };

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
        // recv some bytes from the socket;
        // return actual # of bytes recvd, which may be LESS than `count`;
        // may block once at most, when there's no data yet in the socket;
        virtual ssize_t recv(void *buf, size_t count, int flags = 0) = 0;
        virtual ssize_t recv(const struct iovec *iov, int iovcnt, int flags = 0) = 0;

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
        virtual void terminate() = 0;
    };

    extern "C" ISocketClient* new_tcp_socket_client();
    extern "C" ISocketServer* new_tcp_socket_server();
    extern "C" ISocketClient* new_uds_client();
    extern "C" ISocketServer* new_uds_server(bool autoremove = false);
    extern "C" ISocketClient* new_tcp_socket_pool(ISocketClient* client, uint64_t expiration = -1UL,
                                                  bool client_ownership = false);

    extern "C" ISocketServer* new_zerocopy_tcp_server();
    extern "C" ISocketClient* new_iouring_tcp_client();
    extern "C" ISocketServer* new_iouring_tcp_server();
    extern "C" int et_poller_init();
    extern "C" int et_poller_fini();
    extern "C" ISocketClient* new_et_tcp_socket_client();
    extern "C" ISocketServer* new_et_tcp_socket_server();
}
}

namespace std {
template<>
struct hash<photon::net::EndPoint> {
    hash<uint64_t> hasher;
    size_t operator()(const photon::net::EndPoint& x) const {
        return hasher((x.addr.to_nl() << 16) | x.port);
    }
};
}
