---
sidebar_position: 5
toc_max_heading_level: 4
---

# 代码样例

在此示例中，我们将演示如何使用 Photon 的各个模块， 即`common`、`thread`、`fs`、`io` 和 `net`。

该程序在后台启动了一些 Photon 协程，利用封装的fs接口创建一个文件用于IO读写，并通过 Photon socket 发送数据到网络缓冲区，还使用了锁和条件变量等。

:::note
该例子是用 [std-compatible API](../api/std-compatible-api) 编写的。

如果你想使用原生 API 而不是 std-compatible API, 请参考 [文档](../api/thread#thread_create11)，它可以提供更加灵活的功能。
:::

### 1. 初始化协程环境

```cpp
#include <photon/photon.h>
#include <photon/thread/std-compat.h>

int main() {
	int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    if (ret != 0) {
        return -1;
    }
    DEFER(photon::fini());
    // ...
}
```

执行完 `photon::init` 之后, 协程环境（[**Env**](../api/env)）就初始化好了，这意味着协程栈已经成功地在当前 [**vCPU**](../api/vcpu-and-multicore) 上分配。

现在你可以创建多个并发执行的协程（[**threads**](../api/thread)）了，或者把它们迁移到其他 vCPU。

`photon::fini` 负责销毁环境，它被一个叫做 `DEFER` 的宏（来自于`common/utility.h`）封装了一下。
类似 Go 语言的 defer, 这个宏会确保你的这个语句会在函数返回之前执行。 它的实现正是基于 C++ `RAII` 的概念。

### 2. 创建协程

跟创建 `std::thread` 线程的方法类似，不过我们使用 `photon_std::thread`。

```cpp
// 全局函数
int func(int a, char* b) {}

// 用全局函数创建协程，thread对象析构时自动Join，除非调用了detach
photon_std::thread th(func, 1, '2');
th.detach();

// 或者使用匿名函数（lambda）
photon_std::thread th([&] {
    // 直接访问上下文中的变量
});

// 用类的成员函数创建协程
class A {
    void f() {
        new photon_std::thread(&A::g, this, 1, '2');
    }
    void g(int a, char* b) {}
};
```

### 3. 并发

协程本质上就是函数，是一个执行单元。你可以通过创建多个执行单元实现并发，并且用join等待任务结束。

```cpp
std::vector<photon_std::thread> threads;
for (int i = 0; i < 100; ++i) {
    threads.emplace_back(func, 1, '2');
}
for (auth& th : threads) {
    th.join();
}
```

### 4. 锁和同步

这是一个典型的 `condition_variable` 的用法，同上，只需要把 namespace 改成 photon_std 即可，其他代码都跟 std 的一样。

```cpp
bool condition = false;
photon_std::mutex mu;
photon_std::condition_variable cv;

// 消费者协程
photon_std::thread([&]{
    auto timeout = std::chrono::duration<std::chrono::seconds>(10);
    photon_std::unique_lock<photon_std::mutex> lock(mu);
    while (!condition) {
        cv.wait(lock, timeout);
    }
}).detach();

// 生产者协程
photon_std::thread([&]{
    photon_std::lock_guard<photon_std::mutex> lock(mu);
    condition = true;
    cv.notify_one();
}).detach();
```

### 5. 文件 IO

Photon 封装了一个类 POSIX 的文件系统接口。本例中我们首先在当前工作目录下创建一个 `IFileSystem` 对象，然后使用它又打开了一个 `IFile` 对象。

如果环境允许的话，你可以把 io_engine 从 `photon::fs::ioengine_psync` 改成 `photon::fs::ioengine_iouring`，
这样就可以使用io_uring读写文件了。io_uring IO 天然异步非阻塞，并且跟 aio 相比，还可以使用 page cache。

除了本地文件系统以为，Photon还支持多种远程文件系统，如 httpfs、extfs、fusefs 等。

```cpp
#include <photon/fs/localfs.h>

auto fs = photon::fs::new_localfs_adaptor(".", photon::fs::ioengine_psync);
if (!fs) {
    LOG_ERRNO_RETURN(0, -1, "failed to create fs");
}
DEFER(delete fs);

auto file = fs->open("test-file", O_WRONLY | O_CREAT | O_TRUNC, 0644);
if (!file) {
    LOG_ERRNO_RETURN(0, -1, "failed to open file");
}
DEFER(delete file);

ssize_t n_written = file->write(buf, 4096);
```

IFile 和 IFileSystem 在析构的时候都会自动 close 他们打开的资源，这是 RAII 理念的再一次运用。

### 6. Socket

`tcp_socket_client` 和 `tcp_socket_server` 是客户端/服务端最常见的组合搭配， 请参考文档查阅更多的 socket 类型的封装。

#### Client

```cpp
#include <photon/net/socket.h>

auto client = photon::net::new_tcp_socket_client();
if (client == nullptr) {
    LOG_ERRNO_RETURN(0, -1, "failed to create tcp client");
}
DEFER(delete client);

photon::net::EndPoint ep{photon::net::IPAddr("127.0.0.1"), 9527};
auto stream = client->connect(ep);
if (!stream) {
    LOG_ERRNO_RETURN(0, -1, "failed to connect server");
}
DEFER(delete stream);

// Send data to socket
char buf[1024];
if (stream->send(buf, 1024) != 1024) {
    LOG_ERRNO_RETURN(0, -1, "failed to write socket");
}
```

#### Server

```cpp
#include <photon/net/socket.h>

auto server = photon::net::new_tcp_socket_server();
if (server == nullptr) {
    LOG_ERRNO_RETURN(0, -1, "failed to create tcp server");
}
DEFER(delete server);

auto handler = [&](photon::net::ISocketStream* stream) -> int {       
    char buf[1024];
    ssize_t ret = stream->recv(buf, 1024);
    if (ret <= 0) {
        LOG_ERRNO_RETURN(0, -1, "failed to read socket");
    }     
    return 0;
};

server->set_handler(handler);
server->bind_v4localhost(9527);
server->listen();

LOG_INFO("Server is listening for port ` ...", 9527);
server->start_loop(true);
```

:::info
stream 是 `photon::net::ISocketStream` 的实例，它跟传统的 libc send/recv 相比，扩展了 read 和 write 的方法。

本质上来说，write 等价于 fully_send，read 等价 fully_recv，即读/写到固定的字节数才返回。
:::

:::info

LOG_INFO 是 Photon 独特的日志系统，它基于大量的模板元编程技巧，在编译时进行结果优化，从而降低运行时开销。
跟大多数基于 `sprintf` 的其他日志系统对比起来，Photon 日志的速度比他们快 2~3 倍。

\` 符号是一个占位符，它可以匹配多种类型的元素。
:::

### 完整代码

访问 https://github.com/alibaba/PhotonLibOS/blob/main/examples/simple/simple.cpp 查看完整代码。

或者按照 [集成](./how-to-integrate.md) 中展示的教程，定制你自己的 CMake 项目。 