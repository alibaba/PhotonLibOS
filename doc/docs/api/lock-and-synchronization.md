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
    int lock(Timeout timeout = {});
    int try_lock();
    void unlock();
}
```

:::note
The default value of Timeout is -1UL (microseconds), which means forever.
:::

```cpp
// For seq_mutex, threads are guaranteed to get the lock in FIFO order, when there's contention
class seq_mutex : public mutex {
};
```

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
    int wait(mutex* m, Timeout timeout = {});
    int wait(mutex& m, Timeout timeout = {});

    int wait(spinlock* m, Timeout timeout = {});
    int wait(spinlock& m, Timeout timeout = {});

    int wait(scoped_lock& lock, Timeout timeout = {});
    int wait_no_lock(Timeout timeout = {});
    
    thread* notify_one();
    int notify_all();
};
```

#### semaphore

```cpp
class semaphore {
public:
    explicit semaphore(uint64_t count = 0);
    /**
     * @brief A wrapper of wait that cannot be interrupted
     */
    int wait(uint64_t count, Timeout timeout = {});
    /**
     * @brief Subtract count.
     * @return 1) Count is successfully subtracted (might have been waited). Returns 0.
     *         2) Count is not enough until timeout. Returns -1, errno is set to ETIMEDOUT.
     *         3) Interrupted by another thread before timeout. Returns -1, errno is decided by the interrupter.
     */
    int wait_interruptible(uint64_t count, Timeout timeout = {});
    /**
     * @brief Add count. Does not require Photon environment, can be invoked in any std thread.
     */
    int signal(uint64_t count);
    uint64_t count() const;
};
```

#### rwlock

```cpp
class rwlock {
public:
    int lock(int mode, Timeout timeout = {});	// mode: RLOCK / WLOCK
    int unlock();
};
```

## All Synchronization Primitives

| Primitive | Notes |
|-----------|-------|
| `spinlock` | Exponential backoff |
| `ticket_spinlock` | FIFO ordering, no starvation |
| `mutex` | Two-phase: CAS → yield retries → wait queue with `thread_usleep_defer` |
| `recursive_mutex` | Re-entrant mutex |
| `seq_mutex` | FIFO mutex |
| `condition_variable` | Extends `waitq`, supports predicate-based wait |
| `semaphore` | Counting, with out-of-order resume option |
| `rwlock` | Traditional reader-writer lock |
| `qrwlock` | Optimized for read-heavy workloads |
| `locker<M>` / `SCOPED_LOCK` | RAII lock guard for any lock type |

## Implementation Notes

### mutex

`photon::mutex` uses a two-phase protocol:

1. Fast path: try `CAS` to acquire the lock without yielding.
2. Contended path: retry a few times with `thread_yield()`.
3. If still contended: push the current thread onto a wait queue and call `thread_usleep_defer()` to release the spinlock without an extra context switch.

### Defer-based unlock on sleep

`thread_usleep_defer(timeout, defer, arg)` executes the `defer` callback immediately after the context switch. This is what lets `mutex::unlock()` release its internal spinlock while the thread is going to sleep — without an extra round-trip through the scheduler.

### Asymmetric spinlock

The spinlock used inside the scheduler is optimized for the foreground vCPU accessing its own run queue (single atomic store) versus a background vCPU trying to migrate threads in (must check both flags).

### Work stealing

Optional cross-vCPU load balancing, enabled via the flags `VCPU_ENABLE_ACTIVE_WORK_STEALING` and `VCPU_ENABLE_PASSIVE_WORK_STEALING`. Stealers scan other vCPUs' standby queue and run queue.
