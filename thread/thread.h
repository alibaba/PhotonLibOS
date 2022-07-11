/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once
#include <cinttypes>
#include <cassert>
#include <cerrno>
#include <atomic>
#include <type_traits>

#include <photon/common/callback.h>

namespace photon
{
    int thread_init();
    int thread_fini();
    int wait_all();

    struct thread;
    extern "C" __thread thread* CURRENT;
    extern uint64_t now;

    enum states
    {
        READY = 0,      // Ready to run
        RUNNING = 1,    // Only one thread could be running at a time
        SLEEPING = 2,   // Sleeping and waiting for events
        DONE = 4,       // Have finished the whole life-cycle
        STANDBY = 8,    // Internally used by across-vcpu scheduling
    };

    typedef void* (*thread_entry)(void*);
    typedef void (*defer_func)(void*);
    const uint64_t DEFAULT_STACK_SIZE = 8 * 1024 * 1024;
    thread* thread_create(thread_entry start, void* arg,
                          uint64_t stack_size = DEFAULT_STACK_SIZE);

    // Threads are join-able *only* through their join_handle.
    // Once join is enabled, the thread will remain existing until being joined.
    // Failing to do so will cause resource leak.
    struct join_handle;
    join_handle* thread_enable_join(thread* th, bool flag = true);
    void thread_join(join_handle* jh);

    // switching to other threads (without going into sleep queue)
    void thread_yield();

    // switching to a specific thread, which must be RUNNING
    void thread_yield_to(thread* th);

    // suspend CURRENT thread for specified time duration, and switch
    // control to other threads, resuming possible sleepers.
    // Return 0 if timeout, return -1 if interrupted, and errno is set by the interrupt invoker.
    int thread_usleep(uint64_t useconds);
    // thread_usleep_defer sets a callback, and will execute callback in another photon thread
    // after this photon thread fall in sleep. The defer function should NEVER fall into sleep!
    int thread_usleep_defer(uint64_t useconds, defer_func defer, void* defer_arg=nullptr);
    inline int thread_sleep(uint64_t seconds)
    {
        const uint64_t max_seconds = ((uint64_t)-1) / 1000 / 1000;
        uint64_t usec = (seconds >= max_seconds ? -1 :
                        seconds * 1000 * 1000);
        return thread_usleep(usec);
    }

    inline void thread_suspend()
    {
        thread_usleep(-1);
    }

    states thread_stat(thread* th = CURRENT);
    void thread_interrupt(thread* th, int error_number = EINTR);
    // extern "C" inline void safe_thread_interrupt(thread* th, int error_number = EINTR, int mode = 0)
    // {
    //     thread_interrupt(th, error_number);
    // }
    inline void thread_resume(thread* th)
    {
        thread_interrupt(th, 0);
    }

    // if true, the thread `th` should cancel what is doing, and quit
    // current job ASAP (not allowed `th` to sleep or block more than
    // 10ms, otherwise -1 will be returned to `th` and errno == EPERM;
    // if it is currently sleeping or blocking, it is thread_interupt()ed
    // with EPERM)
    int thread_shutdown(thread* th, bool flag = true);

    class MasterEventEngine;
    struct vcpu_base {
        MasterEventEngine* master_event_engine;
        std::atomic<uint32_t> nthreads;
        uint32_t id;
        volatile uint64_t switch_count;
    };

    // A helper struct in order to make some function calls inline.
    // The memory layout of its first 4 fields is the same as the one of thread.
    struct partial_thread
    {
        uint64_t _, __;
        vcpu_base* vcpu;
        void* _thread_local;
        // ...
    };

    // the getter and setter of thread-local variable
    // getting and setting local in a timer context will cause undefined behavior!
    inline void* thread_get_local()
    {
        return ((partial_thread*)CURRENT) -> _thread_local;
    }
    inline void thread_set_local(void* local)
    {
        ((partial_thread*)CURRENT) -> _thread_local = local;
    }
    inline vcpu_base* get_vcpu(thread* th = CURRENT)
    {
        return ((partial_thread*)th) -> vcpu;
    }

    /**
     * @brief Clear unused stack.
     * if target is a ready or sleeped photon thread, clear will not perform thread switch;
     * if target is CURRENT(by default), it will perform a thread switch.
     * do not support clear thread stack that is in running state on other vcpus.
     *
     * @param th photon thread
     * @return int
     */
    int stack_pages_gc(thread* th = CURRENT);

    /**
     * @brief Migrate a READY state thread to another vcpu
     *
     * @param th photon thead
     * @param vcpu target vcpu ptr, if `vcpu` is nullptr, th will be migrated to
     * unspecified vcpu
     * @return int 0 for success and -1 for failure
     */
    int thread_migrate(thread* th, vcpu_base* vcpu);

    class spinlock {
    public:
        int lock();
        int try_lock();
        void unlock();
    protected:
        std::atomic_bool _lock = {false};
    };

    class ticket_spinlock {
    public:
        int lock();
        int try_lock();
        void unlock();
    protected:
        std::atomic_size_t serv = {0};
        std::atomic_size_t next = {0};
    };

    class waitq
    {
    protected:
        int wait(uint64_t timeout = -1);
        int wait_defer(uint64_t timeout, void(*defer)(void*), void* arg);
        void resume(thread* th, int error_number = ECANCELED);  // `th` must be waiting in this waitq!
        int resume_all(int error_number = ECANCELED);
        thread* resume_one(int error_number = ECANCELED);
        waitq() = default;
        waitq(const waitq& rhs) = delete;   // not allowed to copy construct
        waitq(waitq&& rhs) = delete;
        void operator = (const waitq&) = delete;
        void operator = (waitq&&) = delete;
        ~waitq() { assert(q.th == nullptr); }

    protected:
        struct {
            thread* th = nullptr;           // the first thread in queue, if any
            spinlock lock;
            operator bool () { return th; }
        } q;
    };

    class mutex : protected waitq
    {
    public:
        int lock(uint64_t timeout = -1);        // threads are guaranteed to get the lock
        int try_lock();                         // in FIFO order, when there's contention
        void unlock();
        ~mutex()
        {
            assert(owner == nullptr);
        }

    protected:
        std::atomic<thread*> owner{nullptr};
        spinlock splock;
    };

    class recursive_mutex : protected mutex {
    public:
        int lock(uint64_t timeout = -1);
        int try_lock();
        void unlock();
    protected:
        int32_t recursive_count = 0;
    };

    template<typename M0>
    class locker
    {
    public:
        using M1 = typename std::decay<M0>::type;
        using M  = typename std::remove_pointer<M1>::type;

        // do lock() if `do_lock` > 0, and lock() can NOT fail if `do_lock` > 1
        explicit locker(M* mutex, uint64_t do_lock = 2) : m_mutex(mutex)
        {
            if (do_lock > 0) {
                lock(do_lock > 1);
            } else {
                m_locked = false;
            }
        }
        explicit locker(M& mutex, uint64_t do_lock = 2) : locker(&mutex, do_lock) { }
        locker(locker&& rhs) : m_mutex(rhs.m_mutex)
        {
            m_locked = rhs.m_locked;
            rhs.m_mutex = nullptr;
            rhs.m_locked = false;
        }
        locker(const locker& rhs) = delete;
        int lock(bool must_lock = true)
        {
            int ret; do
            {
                ret = m_mutex->lock();
                m_locked = (ret == 0);
            } while (!m_locked && must_lock);
            return ret;
        }
        int try_lock(){
            auto ret = m_mutex->try_lock();
            m_locked = (ret == 0);
            return ret;
        };
        bool locked()
        {
            return m_locked;
        }
        operator bool()
        {
            return locked();
        }
        void unlock()
        {
            if (m_locked)
            {
                m_mutex->unlock();
                m_locked = false;
            }
        }
        ~locker()
        {
            if (m_locked)
                m_mutex->unlock();
        }
        void operator = (const locker& rhs) = delete;
        void operator = (locker&& rhs) = delete;

        M* m_mutex;
        bool m_locked;
    };

    using scoped_lock = locker<mutex>;

    #define _TOKEN_CONCAT(a, b) a ## b
    #define _TOKEN_CONCAT_(a, b) _TOKEN_CONCAT(a, b)
    #define SCOPED_LOCK(x, ...) locker<decltype(x)> \
        _TOKEN_CONCAT_(__locker__, __LINE__) (x, ##__VA_ARGS__)

    class condition_variable : protected waitq
    {
    public:
        int wait(mutex* m, uint64_t timeout = -1);
        int wait(mutex& m, uint64_t timeout = -1)
        {
            return wait(&m, timeout);
        }
        int wait(spinlock* m, uint64_t timeout = -1);
        int wait(spinlock& m, uint64_t timeout = -1)
        {
            return wait(&m, timeout);
        }
        int wait(scoped_lock& lock, uint64_t timeout = -1)
        {
            return wait(lock.m_mutex, timeout);
        }
        int wait_no_lock(uint64_t timeout = -1)
        {
            return waitq::wait(timeout);
        }
        thread* signal()     { return resume_one(); }
        thread* notify_one() { return resume_one(); }
        int notify_all()     { return resume_all(); }
        int broadcast()      { return resume_all(); }
    };

    class semaphore : protected waitq
    {
    public:
        explicit semaphore(uint64_t count = 0) : m_count(count) { }
        int wait(uint64_t count, uint64_t timeout = -1);
        int signal(uint64_t count)
        {
            SCOPED_LOCK(splock);
            m_count.fetch_add(count);
            resume_one();
            return 0;
        }
        uint64_t count() const {
            return m_count.load(std::memory_order_relaxed);
        }

    protected:
        std::atomic<uint64_t> m_count;
        spinlock splock;
        bool try_substract(uint64_t count);
        void try_resume();
    };

    // to be different to timer flags
    // mark flag should be larger than 999, and not touch lower bits
    // here we selected
    constexpr int RLOCK=0x1000;
    constexpr int WLOCK=0x2000;
    class rwlock
    {
    public:
        int lock(int mode, uint64_t timeout = -1);
        int unlock();
    protected:
        int64_t state = 0;
        condition_variable cvar;
        mutex mtx;
    };

    class scoped_rwlock
    {
    public:
        scoped_rwlock(rwlock &rwlock, int lockmode) : m_locked(false) {
            m_rwlock = &rwlock;
            m_locked = (0 == m_rwlock->lock(lockmode));
        }
        bool locked() {
            return m_locked;
        }
        operator bool() {
            return locked();
        }
        int lock(int mode, bool must_lock = true) {
            int ret; do
            {
                ret = m_rwlock->lock(mode);
                m_locked = (0 == ret);
            } while (!m_locked && must_lock);
            // return 0 for locked else return -1 means failure
            return ret;
        }
        ~scoped_rwlock() {
            // only unlock when it is actually locked
            if (m_locked)
                m_rwlock->unlock();
        }
    protected:
        rwlock* m_rwlock;
        bool m_locked;
    };

    // create `n` threads to run `start(arg)`, then get joined
    void threads_create_join(uint64_t n, thread_entry start, void* arg,
                          uint64_t stack_size = DEFAULT_STACK_SIZE);

    bool is_master_event_engine_default();
    void reset_master_event_engine_default();

    // Saturating addition, primarily for timeout caculation
    __attribute__((always_inline)) inline uint64_t sat_add(uint64_t x,
                                                           uint64_t y) {
#if defined(__x86_64__)
      register uint64_t z asm("rax");
      asm("add %2, %1; sbb %0, %0; or %1, %0;"
          : "=r"(z), "+r"(x)
          : "r"(y)
          : "cc");
      return z;
#elif defined(__aarch64__)
      return (x + y < x) ? -1UL : x + y;
#endif
    }

    // Saturating subtract, primarily for timeout caculation
    __attribute__((always_inline)) inline uint64_t sat_sub(uint64_t x,
                                                           uint64_t y) {
#if defined(__x86_64__)
      register uint64_t z asm("rax");
      asm("xor %0, %0; subq %2, %1; cmovaeq %1, %0;"
          : "=r"(z), "+r"(x), "+r"(y)
          :
          : "cc");
      return z;
#elif defined(__aarch64__)
      return x > y ? x - y : 0;
#endif
    }
};

/*
 WITH_LOCK(mutex)
 {
    ...
 }
*/
#define WITH_LOCK(mutex) if (auto __lock__ = scoped_lock(mutex))

#define SCOPE_MAKESURE_YIELD                                       \
    uint64_t __swc_##__LINE__ = get_vcpu(photon::CURRENT)->switch_count;   \
    DEFER(if (__swc_##__LINE__ == get_vcpu(photon::CURRENT)->switch_count) \
              photon::thread_yield(););
