---
sidebar_position: 2
toc_max_heading_level: 3
---

# Thread

### Concept

- Photon thread is what we usually call coroutine/fiber. The reason why we use thread naming instead of the latter 
two is because we hope that users will not notice the difference between the Photon program and traditional
multi-threaded programs.

- A thread is essentially a function, and an execution unit.

- A thread must reside in one [vCPU](vcpu-and-multicore). Threads can be migrated between vCPUs.

<div><center>
<img src="/img/api/thread-vcpu.png" alt="thread-vcpu.png" width="500" />
</center>
</div>

### Namespace

`photon::`

### Headers

`<photon/thread/thread.h>`

### Types

`photon::thread` → The Photon's coroutine/fiber entity.

`photon::join_handle` → Join handle. Used to join a thread.

`photon::vcpu_base` → A base class for the vCPU entity. By far there is no way to modify a vCPU. It's immutable.

## Thread API

----

### thread_create

```cpp
photon::thread* photon::thread_create(thread_entry start, void* arg,
                                      uint64_t stack_size = DEFAULT_STACK_SIZE,
                                      uint16_t reserved_space = 0);
```

#### Description

Create a new thread, put it into the `RunQueue`. Leave it to the scheduler to decide when to run.

No thread yield in this call, which means the caller will continue its execution.

#### Parameters

- `start` Thread function's entry point.
	> `thread_entry` is a type of `void* (*)(void*)`

- `arg` Thread function's sole argument.

- `stack_size` Default stack size is 8MB.

- `reserved_space` Can be used to passed large arguments to the new thread. Default is 0.

:::caution
Please make sure that the parameters you pass in have the same life cycle as the function and are
not destroyed before the function starts executing. This may cause invalid memory access.

If this cannot be guaranteed, you can use `thread_create11` below, 
which can copy all parameters to the function stack of the new coroutine.
:::

:::caution
`stack_size` refers to the size of the coroutine stack, default is 8MB, same as Linux default function size.

Be careful not to cause a stack overflow. Linux will report a stack overflow error at runtime. However, 
since Photon simulates the stack in user mode, it will not detect a stack overflow error immediately.
Continuous access might cause the memory to be trampled. Normally, you should avoid write recursive functions.

Although we can use kernel's `mprotect` to guard this memory region, it will affect performance, so it is up to the user to control it.
:::

:::info

Every time a new coroutine is created, malloc is called to apply for memory. Therefore, it is recommended to use 
the `thread-pool` or `pooled_stack_allocator` for frequent creation.

In terms of the actual memory used by the OS, it does not add 8MB immediately for every malloc, 
but increases dynamically based on the mapping of the actual written page.

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

In the above example, we created 2000 sleeping coroutines, the process shows that it has 16GB virtual memory, but only
consumed 24MB physical memory.

![malloc](/img/api/malloc.png)

:::info

#### Return

Pointer to the new thread.

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

#### Description

C++11 syntax wrapper for `thread_create`. Multiple arguments is allowed.

#### Headers

`<photon/thread/thread11.h>`

#### Usage

1. F is a global function.

```cpp
double func(int a, char b);

auto th = photon::thread_create11(func, 1, '2');
```

2. FUNCTOR is a [function object](https://en.cppreference.com/w/cpp/utility/functional).

```cpp
int a = 1; char b = '2';

auto th = photon::thread_create11([&] {
	// Access a and b
});
```

3. CLASS is a type, and F is its member function

```cpp
class A {
	void func(int a, char b);

	void run() {
		photon::thread_create11(&A::func, this, 1, '2');
	}
};
```

:::note
Arguments are forwarded to the new thread by value. If needed to pass by reference, it has to be wrapped by `std::ref` or `std::cref`.
:::

#### Return

Pointer to the new thread.

----

### thread_enable_join

```cpp
photon::join_handle* photon::thread_enable_join(photon::thread* th, bool flag = true);
```

#### Description

- Join is disabled by default.

- Once join is enabled, the thread will remain existing until being joined.
Failing to do so will cause resource leak.

- Threads are join-able **only** through their `join_handle`.

#### Parameters

- `th` Thread pointer.

- `flag` Enable or disable. Defaults to true.

#### Return

Pointer of a `join_handle`.

----

### thread_join

```cpp
void photon::thread_join(photon::join_handle* jh);
```

#### Description

Join a thread.

:::caution
You can't join an exited or non-existent thread. It will cause core dump.
:::

#### Parameters

- `jh` Pointer of a `join_handle`. Get from `thread_enable_join`.

#### Return
None

----

### thread_yield

```cpp
void photon::thread_yield();
```

#### Description

Switching to other threads, without going into the `SleepQueue`.

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

Suspend `CURRENT` thread for specified time duration, insert self to the `SleepQueue`, and switch control to other running threads.

Threads in the `SleepQueue` might have a chance to be resumed then.

#### Parameters

- `useconds` Sleep time in microseconds. -1UL means forever sleep.

#### Return

- Returns 0 if slept well (at least `useconds` time).
- Returns -1 if been interrupted, and `errno` is set by the caller who invokes `thread_interrupt`.

----

### thread_interrupt

```cpp
void photon::thread_interrupt(photon::thread* th, int error_number = EINTR);
```

#### Description

Interrupt the target thread. Awaken it from sleep.

:::caution
You can't interrupt an exited or non-existent thread. It will cause core dump.
:::

#### Parameters

- `th` Target thread.

- `error_number` Set the `errno` of the target thread. Default is `EINTR`.

#### Return

None.

#### Note

The target thread and the caller might NOT belong to the same vCPU. In this situation, the `StandbyQueue` will be used.

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

Migrate a `READY` state thread to another vCPU.

- Although Photon is of M:1 model, we can still migrate the coroutine function to another thread.

- Run-to-completion model is prefer. Once created, do not schedule the coroutine to other threads unless you encounter a CPU bottleneck in this thread.

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

## Thread Pool

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

## Timer

#### Additional Header

`<photon/thread/thread-pool.h>`

#### Description

Create a timer object with `default_timedout` in usec, callback function `on_timer`,
and callback argument `arg`. The timer object is implemented as a special thread, so
it has a `stack_size`, and the `on_timer` is invoked within the thread's context.
The timer object is deleted automatically after it is finished.	

A non-repeating is basically equal to creating a new thread and run thread_usleep.

```cpp
Timer(uint64_t default_timeout, Entry on_timer, 
	  bool repeating = true, uint64_t stack_size = 1024 * 64);

int Timer::cancel();
int Timer::reset(uint64_t new_timeout = -1);
int Timer::stop();
```