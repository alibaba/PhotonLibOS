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
#include <photon/common/callback.h>
#include <photon/common/timeout.h>
#include <photon/thread/stack-allocator.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <type_traits>
#ifndef __aarch64__
#include <emmintrin.h>
#endif

namespace photon
{
    constexpr uint8_t  VCPU_ENABLE_ACTIVE_WORK_STEALING     = 1;    // allow this vCPU to steal work from other vCPUs
    constexpr uint8_t  VCPU_ENABLE_PASSIVE_WORK_STEALING    = 2;    // allow this vCPU to be stolen by other vCPUs
    constexpr uint32_t THREAD_JOINABLE                      = 1;    // allow this thread to be joined
    constexpr uint32_t THREAD_ENABLE_WORK_STEALING          = 2;    // allow this thread to be stolen by other vCPUs
    constexpr uint32_t THREAD_PAUSE_WORK_STEALING           = 4;    // temporarily pause work-stealing for a thread

    int vcpu_init(uint64_t flags = 0);
    int vcpu_fini();
    int wait_all();
    int timestamp_updater_init();
    int timestamp_updater_fini();

    struct thread;
    extern __thread thread* CURRENT;
    extern volatile uint64_t now;   // a coarse-grained timestamp in unit of us
    struct NowTime {
        uint64_t now, _sec_usec;
        uint32_t  sec() { return _sec_usec >> 32; }
        uint32_t usec() { auto p = (uint32_t*)&_sec_usec; return *p; }
    };
    NowTime __update_now();    // update `now`

    enum states
    {
        READY = 0,      // Ready to run
        RUNNING = 1,    // Only one thread for each VCPU could be running at a time
        SLEEPING = 2,   // Sleeping and waiting for events
        DONE = 4,       // Have finished the whole life-cycle
        STANDBY = 8,    // Internally used by across-vcpu scheduling
    };

    // Create a new thread with the entry point `start(arg)` and `stack_size`.
    // Reserved space can be used to passed large arguments to the new thread.
    typedef void* (*thread_entry)(void*);
    const uint64_t DEFAULT_STACK_SIZE = 8 * 1024 * 1024;
    // Thread stack size should be at least 16KB. The thread struct located at stack bottom,
    // and the mprotect page is located at stack top-end.
    // reserved_space must be <= stack_size / 2
    thread* thread_create(thread_entry start, void* arg,
        uint64_t stack_size = DEFAULT_STACK_SIZE,
        uint32_t reserved_space = 0, uint64_t flags = 0);

    // get the address of reserved space, which is right below the thread struct.
    template<typename T = void> inline
    T* thread_reserved_space(thread* th, uint64_t reserved_size = sizeof(T)) {
        return (T*)((char*)th - reserved_size);
    }

    // Threads are join-able *only* through their join_handle.
    // Once join is enabled, the thread will remain existing until being joined.
    // Failing to do so will cause resource leak.
    struct join_handle;
    join_handle* thread_enable_join(thread* th, bool flag = true);
    void* thread_join(join_handle* jh);

    // terminates CURRENT with return value `retval`
    void thread_exit(void* retval) __attribute__((noreturn));

    // switching to other threads (without going into sleep queue)
    // return error_number if interrupted during the rolling
    int thread_yield();

    // switching to a specific thread, which must be RUNNING
    // return error_number if interrupted during the rolling
    int thread_yield_to(thread* th);

    // suspend CURRENT thread for specified time duration, and switch
    // control to other threads, resuming possible sleepers.
    // Return 0 if timeout, return -1 if interrupted, and errno is set by the interrupt invoker.
    int thread_usleep(Timeout timeout);

    inline int thread_usleep(uint64_t useconds) {
        return thread_usleep(Timeout(useconds));
    }


    // thread_usleep_defer sets a callback, and will execute callback in another photon thread
    // after this photon thread fall in sleep. The defer function should NEVER fall into sleep!
    typedef void (*defer_func)(void*);
    int thread_usleep_defer(Timeout timeout, defer_func defer, void* defer_arg=nullptr);

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
    // if it is currently sleeping or blocking, it is thread_interrupt()ed
    // with EPERM)
    int thread_shutdown(thread* th, bool flag = true);

    class MasterEventEngine;
    struct vcpu_base {
        MasterEventEngine* master_event_engine;
        volatile uint64_t switch_count = 0;
    };

    // A helper struct in order to make some function calls inline.
    // The memory layout of its first 4 fields is the same as the one of thread.
    struct partial_thread {
        uint64_t _[2];
        volatile vcpu_base* vcpu;
        uint64_t __[3];
        uint32_t flags, ___[3];
        void* tls;
    };

    // this function doesn't affect whether WS is enabled or not for the thread
    inline void thread_pause_work_stealing(bool flag, thread* th = CURRENT) {
        auto& flags = ((partial_thread*)th)->flags;
        if (flag) {
            flags |= THREAD_PAUSE_WORK_STEALING;
        } else {
            flags &= ~THREAD_PAUSE_WORK_STEALING;
        }
    }
    #define SCOPED_PAUSE_WORK_STEALING               \
        thread_pause_work_stealing(true);            \
        DEFER(thread_pause_work_stealing(false));

    inline vcpu_base* get_vcpu(thread* th = CURRENT) {
        auto vcpu = ((partial_thread*)th) -> vcpu;
        return (vcpu_base*)vcpu;
    }

    uint32_t get_vcpu_num();

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
     * @param vcpu target vcpu
     * @return int 0 for success and -1 for failure
     */
    int thread_migrate(thread* th, vcpu_base* vcpu);

    inline void spin_wait() {
#ifdef __aarch64__
        asm volatile("isb" : : : "memory");
#else
        _mm_pause();
#endif
    }

    class spinlock {
    public:
        int lock() {
            while (unlikely(xchg())) {
                while (likely(load())) {
                    spin_wait();
                }
            }
            return 0;
        }
        int try_lock() {
            return (likely(!load()) &&
                    likely(!xchg())) ? 0 : -1;
        }
        void unlock() {
            _lock.store(false, std::memory_order_release);
        }
    protected:
        std::atomic_bool _lock = {false};
        bool xchg() {
            return _lock.exchange(true, std::memory_order_acquire);
        }
        bool load() {
            return _lock.load(std::memory_order_relaxed);
        }
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
        int wait(Timeout timeout = {});
        int wait_defer(Timeout Timeout, void(*defer)(void*), void* arg);
        void resume(thread* th, int error_number = -1);  // `th` must be waiting in this waitq!
        int resume_all(int error_number = -1);
        thread* resume_one(int error_number = -1);
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
        mutex(uint16_t max_retries = 100) : retries(max_retries) { }
        int lock(Timeout timeout = {});
        int try_lock();
        void unlock();
        ~mutex()
        {
            assert(owner == nullptr);
        }

    protected:
        std::atomic<thread*> owner{nullptr};
        uint16_t retries;
        spinlock splock;
    };

    class seq_mutex : protected mutex {
    public:
        // threads are guaranteed to get the lock in sequental order (FIFO)
        seq_mutex() : mutex(0) { }
    };

    class recursive_mutex : protected mutex {
    public:
        using mutex::mutex;
        int lock(Timeout timeout = {});
        int try_lock();
        void unlock();
    protected:
        int32_t recursive_count = 0;
    };

    template <typename M>
    class locker
    {
    public:

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

#if __cpp_deduction_guides >= 201606
    #define SCOPED_LOCK(x, ...) photon::locker \
        _TOKEN_CONCAT_(__locker__, __LINE__) (x, ##__VA_ARGS__)
#else
    #define SCOPED_LOCK(x, ...) photon::locker<std::remove_pointer_t<std::decay_t<decltype(x)>>> \
        _TOKEN_CONCAT_(__locker__, __LINE__) (x, ##__VA_ARGS__)
#endif

    class condition_variable : protected waitq
    {
    public:
        int wait(mutex* m, Timeout timeout = {});
        int wait(mutex& m, Timeout timeout = {})
        {
            return wait(&m, timeout);
        }
        int wait(spinlock* m, Timeout timeout = {});
        int wait(spinlock& m, Timeout timeout = {})
        {
            return wait(&m, timeout);
        }
        int wait(scoped_lock& lock, Timeout timeout = {})
        {
            return wait(lock.m_mutex, timeout);
        }
        int wait_no_lock(Timeout timeout = {})
        {
            return waitq::wait(timeout);
        }
        template <typename LOCK, typename PRED,
                  typename = decltype(std::declval<PRED>()())>
        int wait(LOCK&& lock, PRED&& pred, Timeout timeout = {}) {
            return do_wait_pred(
                [&] { return wait(std::forward<LOCK>(lock), timeout); },
                std::forward<PRED>(pred), timeout);
        }
        template <typename PRED,
                  typename = decltype(std::declval<PRED>()())>
        int wait_no_lock(PRED&& pred, Timeout timeout = {}) {
            return do_wait_pred(
                [&] { return wait_no_lock(timeout); },
                std::forward<PRED>(pred), timeout);
        }
        thread* signal()     { return resume_one(); }
        thread* notify_one() { return resume_one(); }
        int notify_all()     { return resume_all(); }
        int broadcast()      { return resume_all(); }
    protected:
        template<typename DO_WAIT, typename PRED>
        int do_wait_pred(DO_WAIT&& do_wait, PRED&& pred, Timeout timeout) {
            int ret = 0;
            int err = ETIMEDOUT;
            while (!pred() && !timeout.expired()) {
                ret = do_wait();
                err = errno;
            }
            errno = err;
            return ret;
        }
    };

    class semaphore : protected waitq
    {
    public:
        explicit semaphore(uint64_t count = 0, bool in_order_resume = true)
            : m_count(count), m_ooo_resume(!in_order_resume) { }
        int wait(uint64_t count, Timeout timeout = {}) {
            int ret = 0;
            do {
                ret = wait_interruptible(count, timeout);
            } while (ret < 0 && (errno != ESHUTDOWN && errno != ETIMEDOUT));
            return ret;
        }
        int wait_interruptible(uint64_t count, Timeout timeout = {});
        int signal(uint64_t count) {
            if (count == 0) return 0;
            SCOPED_LOCK(splock);
            auto cnt = m_count.fetch_add(count) + count;
            try_resume(cnt);
            return 0;
        }
        uint64_t count() const {
            return m_count.load(std::memory_order_relaxed);
        }

    protected:
        std::atomic<uint64_t> m_count;
        bool m_ooo_resume;
        spinlock splock;
        bool try_subtract(uint64_t count);
        void try_resume(uint64_t count);
    };

    // to be different to timer flags
    // mark flag should be larger than 999, and not touch lower bits
    // here we selected
    constexpr int RLOCK=0x1000;
    constexpr int WLOCK=0x2000;
    class rwlock
    {
    public:
        int lock(int mode, Timeout timeout = {});
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

    // alloc space on rear end of current thread stack,
    // helps allocating when using hybrid C++20 style coroutine
    void* stackful_malloc(size_t size);
    void stackful_free(void* ptr);
};

/*
 WITH_LOCK(mutex)
 {
    ...
 }
*/
#define WITH_LOCK(mutex) if (auto __lock__ = scoped_lock(mutex))

#define SCOPE_MAKESURE_YIELD                                               \
    uint64_t __swc_##__LINE__ = get_vcpu(photon::CURRENT)->switch_count;   \
    DEFER(if (__swc_##__LINE__ == get_vcpu(photon::CURRENT)->switch_count) \
              photon::thread_yield(););
