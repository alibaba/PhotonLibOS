---
sidebar_position: 5
toc_max_heading_level: 4
---

# Write Your First Example

In this example, we will demonstrate a simple example using various Photon modules,
i.e., `common`, `thread`, `fs`, `io` and `net`. The program basically set up some Photon threads
in the background, created a Photon file for IO, and sent buffer through Photon socket.
Photon locks and condition viariables are used as well.

:::note
The example code is written in [std-compatible API](../api/std-compatible-api). 

If you want to use the raw API, please refer to this [doc](../api/thread). It can provide more flexible functionalities.
:::

### 1. Initialize environment

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

After `photon::init`, the [**Env**](../api/env) is initialized, which means the coroutine stack is successfully allocated on current [**vCPU**](../api/vcpu-and-multicore). 

Now you can create multiple Photon [**threads**](../api/thread) to run in parallel, or migrate them to other vCPUs.

The `photon::fini` is responsible for deallocating the environment. 
It's wrapped in a helper macro called `DEFER` from `common/utility.h`.
Like Go's defer, it ensures the statement be executed before the function returns.
Its implementation is based on the concept of `RAII`.

### 2. Create thread

There are many ways to create a thread. Just like the old ways you use `std::thread`, but try `photon_std::thread` instead.

```cpp
// Global function
int func(int a, char* b) {}

// Use global function to create a thread.
// Will be automatically joined when thread object destructed, unless been detached.
photon_std::thread th(func, 1, '2');
th.detach();

// Create a thread with anonymous function (lambda)
photon_std::thread th([&] {
    // Access variables directly in the context
});

// Create a thread with class member function
class A {
    void f() {
        new photon_std::thread(&A::g, this, 1, '2');
    }
    void g(int a, char* b) {}
};
```

### 3. Concurrency

A thread is basically a function, and thus an execution unit. 
You can create multiple threads at a time to achieve concurrency, and wait them finished by Join.

```cpp
std::vector<photon_std::thread> threads;
for (int i = 0; i < 100; ++i) {
    threads.emplace_back(func, 1, '2');
}
for (auto& th : threads) {
    th.join();
}
```

### 4. Lock and synchronization

This is a typical `condition_variable` usage. Again, we switch to Photon's exclusive namespace.

```cpp
bool condition = false;
photon_std::mutex mu;
photon_std::condition_variable cv;

// Consumer thread
photon_std::thread([&]{
    auto timeout = std::chrono::duration<std::chrono::seconds>(10);
    photon_std::unique_lock<photon_std::mutex> lock(mu);
    while (!condition) {
        cv.wait(lock, timeout);
    }
}).detach();

// Producer thread
photon_std::thread([&]{
    photon_std::lock_guard<photon_std::mutex> lock(mu);
    condition = true;
    cv.notify_one();
}).detach();
```

### 5. File IO

Photon has POSIX-like encapsulations for file and filesystem. In this example we first create a `IFileSystem` under current working dir, and then open a `IFile` from it. 

You can switch the io_engine from `photon::fs::ioengine_psync` to `photon::fs::ioengine_iouring`, 
if your event_engine satisfied. io_uring IO is naturally asynchronous and non-blocking. 
It can also use page cache compared with aio.

In addition to local file systems, Photon also supports a variety of remote file systems, such as httpfs, extfs, fusefs, etc.

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

Both IFile and IFileSystem object will close itself at destruction. Again, RAII.

### 6. Socket

The `tcp_socket_client` + `tcp_socket_server` is the most regular combination for client and server. Please refer to the API docs for more socket types.

#### Client

```cpp
#include <photon/net/socket.h>

auto client = photon::net::new_tcp_socket_client();
if (client == nullptr) {
    LOG_ERRNO_RETURN(0, -1, "failed to create tcp client");
}
DEFER(delete client);

photon::net::EndPoint ep("127.0.0.1:9527");
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
The stream is of type `photon::net::ISocketStream`. It has extended write/read methods compared to traditional libc's send/recv.

Essentially, write = fully_send, and read = fully_recv.
:::

:::info
LOG_INFO is Photon's unique logging system. It is based on template metaprogramming techniques 
and optimizes results at compile time. So runtime overhead is reduced.
Compared with other logging systems based on `sprintf`, Photon logging is 2~3 times faster than them.

The \` symbol is a generic placeholder for multiple types of elements.
:::

### Full source code

You may visit https://github.com/alibaba/PhotonLibOS/blob/main/examples/simple/simple.cpp to get the source.

Or create your own CMake demo project guided by the [How to integrate](./how-to-integrate.md) doc.
