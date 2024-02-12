---
sidebar_position: 4
toc_max_heading_level: 4
---

# 锁和同步原语

- 在同一个线程中的多个协程，彼此之间没有可见性问题。例如他们可能会修改线程内部的某个变量，修改动作本身不需要使用atomic，不需要关注memory order。
- 但同步原语（sync primitives）仍然是需要的，如在一个长的时间段内使用锁保护变量不被其他的协程修改，因为锁的持有者可能会让出CPU。
- 所有的协程同步原语都是支持跨线程使用的（也包括之前介绍的`thread_interrupt`唤醒操作）。

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
    // 针对不需要跨vCPU使用的场景，不加锁可以提升性能
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