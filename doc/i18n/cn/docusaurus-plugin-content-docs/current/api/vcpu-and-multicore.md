---
sidebar_position: 3
toc_max_heading_level: 5
---

# vCPU和多核

### 概念

Photon 的 vCPU 的概念等价于 OS 线程。

每个 vCPU 都有一个调度器，决定 [threads](thread) 的执行和切换顺序。

### 开启多核

目前 Photon 有两种方式使用多核。

### 1. 手动创建OS线程，并初始化Env

可以利用`thread_migrate`将协程迁移到其他vCPU上去

```cpp
std::thread([&]{
	photon::init();
	DEFER(photon::fini());
    
    auto th = photon::thread_create11(func);
    photon::thread_migrate(th, vcpu);
}).detach();
```

### 2. 使用 `WorkPool`

#### 头文件

`<photon/thread/workerpool.h>`

#### 描述

Create a WorkPool to manage multiple vCPUs, and utilize multi-core.

#### 构造函数

```cpp
WorkPool(size_t vcpu_num, int ev_engine = 0, int io_engine = 0, int thread_mod = -1);
```

- `vcpu_num` 使用多少个vCPU，即OS线程
- `ev_engine` 使用何种事件引擎
- `io_engine` 使用哪些IO引擎
- `thread_mod` 协程工作模式
	- -1： 非协程模式，即同步执行
	- 0： 每个任务都创建一个新的协程
	- \>0：会创建一个协程池 `thread_pool` 执行任务，池子大小为该数量

#### 公共方法

##### 1. 异步执行

```cpp
template <typename Task>
int WorkPool::async_call(Task* task);
```

- `async_call` 的实现原理是利用一个 MPMC Queue 去传递消息，将 Task 放到 WorkPool 内部的多个 vCPU 上去执行。调用方不等待执行完毕。
- task通常可以是一个new出来的lambda函数，执行完之后会自动调用delete释放。

例子如下：

```cpp
photon::WorkPool pool(4, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 32768);
photon::semephore sem;

pool.async_call(new auto ([&]{
    photon::thread_sleep(1);
    sem.signal(1);
}));
```

##### 2. 获取vCPU数量

```cpp
int WorkPool::get_vcpu_num();
```

:::note
只包含 WorkPool 的 vCPU 数量，主 OS 线程的 vCPU 不算。
:::

##### 3. 迁移

WorkPool migrate 的本质还是使用协程的迁移功能，不使用MPMC Queue。

```cpp
int WorkPool::thread_migrate(photon::thread* th = CURRENT, size_t index = -1UL);
```

- `th` 需要迁移的协程
- `index` 目标 vCPU 的 index。如果 index 不在 [0, vcpu_num) 范围内，如默认值-1UL，将使用 round-robin 的方式选择下一个 vCPU。
     
返回0表示成功， 小于0表示失败。

## 事件引擎

每个 vCPU 都有一个 `MasterEventEngine`，它阻塞在 I/O 事件上并通过 `thread_interrupt()` 唤醒睡眠中的光子协程。定义于 `<photon/io/fd-events.h>`。

### MasterEventEngine

每 vCPU 一个，驱动空闲循环：

- `wait_for_fd(fd, interests, timeout)` —— 挂起当前协程直到 FD 事件
- `wait_and_fire_events(timeout)` —— 阻塞并通过调用 `thread_interrupt(thread*, EOK)` 触发事件
- `cancel_wait()` —— 唤醒引擎（通过 eventfd 或等价机制）

### CascadingEventEngine

用于复杂的多 FD 等待。不会阻塞 vCPU —— 只阻塞调用它的光子协程：

- `add_interest(Event)` / `rm_interest(Event)` —— 管理 FD 注册
- `wait_for_events(data[], count, timeout)` —— 等待多个事件

### 事件标志

```cpp
EVENT_READ    = 1
EVENT_WRITE   = 2
EVENT_ERROR   = 4
EDGE_TRIGGERED = 0x4000
ONE_SHOT       = 0x8000
```

## 后端

### epoll

头文件：`<photon/io/epoll.cpp>`、`<photon/io/epoll-ng.cpp>`。

- `epoll_create(1)` + eventfd 用于 `cancel_wait`
- 按 fd 索引的 `_inflight_events` vector
- ONE_SHOT interest，`data=CURRENT`（光子协程指针）
- `wait_and_fire_events()`：`epoll_wait()` → 每个事件都调用 `thread_interrupt((thread*)data, EOK)`
- `epoll-ng.cpp` 是边沿触发变体

### io_uring

头文件：`<photon/io/iouring-wrapper.h>`。

功能最丰富的后端：

- SQPoll 模式、IOPoll 模式、注册文件
- `io_uring_prep_poll_add` / `multishot` 用于 fd 事件
- `async_io()` 模板支持：read、write、send、recv、connect、accept、open、mkdir、close、splice、fsync
- 通过 `io_uring_prep_cancel` 取消
- 内核版本检测（5.11、5.15、5.18、5.19）用于特性门控
- `IouringFixedFileFlag`（第 32 位）标记注册 FD

### kqueue

头文件：`<photon/io/kqueue.cpp>`。BSD/macOS 后端：

- `EVFILT_USER` 用于自唤醒（`cancel_wait`）
- `EV_ONESHOT` 用于单次事件等待

### select

内联 stub。在没有 epoll/kqueue 的平台上的回退方案。

## 异步 I/O 后端

### libaio

头文件：`<photon/io/aio-wrapper.h>`。

- `libaio_pread` / `preadv` / `pwrite` / `pwritev` —— 需要 `O_DIRECT` 和对齐的缓冲
- 需要 `fd_events_init()` 进行事件通知

### POSIX AIO

- `posixaio_pread` / `pwrite` / `fsync` / `fdatasync`

## 用户态网络

| 头文件 | 描述 |
|--------|------|
| `<photon/io/fstack-dpdk.h>` | F-Stack / DPDK：`fstack_socket` / `connect` / `bind` / `listen` / `accept` / `send` / `recv` / `close` |
| `<photon/io/spdknvme-wrapper.h>` | SPDK NVMe 块设备访问 |
| `<photon/io/spdkbdev-wrapper.h>` | SPDK 通用块设备 |

## 信号处理

头文件：`<photon/io/signal.h>`。

- `sync_signal_init()` 和 `sync_signal(signum, handler)` —— 信号处理函数在专用光子协程中运行
- `block_all_signal()` 阻塞除 `SIGSTOP` / `SIGKILL` 之外的所有信号

`<photon/io/reset_handle.h>` 定义 `ResetHandle`，是带 `reset()` 虚方法的侵入式链表节点。`reset_all_handle()` 在光子重新初始化时被调用。

## 设计决策

- **ONE_SHOT 语义。** 所有后端都使用 one-shot 事件注册，避免惊群并保证每个事件只唤醒一个协程。
- **EOK 约定。** 事件以 `EOK`（ENXIO，"Event of NeXt I/O"）触发；被唤醒的协程通过检查 `error_number` 区分"事件唤醒"与"超时"。
- **事件数据即协程指针。** `data=CURRENT` 把光子协程指针存入事件，使 `thread_interrupt()` 可直接调用，无需查表。
- **Master 与 Cascading 分离。** Master 驱动空闲循环（阻塞 vCPU）；Cascading 处理每协程的多 FD 等待，而不阻塞其他协程。