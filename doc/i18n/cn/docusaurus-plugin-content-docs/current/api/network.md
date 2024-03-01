---
sidebar_position: 6
toc_max_heading_level: 4
---

# 网络

### 命名空间

`photon::net::`

### 头文件

`<photon/net/socket.h>`

### Socket封装

#### 概述

- Photon把Socket抽象成`ISocketClient`，`ISocketServer`，以及`ISocketStream`三个接口
- Photon Socket同时支持IPv4和IPv6
- 所有的Socket实现都是非阻塞的

#### ISocketClient

- `ISocketClient`只有connect方法，但可以连接多种协议，如TCP、UDP、Unix Domain Socket等。 

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

- `ISocketServer`有一系列的bind、listen、accept等方法，以及启动loop和终止loop的方法。它通过设置一个回调函数，来指定所有连接（ISocketStream）的处理入口。
- accept成功会返回一个`ISocketStream`指针；回调函数的唯一参数也是这个指针。

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

- `ISocketStream`有两组接口，一组send/recv，另一组read/write。

- 前者等价于libc在non-blocking fd条件下的send/recv，即收发的字节数可能小于指定的count数量；而后者对前者做了封装，
要求收发数量等于count才能够返回。因此从本质上来说，read 等价于 fully recv，write 等价于 fully send。

- 除此以外，针对每组接口还提供了对应的 io-vector 版本，等价于libc的sendmsg和recvmsg。 

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


#### Socket类的继承关系

![socket](/img/api/socket.png)


### Socket实现

#### 通用TCP
这是最常用的TCP socket。
```cpp
ISocketClient* new_tcp_socket_client();
ISocketServer* new_tcp_socket_server();
```

#### UDS

UDS server的autoremove参数表示在关闭server时是否要自动删除UDS文件。

```cpp
ISocketClient* new_uds_client();
ISocketServer* new_uds_server(bool autoremove = false);
```

#### io_uring
这一组client/server使用原生的io_uring读写接口，而不使用libc的send/recv。此外，它的socket fd也不是non-blocking的。

在Ping-pong的大连接小流量场景应该优先使用io_uring socket，在Streaming的大流量场景应该优先使用普通TCP socket，详情请参考网络性能测试。
```cpp
ISocketClient* new_iouring_tcp_client();
ISocketServer* new_iouring_tcp_server();
```

#### zerocopy
TCP zerocopy send功能，依赖4.15以上内核，能够降低CPU负载。对大的buffer效果比较好。
```cpp
ISocketClient* new_zerocopy_tcp_client();
ISocketServer* new_zerocopy_tcp_server();
```

#### Edge-Trigger
边缘触发的TCP socket实现。
```
ISocketClient* new_et_tcp_socket_client();
ISocketServer* new_et_tcp_socket_server();
```

#### SMC
基于[SMC-R](https://www.ibm.com/docs/en/aix/7.2?topic=access-shared-memory-communications-over-rdma-smc-r)协议的RDMA实现。
```
ISocketClient* new_smc_socket_client();
ISocketServer* new_smc_socket_server();
```

#### F-Stack + DPDK
运行在DPDK polling模式下的协程，底层网络库采用了F-Stack（FreeBSD + UserSpace）
```
ISocketClient* new_fstack_dpdk_socket_client();
ISocketServer* new_fstack_dpdk_socket_server();
```


### 网络地址

网络地址主要有 `IPAddr` 和 `Endpoint` 两个类，后者等于前者加上端口号。

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
    // 默认的地址是IPv4 0.0.0.0，我们认为这是未定义的
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
    // 默认的endpoint是0.0.0.0:0，我们认为这是未定义的
    bool undefined();
};
```

:::tip 小知识
一个监听了 `::0` 地址的 server，可以同时服务 v4 和 v6 协议的 client
:::

### HTTP

Photon有两个HTTP组件，一个是基于libcurl+协程封装的异步框架（只有client功能），另一个是自研的轻量化HTTP client/server（以下简称Photon HTTP）。

#### libcurl

##### 初始化
photon::init的时候需要加上libcurl的IO_ENGINE
```cpp
photon::init(INIT_EVENT_DEFAULT, INIT_IO_LIBCURL);
```

##### 头文件
```cpp
#include <photon/net/curl.h>
```

##### 使用

每次请求都需要new一个net::cURL()对象，然后调用它的GET/POST等方法。

##### 封装

在`<photon/fs/httpfs/httpfs.h>`中，我们封装了一个满足POSIX文件读写接口的fs，并且对于HTTP header、状态返回码等做了一些封装。

#### Photon HTTP

自研的Photon HTTP框架没有第三方依赖，且init的时候不需要额外的IO_ENGINE。

##### 头文件
```cpp
#include <photon/net/http/client.h>
#include <photon/net/http/server.h>
```

##### 使用

请参考 `net/http/test/client_perf.cpp` 和 `net/http/test/server_perf.cpp`

##### 封装

同样的，我们在`<photon/fs/httpfs/httpfs.h>`也封装了它的fs，称为httpfs v2。