---
sidebar_position: 2
toc_max_heading_level: 4
---

# 架构

Photon 的主要目标是处理并发与 I/O (包括文件和网络 I/O)

![architecture](/img/photon.png)

它的主要组成部分有：

* [Thread](../api/thread.md)，[vCPU](../api/vcpu-and-multicore.md)，锁和同步原语, 事件引擎，WorkPool任务分发 ...
* 多种文件IO封装: psync, posix_aio, libaio, io_uring
* 多种网络socket分封装: tcp (level-trigger/edge-trigger), unix-domain, zero-copy, libcurl, TLS support, etc.
* 高性能的 RPC client/server 和 HTTP client/server.
* POSIX 文件系统抽象和实现: local fs, http fs, fuse fs, e2fs, 等等
* 其他通用库，如 io-vector操作, 资源池, 对象池, 内存分配器, 回调代理，编译期logging模块, 无锁队列, defer, range lock, 限流模块, 等等

## 分层架构

```
+--------------------------------------------------------------------+
|                      应用代码                                      |
+--------------------------------------------------------------------+
|  go() / thread_create11() / thread_create()  |  photon_std::thread |
+--------------------------------------------------------------------+
|                   photon::thread (协程)                            |
|  调度器 (RunQ/SleepQ/StandbyQ) | 同步原语 | channel<T>             |
+--------------------------------------------------------------------+
|                        vCPU (OS 线程)                              |
|  MasterEventEngine (epoll/io_uring/kqueue) + 空闲协程              |
+--------------------------------------------------------------------+
|                    WorkPool (多 vCPU)                              |
|  std::thread[] + LockfreeMPMCRingQueue 做任务分发                  |
+--------------------------------------------------------------------+
|   fs/ (文件系统)  |  net/ (socket、HTTP)  |  rpc/ (RPC)            |
+--------------------------------------------------------------------+
|              common/ (工具、日志、容器)                             |
+--------------------------------------------------------------------+
```

自顶向下：

1. **应用代码** 使用 Photon 的协程原语（`go`、`thread_create11`、`photon_std::thread`）表达并发。
2. **协程层** 在每 vCPU 的队列上调度 `photon::thread`，提供同步原语（`mutex`、`semaphore`、`condition_variable`、`rwlock`）和 Go 风格的 `channel<T>`。
3. **vCPU 层** 运行空闲协程，在没有可运行光子协程时驱动 `MasterEventEngine`（epoll、io_uring、kqueue 或 select）。
4. **WorkPool** 管理多个 vCPU 并通过无锁 MPMC 队列分发任务。
5. **服务模块**（`fs`、`net`、`rpc`）提供文件系统、网络、RPC 抽象。
6. **common/** 提供跨模块工具：日志、字符串、容器、delegate、I/O 向量、校验和。

## 模块概览

| 模块 | 用途 | 关键文件 |
|------|------|----------|
| [thread/](../api/thread.md) | 协程运行时、调度器、同步原语、channel | `thread.h`、`thread.cpp`、`go.h`、`thread-pool.h`、`workerpool.h` |
| [io/](../api/vcpu-and-multicore.md#事件引擎) | 事件引擎后端（epoll、io_uring、kqueue）、信号处理 | `fd-events.h`、`epoll.cpp`、`iouring-wrapper.cpp`、`kqueue.cpp` |
| [net/](../api/network.md) | socket 抽象、HTTP client/server、TLS、连接池 | `socket.h`、`http/client.h`、`http/server.h`、`security-context/tls-stream.h` |
| [fs/](../api/filesystem-and-io.md) | 文件系统 VFS、本地 fs、HTTP fs、缓存层 | `filesystem.h`、`localfs.h`、`cache/cache.h` |
| [rpc/](../api/rpc.md) | 零拷贝序列化的高性能 RPC | `rpc.h`、`serialize.h`、`out-of-order-execution.h` |
| [common/](../api/common.md) | 日志、字符串工具、容器、delegate、异步辅助 | `alog.h`、`estring.h`、`callback.h`、`iovector.h`、`lockfree_queue.h` |
| [ecosystem/](../ecosystem/overview.md) | Redis 客户端、OSS 客户端、JSON/XML/YAML 解析 | `redis.h`、`oss.h`、`simple_dom.h` |

## 关键设计模式

**接口隔离。** 抽象接口（`IStream`、`IFile`、`IFileSystem`、`ISocketStream`、`ISocketClient`、`ISocketServer`）分离关注点，便于组合。

**装饰器模式。** `ForwardFile`、`ForwardFS`、`ForwardSocket*` 透明地包装下层对象 —— 用于 TLS、缓存、限流、对齐。

**工厂函数。** `new_*_client()`、`new_*_server()`、`new_localfs_adaptor()` 等在抽象接口后创建具体实现。

**策略模式。** 可插拔的事件引擎（epoll / io_uring /kqueue）和 I/O 引擎（psync / libaio / iouring）实现同一接口，在创建时选择。

**Delegate 回调。** `Delegate<R, Ts...>` 为 handler、body writer、异步完成回调提供零开销包装。参见 [Delegate 与 Callback](../api/delegate_callback.md)。

**侵入式集合。** `intrusive_list_node<T>` 用于 reset handle、socket 池、调度器队列，避免逐节点分配。

## 协程与 I/O 集成

所有 I/O 都通过"事件驱动的挂起"与协程系统集成：

1. 系统调用返回 `EAGAIN` → 包装层调用 `wait_for_fd_readable()` 或 `wait_for_fd_writable()`。
2. 当前光子协程挂起，FD 被注册到 `MasterEventEngine`。
3. 事件到达时，`thread_interrupt()` 把协程从 sleep 队列移到 run 队列。
4. 调度器恢复协程，重试系统调用。

该模式出现在 `net/basic_socket.h`（`doio_once` 模板）中，并在整个代码库复用。

## 约定

- **所有权。** 包装对象接受 `ownership` 参数（通常是 `bool`），用于控制是否在析构时删除被包装对象。
- **超时。** 所有 I/O 操作接受与 `photon::now`（微秒精度全局时间戳）集成的 `Timeout` 对象。
- **错误处理。** 出错时函数返回 `-1` 或 `nullptr`，设置 `errno`，并通过 `LOG_ERROR_RETURN` 记日志。
- **线程安全。** Photon 的同步原语（`mutex`、`semaphore`、`condition_variable`）是协程感知的；对应的 `std::` 会阻塞底层 OS 线程。
