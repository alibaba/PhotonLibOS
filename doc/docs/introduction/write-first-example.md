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
The example code is written with [std-compatible API](../api/std-compatible-api).
:::

### 1. Initialize Photon environment

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

After `photon::init`, the [Env](../api/env) is initialized, which means the coroutine stack is successfully allocated on current [`vCPU`](../api/vcpu-and-multicore). You can now create multiple Photon [`threads`](../api/thread) to run in parallel, or migrate them to other vCPUs.

The `photon::fini` is responsible for deallocating the environment. 
It's wrapped in a helper macro called `DEFER` from `common/utility.h`.
Like Go's defer, it ensures the statement be executed before the function returns.
Its implementation is based on the concept of `RAII`.

### 2. Create a thread

Just like the old way you create a `std::thread`, use `photon_std::thread` instead. The thread will start running immediately once you create it.

```cpp
int run_server(int a, MyType* b, MyType& c) {
	// ...
}

photon_std::thread th(run_server, a, b, std::ref(c));

// Or new obj
auto th = new photon_std::thread(run_server, a, b, std::ref(c));
DEFER(delete th);

// Or anonymous function by lambda
new photon_std::thread([&] {
		// Use a, b, c
	}
);
```

:::tip
If you want to use the raw API, rather than the std-compatible one, please refer to this [doc](../api/thread#thread_create11)

```cpp
#include <photon/photon.h>
#include <photon/thread/thread11.h>

auto th = photon::thread_create11(run_server, a, b, std::ref(c));
```

:::

### 3. Lock and synchronization

This is a typical `condition_variable` usage. Again, we switch to Photon's exclusive namespace.

```cpp
bool condition = false;
photon_std::mutex mu;
photon_std::condition_variable cv;

// Producer thread
photon_std::lock_guard<photon_std::mutex> lock(mu);
condition = true;
cv.notify_one();

// Consumer thread
auto timeout = std::chrono::duration<std::chrono::seconds>(10);
photon_std::unique_lock<photon_std::mutex> lock(mu);
while (!condition) {
    cv.wait(lock, timeout);
}
```

### 4. File IO

Photon has POSIX-like encapsulations for file and filesystem. In this example we first create a `IFileSystem` under current working dir, and then open a `IFile` from it. 

You can switch the io_engine from `photon::fs::ioengine_psync` to `photon::fs::ioengine_iouring`, if your event_engine satisfied.

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

### 5. Socket

The `tcp_socket_client` + `tcp_socket_server` is the most regular combination for client and server. Please refer to the API docs for more socket types.

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
server->bind(9527, photon::net::IPAddr());	// bind to 0.0.0.0
server->listen();

LOG_INFO("Server is listening for port ` ...", 9527);
server->start_loop(true);
```

:::info
The stream is a instance of `photon::net::ISocketStream`. It has extented write/read methods compared to triditional libc's send/recv.

Essentially, write = fully_send, and read = fully_recv.
:::

### Full source code

You may visit https://github.com/alibaba/PhotonLibOS/blob/main/examples/simple/simple.cpp to get the source.

Or create your own CMake demo project guided by the [How to integrate](./how-to-integrate.md) doc.