---
sidebar_position: 6
toc_max_heading_level: 4
---

# Network

### Namespace

`photon::net::`

### Headers

`<photon/net/socket.h>`

### Socket Encapsulation

#### Brief introduction

- Photon abstracts Socket into three interfaces: `ISocketClient`, `ISocketServer`, and `ISocketStream`
- Photon Socket supports both IPv4 and IPv6
- All Socket implementations are non-blocking

#### ISocketClient

- `ISocketClient` only has the connect method, but can connect to multiple protocols, such as TCP, UDP, Unix Domain Socket, etc.

```cpp
class ISocketClient : public ISocketBase, public Object {
public:
    // Connect to a remote endpoint.
    // If `local` endpoint is not empty, its address will be bind to the socket before connecting to the `remote`.
    virtual ISocketStream* connect(const EndPoint& remote, const EndPoint* local = nullptr) = 0;
    // Connect to a Unix Domain Socket.
    virtual ISocketStream* connect(const char* path, size_t count = 0) = 0;
};
```

#### ISocketServer

- `ISocketServer` has a series of methods such as bind, listen and accept, as well as methods for starting and terminating loop.
The callback function is used to specify the entry for all connections. 
- A successful accept will return an `ISocketStream` pointer; the only parameter of the callback function is also this pointer.

```cpp
class ISocketServer : public ISocketBase, public ISocketName, public Object {
public:
    virtual int bind(const EndPoint& ep) = 0;
    virtual int bind(const char* path, size_t count) = 0;
    int bind(uint16_t port, IPAddr a);
    // ...

    virtual int listen(int backlog = 1024) = 0;
    virtual ISocketStream* accept(EndPoint* remote_endpoint = nullptr) = 0;

    using Handler = Callback<ISocketStream*>;
    virtual ISocketServer* set_handler(Handler handler) = 0;
    virtual int start_loop(bool block = false) = 0;
    
    // Close the listening fd. It's the user's responsibility to close the active connections.
    virtual void terminate() = 0;
};
```

#### ISocketStream

- There are two interfaces of `ISocketStream`, one is send/recv and the other is read/write.

- The former is equivalent to libc's send/recv for non-blocking fd. The number of bytes it sent or received may be less than the specified count.
  However, the latter is an encapsulation of the former, and it requires the bytes to be exactly equal to count when function returns. 
  So essentially, read is equivalent to fully_recv and write is equivalent to fully_send.

- In addition, a corresponding io-vector version has been provided for these two interfaces, same to libc's sendmsg and recvmsg.

```cpp
class ISocketStream : public IStream, public ISocketBase, public ISocketName {
public:
    virtual ssize_t recv(void *buf, size_t count, int flags = 0) = 0;
    virtual ssize_t recv(const struct iovec *iov, int iovcnt, int flags = 0) = 0;
    
    virtual ssize_t send(const void *buf, size_t count, int flags = 0) = 0;
    virtual ssize_t send(const struct iovec *iov, int iovcnt, int flags = 0) = 0;
    
    virtual ssize_t read(void *buf, size_t count) = 0;
    virtual ssize_t readv(const struct iovec *iov, int iovcnt) = 0;
    
    virtual ssize_t write(const void *buf, size_t count) = 0;
    virtual ssize_t writev(const struct iovec *iov, int iovcnt) = 0;
};
```


#### Socket class hierarchy

![socket](/img/api/socket.png)


### Socket Implementations

#### General TCP
This is the most commonly used combination of TCP sockets
```cpp
ISocketClient* new_tcp_socket_client();
ISocketServer* new_tcp_socket_server();
```

#### UDS

The autoremove parameter indicates whether the UDS file should be automatically deleted when the server is shut down.

```cpp
ISocketClient* new_uds_client();
ISocketServer* new_uds_server(bool autoremove = false);
```

#### io_uring

This group of clients/servers uses the native io_uring IO instead of libc's send/recv. 
What's more, its socket fd is not non-blocking.

In the scenario of large connections and small traffic (we call it Ping-pong), the io_uring socket should be used first.
However, for large traffic (we call it Streaming), the common TCP socket should be used first. 
For details, please refer to the network performance test.

```cpp
ISocketClient* new_iouring_tcp_client();
ISocketServer* new_iouring_tcp_server();
```

#### zerocopy
The TCP zerocopy send relies on kernel 4.15 or above and can reduce CPU workload. It works better for large buffers.
```cpp
ISocketClient* new_zerocopy_tcp_client();
ISocketServer* new_zerocopy_tcp_server();
```

#### Edge-Trigger

Edge-triggered TCP socket implementation.

```
ISocketClient* new_et_tcp_socket_client();
ISocketServer* new_et_tcp_socket_server();
```

#### SMC
RDMA implementation based on [SMC-R](https://www.ibm.com/docs/en/aix/7.2?topic=access-shared-memory-communications-over-rdma-smc-r) protocol.
```
ISocketClient* new_smc_socket_client();
ISocketServer* new_smc_socket_server();
```

#### F-Stack + DPDK
Coroutine network running with DPDK polling mode. The underlying library is F-Stack (FreeBSD + UserSpace)
```
ISocketClient* new_fstack_dpdk_socket_client();
ISocketServer* new_fstack_dpdk_socket_server();
```


### IP address and Endpoint

The main classes ares `IPAddr` and `Endpoint`. The latter equals to the first plus port number.

#### IPAddr

```cpp
struct IPAddr {
    // For compatibility, the default constructor is 0.0.0.0 (IPv4 Any)
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
    // Default addr is IPv4 0.0.0.0, and we regard it as undefined
    bool undefined();
    // Should ONLY be used for IPv4 address
    uint32_t to_nl() const;
    bool is_loopback() const;
    bool is_broadcast() const;
    bool is_link_local() const;
    
    static IPAddr V6None();
    static IPAddr V6Any();
    static IPAddr V6Loopback();
    static IPAddr V4Broadcast();
    static IPAddr V4Any();
    static IPAddr V4Loopback();
};
```

#### Endpoint

```cpp
struct EndPoint {
    EndPoint() = default;
    EndPoint(IPAddr ip, uint16_t port) : addr(ip), port(port) {}
    explicit EndPoint(const char* ep);
    EndPoint(const char* ip, uint16_t port) : addr(ip), port(port) {}
    bool is_ipv4() const;
    // Default endpoint is 0.0.0.0:0，and we regard it as undefined
    bool undefined();
};
```

:::tip
A server listens to `::0` can serve both IPv4 and IPv6 clients at the same time.
:::

### HTTP

Photon has two HTTP components, one is an asynchronous framework based on libcurl + coroutine (only client), 
and the other is a self-developed lightweight HTTP client/server (hereinafter referred to as Photon HTTP).

#### libcurl

##### Initialization
You need to add libcurl as an IO_ENGINE when calling photon's init.
```cpp
photon::init(INIT_EVENT_DEFAULT, INIT_IO_LIBCURL);
```

##### Headers
```cpp
#include <photon/net/curl.h>
```

##### Usage

You need to create a new net::cURL() object for each request, and then call its GET/POST methods.

##### Fs encapsulation

In `<photon/fs/httpfs/httpfs.h>`, we have implemented a httpfs that has the POSIX compatible read/write interfaces, and did some encapsulation of HTTP headers, status codes, etc.

#### Photon HTTP

There are no third-party dependencies of the self-developed Photon HTTP framework, and it does not require additional IO_ENGINE during init.

##### 头文件
```cpp
#include <photon/net/http/client.h>
#include <photon/net/http/server.h>
```

##### 使用

Please refer to `net/http/test/client_perf.cpp` and `net/http/test/server_perf.cpp`

##### 封装

Similarly, we also encapsulated its fs in `<photon/fs/httpfs/httpfs.h>`, which is called httpfs v2.