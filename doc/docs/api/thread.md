---
sidebar_position: 2
toc_max_heading_level: 3
---

# Thread

### Concept

Photon thread == general coroutine/fiber

A thread must reside in one [vCPU](vcpu-and-multicore).

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
photon::thread*
photon::thread_create(thread_entry start, void* arg, 
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
Make sure your argument has the same lifecycle as the thread. So there won't be invalid memory access.
:::

:::caution
Do not overflow your stack. Photon's stack is simulated in userspace. You won't get a notice when it's full.
:::

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

:::note
Arguments are forwarded to the new thread by value. If needed to pass by reference, it has to be wrapped by `std::ref` or `std::cref`.
:::

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

#### Return

Pointer to the new thread.

----

### thread_enable_join

```cpp
photon::join_handle* 
photon::thread_enable_join(photon::thread* th, bool flag = true);
```

#### Description

Threads are join-able **only** through their `join_handle`. Once join is enabled, the thread will remain existing until being joined.
Failing to do so will cause resource leak.

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

```cpp
Timer(uint64_t default_timeout, Entry on_timer, 
	  bool repeating = true, uint64_t stack_size = 1024 * 64);

int Timer::cancel();
int Timer::reset(uint64_t new_timeout = -1);
int Timer::stop();
```