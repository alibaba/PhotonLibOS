---
sidebar_position: 4
toc_max_heading_level: 4
---

# Lock and Synchronization

- Multiple coroutines in the same OS thread have no visibility issues with each other. 
For example, if multiple coroutines modify variables inside a thread at the same time, we don't need to use atomic 
variables, and there is no need to pay attention to memory order.

- But sync primitives are still needed, because locks are needed to protect variables from being modified by 
other coroutines, if the lock owner might have a chance to yield its CPU.

- All Photon's synchronization primitives are thead-safe, including the `thread_interrupt` API we introduced before.

### Namespace

`photon::`

### Headers

`<photon/thread/thread.h>`

### API

#### mutex

```cpp
class mutex {
public:
    int lock(uint64_t timeout = -1);	// threads are guaranteed to get the lock
    int try_lock();                    	// in FIFO order, when there's contention
    void unlock();
}
```

:::note
The timeout type is `uint64_t`. -1UL means forever.
:::

#### spinlock

```cpp
class spinlock {
public:
    int lock();
    int try_lock();
    void unlock();
};
```

#### scoped_lock

```cpp
using scoped_lock = locker<mutex>;
```

#### condition_variable

```cpp
class condition_variable {
public:
    int wait(mutex* m, uint64_t timeout = -1);
    int wait(mutex& m, uint64_t timeout = -1);

    int wait(spinlock* m, uint64_t timeout = -1);
    int wait(spinlock& m, uint64_t timeout = -1);

    int wait(scoped_lock& lock, uint64_t timeout = -1);
    int wait_no_lock(uint64_t timeout = -1);
    
    thread* notify_one();
    int notify_all();
};
```

#### semaphore

```cpp
class semaphore {
public:
    explicit semaphore(uint64_t count = 0);
    int wait(uint64_t count, uint64_t timeout = -1);
    int signal(uint64_t count);
    uint64_t count() const;
};
```

#### rwlock

```cpp
class rwlock {
public:
    int lock(int mode, uint64_t timeout = -1);	// mode: RLOCK / WLOCK
    int unlock();
};
```