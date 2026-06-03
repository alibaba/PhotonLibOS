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
    int lock(Timeout timeout = {});
    int try_lock();
    void unlock();
}
```

:::note
`photon::Timeout`的默认值是-1UL (microseconds)，即永不超时
:::

```cpp
// 对于seq_mutex, 协程抢锁时如果有竞争，会按照FIFO顺序拿到锁
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
    // 针对不需要跨vCPU使用的场景，不加锁可以提升性能
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
     * @brief 不会被打断的wait
     */
    int wait(uint64_t count, Timeout timeout = {});
    /**
     * @brief 减去count
     * @return 1) count被成功减去（可能经过了等待）。返回0
     *         2) count不足，直到超时。返回-1，errno被设置成`ETIMEDOUT`
     *         3) 在超时前被其他协程唤醒。返回-1，errno由负责唤醒的协程决定
     */
    int wait_interruptible(uint64_t count, Timeout timeout = {});
    /**
     * @brief 增加count。不依赖Photon环境，可以在任何std::thread里面调用
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

## 全部同步原语

| 原语 | 说明 |
|------|------|
| `spinlock` | 指数退避 |
| `ticket_spinlock` | FIFO 顺序，无饥饿 |
| `mutex` | 两阶段：CAS → yield 重试 → 配合 `thread_usleep_defer` 的等待队列 |
| `recursive_mutex` | 可重入 mutex |
| `seq_mutex` | FIFO mutex |
| `condition_variable` | 扩展 `waitq`，支持基于谓词的等待 |
| `semaphore` | 计数信号量，支持乱序恢复选项 |
| `rwlock` | 传统读写锁 |
| `qrwlock` | 针对读多场景优化 |
| `locker<M>` / `SCOPED_LOCK` | 任意锁类型的 RAII 守卫 |

## 实现细节

### mutex

`photon::mutex` 使用两阶段协议：

1. 快路径：尝试 `CAS` 获取锁，不让出。
2. 竞争路径：多次 `thread_yield()` 重试。
3. 仍竞争：把当前协程推入等待队列，调用 `thread_usleep_defer()`，在不需要额外上下文切换的情况下释放内部 spinlock。

### 基于 defer 的睡眠时解锁

`thread_usleep_defer(timeout, defer, arg)` 在上下文切换后立即执行 `defer` 回调。这正是 `mutex::unlock()` 能在协程进入睡眠时释放内部 spinlock 的原因 —— 不需要经过调度器的额外往返。

### 非对称 spinlock

调度器内部使用的 spinlock 针对两种场景做了优化："前台 vCPU 访问自己的 runq"（单次原子 store）与"后台 vCPU 尝试迁入协程"（必须检查两个 flag）。

### 工作窃取

可选的跨 vCPU 负载平衡，通过 flag `VCPU_ENABLE_ACTIVE_WORK_STEALING` 和 `VCPU_ENABLE_PASSIVE_WORK_STEALING` 启用。窃取者扫描其他 vCPU 的 standby 队列和 run 队列。