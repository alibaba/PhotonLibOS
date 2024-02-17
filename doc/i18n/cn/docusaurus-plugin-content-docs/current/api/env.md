---
sidebar_position: 1
toc_max_heading_level: 3
---

# 协程环境

### 概念

Photon Env 包含了多种事件引擎、IO引擎，以及一个在用户态模拟的协程栈。

### 命名空间

`photon::`

### 头文件

`<photon/photon.h>`

## API

### init

```cpp
int photon::init(uint64_t event_engine = INIT_EVENT_DEFAULT, uint64_t io_engine = INIT_IO_DEFAULT);
```

#### 描述

在每个线程最开始的地方，我们都应该使用这个初始化函数，它的作用是在当前线程（ [vCPU](vcpu-and-multicore) ）初始化协程环境。
接下来你可以使用 `thread_create` 来创建协程（[threads](thread)）了，或者把它们迁移到其他 vCPUs。
从现在开始，你不应该再调用阻塞的函数或者系统调用。

`event_engine` 的选择跟平台相关，它跟调度器协作，并且决定了 poll fd 和处理事件的方式。
通常我们会在一个协程上执行非阻塞的 `wait` 以便等待事件，这样会让该协程进入 `SLEEPING` 状态（睡眠）。
当事件被处理完之后，`event_engine` 还要负责去唤醒睡眠的协程，让它进入 `READY` 状态。

`io_engine` 是用来创建辅助的IO协程的，如果需要的话，它会把这些辅助协程创建在后台持续执行。

#### 参数

- `event_engine` 支持的类型有
	
	- `INIT_EVENT_NONE` 空，只用于测试
	- `INIT_EVENT_DEFAULT` 默认值，Linux下为epoll，macOS下为kqueue
	- `INIT_EVENT_EPOLL` epoll
	- `INIT_EVENT_IOURING` io_uring
	- `INIT_EVENT_KQUEUE` kqueue


- `io_engine` 支持的类型有

	- `INIT_IO_NONE` 不需要辅助 IO 引擎。只使用 `libc` 的 read/write, 或者 `io_uring` 的原生IO栈（如果事件引擎设置了 io_uring 的话）
	- `INIT_IO_LIBAIO` AIO
	- `INIT_IO_LIBCURL` libcurl for HTTP
	- `INIT_IO_SOCKET_EDGE_TRIGGER` 边缘触发
	- `INIT_IO_EXPORTFS`
	- `INIT_IO_FSTACK_DPDK` 

:::info
`IOURING` 事件引擎需要Linux内核版本大于5.8。我们建议您升级到最新内核版本，以便体验io_uring的极致性能。
:::

#### 返回值

0成功，-1失败

----

### fini

```cpp
int photon::fini();
```

#### Description

停止所有的辅助IO协程，关闭事件引擎，销毁协程栈释放内存。

#### Parameters

无

#### Return

0成功，-1失败