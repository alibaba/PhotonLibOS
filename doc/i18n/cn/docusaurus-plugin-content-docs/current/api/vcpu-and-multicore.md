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

```cpp
new std::thread([&]{
	photon::init();
	DEFER(photon::fini());
});
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

##### Get vCPU number

```cpp
int get_vcpu_num();
```

:::note
只包含 WorkPool 的 vCPU 数量，主 OS 线程的 vCPU 不算。
:::

##### Thread Migrate

```cpp
int thread_migrate(photon::thread* th = CURRENT, size_t index = -1UL);
```

- `th` 需要迁移的协程
- `index` 目标 vCPU 的 index。如果 index 不在 [0, vcpu_num) 范围内，将使用 round-robin 的方式选择下一个 vCPU。
     
返回0表示成功， 小于0表示失败。