# Thread lib

> In the photon world, we just call coroutine thread.

## Core API

### Thread (thread/thread.h)
```cpp
namespace photon {
    // Setup and destroy the photon thread environment.
    // Should be called right after a native OS thread is created, for instance, std::thread or pthread.
    int init();
    int fini();
    
    // A pointer refers to the CURRENT RUNNING photon thread.
    thread* CURRENT;
    
    // CLOCK_REALTIME from the Epoch, in microseconds
    uint64_t now;       

    // Get thread state
    enum states {
        READY = 0,      // Ready to run
        RUNNING = 1,    // Only one thread could be running at a time
        SLEEPING = 2,   // Sleeping and waiting for events
        DONE = 4,       // Have finished the whole life-cycle
        STANDBY = 8,    // Internally used by across-vcpu scheduling
    };
    states thread_stat(thread* th = CURRENT);

    // Create a thread to run the `thread_entry` function
    typedef void* (*thread_entry)(void*);
    const uint64_t DEFAULT_STACK_SIZE = 8 * 1024 * 1024;
    thread* thread_create(thread_entry func, void* arg, uint64_t stack_size = DEFAULT_STACK_SIZE);

    // Join threads
    join_handle* thread_enable_join(thread* th);
    void thread_join(join_handle* jh);

    // Manually switch to other threads 
    void thread_yield();
    void thread_yield_to(thread* th);

    // Sleep CURRENT thread for a specified time duration. 
    // Unlike the glibc sleep, it is a non-blocking implementation.
    // It will switch context to another READY thread, and awake SLEEPING threads if needed.
    int thread_usleep(uint64_t useconds);
    int thread_sleep(uint64_t seconds);
    
    // Interrupt a SLEEPING thread. After awakening, the target thread turns into READY.
    void thread_interrupt(thread* th, int error_number = EINTR);
}
```

### C++ 11 modern syntax (thread/thread11.h)
```cpp
// 1. Create a thread to run a global function with arbitrary arguments
int func(int, char) { return 0; }
photon::thread_create11(func, 1, 'a');

// 2. Create a thread to run class member function
class A {
public:
    void caller() {
        photon::thread_create11(&A::member_func, this, 1, 'a');
    }
private:
    void* member_func(int, char) { return nullptr; }
}

// 3. Create a thread to run lambda
auto f = [&](int, char) -> int { return 0; };
photon::thread_create11(f, 1, 'a');
```

### Timer (thread/timer.h)
```cpp
namespace photon {
    using Entry = Delegate<uint64_t>;
    // Create a timer object with `default_timedout` in usec, callback function `on_timer`,
    // and callback argument `arg`. The timer object is implemented as a special thread, so
    // it has a `stack_size`, and the `on_timer` is invoked within the thread's context.
    // The timer object is deleted automatically after it is finished.
    Timer(uint64_t default_timeout, Entry on_timer, bool repeating = true, uint64_t stack_size = 1024 * 64);

    int Timer::cancel();
    int Timer::reset(uint64_t new_timeout = -1);
    int Timer::stop();
}
```

### Synchronization (thread/thread.h)
```cpp
namespace photon {
    // Mutex
    class mutex {
    public:
        int lock(uint64_t timeout = -1);
        int try_lock();
        void unlock();
    };

    // RAII lock
    class scoped_lock {
    public:
        explicit scoped_lock(mutex& m, uint64_t do_lock = 2);
        int lock(bool must_lock = true);
        int try_lock();
        bool locked();
        void unlock();
    };

    // Condition variable
    class condition_variable {
    public:
        int wait(mutex& m, uint64_t timeout = -1);
        int wait_no_lock(uint64_t timeout = -1);
        void notify_one();
        void notify_all();
    };

    // Semaphore
    class semaphore {
    public:
        explicit semaphore(uint64_t count);
        int wait(uint64_t count, uint64_t timeout = -1);
        int signal(uint64_t count);
    };
}
```

### Thread pool（thread/thread-pool.h）
The thread pool's implementation is based on identity-pool.h. It supports both static allocation (on stack) and dynamic allocation (on heap).
```cpp
// on heap
auto p1 = photon::ThreadPoolBase::new_thread_pool(100);
auto th1 = p1->thread_create(&func, nullptr);

// on stack
photon::ThreadPool<400> p2;
auto th2 = p2.thread_create(&func, nullptr);
```
