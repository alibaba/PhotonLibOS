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