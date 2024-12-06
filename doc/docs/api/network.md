---
sidebar_position: 6
toc_max_heading_level: 4
---

# Network

### Namespace

`photon::net::`

### Headers

`<photon/net/socket.h>`

### API

#### Client and Server

Network lib provides non-blocking socket implementations for clients and servers.

```cpp
ISocketClient* new_tcp_socket_client();
ISocketServer* new_tcp_socket_server();

ISocketClient* new_tcp_socket_client_ipv6();
ISocketServer* new_tcp_socket_server_ipv6();

ISocketClient* new_uds_client();
ISocketServer* new_uds_server(bool autoremove = false);

ISocketClient* new_iouring_tcp_client();
ISocketServer* new_iouring_tcp_server();

ISocketClient* new_zerocopy_tcp_client();
ISocketServer* new_zerocopy_tcp_server();

ISocketClient* new_et_tcp_socket_client();
ISocketServer* new_et_tcp_socket_server();

ISocketClient* new_smc_socket_client();
ISocketServer* new_smc_socket_server();

ISocketClient* new_fstack_dpdk_socket_client();
ISocketServer* new_fstack_dpdk_socket_server();
```

:::note
An IPv6 socket server listening on `::0` can handle both v4 and v6 socket client.
:::


#### Class Method

```cpp
class ISocketClient : public ISocket {
public:
    virtual ISocketStream* connect(const EndPoint& ep) = 0;
    virtual ISocketStream* connect(const char* path, size_t count = 0) = 0;
};

class ISocketServer : public ISocket {
public:
    virtual int bind(uint16_t port = 0, IPAddr addr = IPAddr()) = 0;
    virtual int bind(const char* path, size_t count) = 0;
    virtual int listen(int backlog = 1024) = 0;
    virtual ISocketStream* accept(EndPoint* remote_endpoint = nullptr) = 0;
    using Handler = Callback<ISocketStream*>;
    virtual ISocketServer* set_handler(Handler handler) = 0;
    virtual int start_loop(bool block = false) = 0;
    virtual void terminate() = 0;
};
```

#### ISocketStream

`ISocketStream` is inherited from `IStream`, with some socket operations like `recv`, `send`, `timeout`, etc.

```cpp
namespace photon {
namespace net {
    class ISocketStream : public IStream {
    public:
        // Receive some bytes from the socket;
        // Return the actual number of bytes received, which may be LESS than `count`;
        virtual ssize_t recv(void *buf, size_t count) = 0;
        virtual ssize_t recv(const struct iovec *iov, int iovcnt) = 0;

        // Send some bytes to the socket;
        // Return the actual number of bytes sent, which may be LESS than `count`;
        virtual ssize_t send(const void *buf, size_t count) = 0;
        virtual ssize_t send(const struct iovec *iov, int iovcnt) = 0;
        
        // Fully receive until `count` bytes.
        virtual ssize_t read(void *buf, size_t count) = 0;
        virtual ssize_t readv(const struct iovec *iov, int iovcnt) = 0;
        
        // Fully send until `count` bytes.
        virtual ssize_t write(const void *buf, size_t count) = 0;
        virtual ssize_t writev(const struct iovec *iov, int iovcnt) = 0;
    };
}
}
```

#### Address and Endpoint
```cpp
namespace photon {
namespace net {
    struct IPAddr {
        in6_addr addr;
        // For compatibility, the default constructor is still 0.0.0.0 (IPv4)
        IPAddr();
        // V6 constructor (Internet Address)
        explicit IPAddr(in6_addr internet_addr);
        // V6 constructor (Network byte order)
        IPAddr(uint32_t nl1, uint32_t nl2, uint32_t nl3, uint32_t nl4);
        // V4 constructor (Internet Address)
        explicit IPAddr(in_addr internet_addr);
        // V4 constructor (Network byte order)
        explicit IPAddr(uint32_t nl);
        // String constructor
        explicit IPAddr(const char* s);
        // Check if it's actually an IPv4 address mapped in IPV6
        bool is_ipv4();
        // We regard the default IPv4 0.0.0.0 as undefined
        bool undefined();
        // Should ONLY be used for IPv4 address
        uint32_t to_nl() const;
        bool is_loopback() const;
        bool is_broadcast() const;
        bool is_link_local() const;
        bool operator==(const IPAddr& rhs) const;
        bool operator!=(const IPAddr& rhs) const;
        static IPAddr V6None();
        static IPAddr V6Any();
        static IPAddr V6Loopback();
        static IPAddr V4Broadcast();
        static IPAddr V4Any();
        static IPAddr V4Loopback();
    };

    // EndPoint represents IP address and port
    // A default endpoint is undefined (0.0.0.0:0)
    struct EndPoint {
        IPAddr addr;
        uint16_t port = 0;
        EndPoint() = default;
        EndPoint(IPAddr ip, uint16_t port);
        bool is_ipv4() const;
        bool operator==(const EndPoint& rhs) const;
        bool operator!=(const EndPoint& rhs) const;
        bool undefined() const;
    };
}
}
```

### Socket class hierarchy

![socket](/img/api/socket.png)



