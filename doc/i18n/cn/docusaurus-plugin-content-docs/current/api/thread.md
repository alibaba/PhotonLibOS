---
sidebar_position: 2
toc_max_heading_level: 3
---

# 协程thread

### 概念

- Photon thread 即是我们通常所说的协程（coroutine/fiber）。之所以使用 thread 命名而不使用后两者，是因为我们希望用户察觉不到Photon程序跟传统多线程程序的差异。

- thread 本质上就是一个函数，一个执行单元。

- thread 必须存在于 [vCPU](vcpu-and-multicore) 里。thread 可以在 vCPU 之间迁移。

<div><center>
<img src="/img/api/thread-vcpu.png" alt="thread-vcpu.png" width="500" />
</center>
</div>

### 命名空间

`photon::`

### 头文件

`<photon/thread/thread.h>`

### 类型

`photon::thread` → Photon协程实体

`photon::join_handle` → Join句柄，用于Join协程

`photon::vcpu_base` → vCPU实体的基类。目前不能修改 vCPU，这是不可变值。

## API

----

### thread_create

```cpp
photon::thread* photon::thread_create(thread_entry start, void* arg,
                                      uint64_t stack_size = DEFAULT_STACK_SIZE,
                                      uint16_t reserved_space = 0);
```

#### 描述

创建一个协程，把它放进 `RunQueue`。由调度器决定何时开始执行这个协程函数。

该API调用过程中没有发生协程切换，这意味着调用方会继续下一条语句的执行。

#### 参数

- `start` 协程函数入口
	> `thread_entry` 的类型是 `void* (*)(void*)`

- `arg` 协程函数的单参数，指针类型。

- `stack_size` 协程栈大小，默认8MB

- `reserved_space` 用于向新的协程栈上拷贝参数。默认0

:::caution

请确保你传入的参数跟函数有着相同的生命周期，而不是在函数开始执行之前就析构了。这样可能造成无效的内存访问。

如果无法保证，可以使用下文的 `thread_create11`，它能够将所有的参数复制到新的协程的函数栈上。
:::

:::caution
stack_size是指协程栈的大小，这里采用的Linux默认的函数栈大小，8MB。

注意不要导致栈溢出，Linux会在运行时报栈溢出错误，但Photon由于是在用户态模拟的栈，因此不会立刻检测到栈溢出错误，继续运行下去会导致内存被踩。
通常情况下，不写递归函数即是安全的。

虽然我们可以使用内核提供的mprotect等功能保护这个内存段，但是会影响性能，因此这里是让用户自己把握。
:::

:::info

每次创建新的协程都会调用malloc申请内存，因此频繁创建建议使用协程池 `thread-pool`，或者 `pooled_stack_allocator`。
关于OS实际使用的内存量，即RES，并不是malloc完之后立刻增长8MB，而是根据实际写入page的映射情况动态增长。

```cpp
int main() {
    photon::init();
    for (int i = 0; i < 2000; ++i) {
        photon::thread_create11([]{
            photon::thread_usleep(-1UL);
        });
    }
    photon::thread_usleep(-1UL);
}
```
如上文例子，创建了2000个协程都在sleep。进程虚拟内存16G，但物理内存只有24M。

![malloc](/img/api/malloc.png)

:::info

#### Return

新的 thread 的指针

----

### thread_create11

```cpp
template<typename F, typename... ARGUMENTS>
photon::thread* photon::thread_create11(F f, ARGUMENTS&& ...args);				// (1)

template<typename FUNCTOR, typename... ARGUMENTS>
photon::thread* photon::thread_create11(FUNCTOR&& f, ARGUMENTS&& ...args);		// (2)

template<typename CLASS, typename F, typename... ARGUMENTS>
photon::thread* photon::thread_create11(F f, CLASS* obj, ARGUMENTS&& ...args);	// (3)
```

#### 描述

这是 `thread_create` 在C++11语法下的封装，同时支持以拷贝的方式传入多个参数。

#### 头文件

`<photon/thread/thread11.h>`

#### 用法

thread_create11 有三种 API，对应不同场景。

1. F 是一个全局函数

```cpp
double func(int a, char b);

auto th = photon::thread_create11(func, 1, '2');
```

2. FUNCTOR 是一个 [function object](https://en.cppreference.com/w/cpp/utility/functional).

```cpp
int a = 1; char b = '2';

auto th = photon::thread_create11([&] {
	// Access a and b
});
```

3. CLASS 是一个类型，F 是它的成员函数

```cpp
class A {
	void func(int a, char b);

	void run() {
		photon::thread_create11(&A::func, this, 1, '2');
	}
};
```

:::note
参数以值传递的方式传给协程函数，如果需要传引用，请使用 `std::ref` 或者 `std::cref` 处理变量
:::

#### Return

新的 thread 的指针

----

### thread_enable_join

```cpp
photon::join_handle* photon::thread_enable_join(photon::thread* th, bool flag = true);
```

#### Description

- 新建的协程默认是**不用**Join的，退出时自动释放内存
- 但如果通过 `thread_enable_join` 开启了 join 却最终没有去执行 `thread_join`，则会产生资源泄露。
- 得到的 `join_handle` 指针是 `thread_join` 的参数。

#### Parameters

- `th` Thread 指针.

- `flag` Enable or disable. Defaults to true.

#### Return

`join_handle` 指针。

----

### thread_join

```cpp
void photon::thread_join(photon::join_handle* jh);
```

#### 描述

等待目标协程结束。

:::caution
你不能 join 一个已经退出的或者不存在的 thread，这将导致 core dump。
:::

#### 参数

- `jh`：`join_handle` 指针，从上文的 `thread_enable_join` 获得。

#### 返回值

无

----

### thread_yield

```cpp
void photon::thread_yield();
```

#### Description

切换到其他协程。本协程不会进入 `SleepQueue`，而是仍然在 `RunQueue` 上。

- 协程的调度有如下的特点：除非主动让出CPU，否则该协程函数会一直继续执行
- 协程的 `thread_yield` 由高性能汇编实现，切换开销在**10纳秒**左右
- `thread_yield` 只是若干种能够让出CPU的方法中的其中一种，除了它以外，还有下文将要介绍的`thread_usleep`，以及各种协程锁和同步原语。
- `thread_yield` 只负责中断当前函数的执行，具体下一个协程是哪个它并不知道，由调度器决定。在一段时间后，调度器可能决定又返回本函数，则沿着 `thread_yield` 的下一条语句继续执行。
- 普通用户用到yield的场景不太多，它一般出现在锁和同步原语的内部，或者一些偏底层的模块内。如果正确的话，用户可以假定他们使用的所有IO函数都是非阻塞的，即会在恰当的时候进行 yield。

#### Parameters

None

#### Return

None

----

### thread_usleep

```cpp
int photon::thread_usleep(uint64_t useconds);
```

#### Description

- 暂停当前协程（`CURRENT`）的执行，将其插入 `SleepQueue` 队列，睡眠指定时间（微秒）。最后 yield 让出 CPU。 

- 进入 `SleepQueue` 里的协程会在将来会被唤醒。

- 除了用户主动调用睡眠，所有的文件IO（psync除外）、网络收发、锁和同步原语的底层等待都是通过这个 `thread_usleep` 实现的。

#### Parameters

- `useconds` 睡眠时间（微秒）。-1UL 表示永久睡眠

#### Return

- 返回 0 表示成功，即至少已睡眠了 `useconds` 微秒的时间。
- 返回 -1 表示被提前唤醒了， `errno` 是由调用 `thread_interrupt` 的唤醒方设定的。

----

### thread_interrupt

```cpp
void photon::thread_interrupt(photon::thread* th, int error_number = EINTR);
```

#### 描述

打断目标协程，从睡眠中将其唤醒。

在协程场景下，将任务的提交、执行、收割分为前台和后台，是常见的一种设计模式。
例如我们可以在前台协程中将任务提交到某个队列后立刻睡眠，后台协程在检查到任务执行完之后，去唤醒前台协程继续执行。

:::caution
你不能 interrupt 一个已经退出的或者不存在的 thread，这将导致 core dump。
:::

#### 参数

- `th` 目标 thread.

- `error_number` 设置目标 thread 看到的 `errno`. 默认是 `EINTR`.

#### 返回值

无

#### 备注

调用方和目标协程可能不在同一个 vCPU 上，这种情况下，`StandbyQueue` 就会被使用到，作为跨线程交互手段。

----

### thread_shutdown

```cpp
int photon::thread_shutdown(photon::thread* th, bool flag = true);
```

#### Description

Set the shutdown flag to disable further scheduling of the target thread, then fire an interrupt.

#### Parameters

- `th` Target thread.

- `flag` Set the shutdown flag or not.

#### Return

None.

----

### thread_migrate

```cpp
int photon::thread_migrate(photon::thread* th, photon::vcpu_base* vcpu);
```

#### Description

把一个 `READY` 状态的 thread 迁移到另一个 vCPU.

- 虽然是M:1 模型，我们仍然可以将协程函数迁移到另一个线程上去执行。
- 优先使用run-to-completion模型，减少跨线程交互。一旦创建后，非必要不将协程调度到其他线程，除非遇到本线程的CPU瓶颈。

#### Parameters

- `th` Target thread.

- `vcpu` Target vCPU.

#### Return

Returns 0 on success, returns -1 on error.

----

### get_vcpu

```cpp
vcpu_base* get_vcpu(thread* th = CURRENT);
```

#### Description

Get the vCPU of the target thread.

#### Parameters

- `th` Target thread. Defaults to `CURRENT` thread.

#### Return

Returns a pointer of `vcpu_base`.

----

## 协程池

#### Additional Header

`<photon/thread/thread-pool.h>`

#### Description

The thread-pool's implementation is based on identity-pool.h. It supports both static allocation (on stack) and dynamic allocation (on heap).

```cpp
// on heap
auto p1 = photon::ThreadPoolBase::new_thread_pool(100);
auto th1 = p1->thread_create(&func, nullptr);

// on stack
photon::ThreadPool<400> p2;
auto th2 = p2.thread_create(&func, nullptr);
```

## 计时器

#### Additional Header

`<photon/thread/thread-pool.h>`

#### Description

Create a timer object with `default_timedout` in usec, callback function `on_timer`,
and callback argument `arg`. The timer object is implemented as a special thread, so
it has a `stack_size`, and the `on_timer` is invoked within the thread's context.
The timer object is deleted automatically after it is finished.	

一个 non-repeating 的计时器基本上等于创建一个新协程，并执行 thread_sleep。

```cpp
Timer(uint64_t default_timeout, Entry on_timer, 
	  bool repeating = true, uint64_t stack_size = 1024 * 64);

int Timer::cancel();
int Timer::reset(uint64_t new_timeout = -1);
int Timer::stop();
```