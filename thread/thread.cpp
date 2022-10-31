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

#include <memory.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef __aarch64__
#include <emmintrin.h>
#endif

#include <cstddef>
#include <cassert>
#include <cerrno>
#include <vector>
#include <new>
#include <thread>
#include <mutex>
#include <condition_variable>

#define protected public
#include "thread.h"
#include "timer.h"
#include "list.h"
#undef protected

#include <photon/io/fd-events.h>
#include <photon/common/timeout.h>
#include <photon/common/alog.h>
#include <photon/thread/thread-key.h>

/* notes on the scheduler:

1. runq (denoted by CURRENT) and sleepq are compeltely private,
   i.e. they are *not* accessed by other vcpus;

2. accessing runq and sleepq doesn't need locking, but a thread
   needs locking in order to get in / out of runq, so as to prevent
   racing among thread_usleep() and thread_interrupt()s;

3. always lock struct thread before waitq, so as to avoid deadlock;

4. standbyq, all waitqs, and struct thread *may* be accessed by
   other vcpus at anytime;

5. for thread_interrupt()s that crosses vcpus, threads are pushed
   to standbyq (with locking, of course) of target vcpu, setting to
   state READY; they will be moved to runq (and popped from sleepq)
   by target vcpu in resume_thread(), when its runq becomes empty;
*/

#define SCOPED_MEMBER_LOCK(x) SCOPED_LOCK(&(x)->lock, ((bool)x) * 2)

static constexpr size_t PAGE_SIZE = 1 << 12;

namespace photon
{
    inline uint64_t min(uint64_t a, uint64_t b) { return (a<b) ? a : b; }
    class NullEventEngine : public MasterEventEngine {
    public:
        std::mutex _mutex;
        std::condition_variable _cvar;
        std::atomic_bool notify{false};
        int wait_for_fd(int fd, uint32_t interests, uint64_t timeout) override {
            return -1;
        }
        int cancel_wait() override {
            {
                std::unique_lock<std::mutex> lock(_mutex);
                notify.store(true, std::memory_order_release);
            }
            _cvar.notify_all();
            return 0;
        }
        ssize_t wait_and_fire_events(uint64_t timeout = -1) override {
            DEFER(notify.store(false, std::memory_order_release));
            if (!timeout) return 0;
            timeout = min(timeout, 1000 * 100UL);
            std::unique_lock<std::mutex> lock(_mutex);
            if (notify.load(std::memory_order_acquire)) {
                return 0;
            }
            _cvar.wait_for(lock, std::chrono::microseconds(timeout));
            return 0;
        }
    };

    struct vcpu_t;
    struct thread;
    class Stack
    {
    public:
        template<typename F>
        void init(void* ptr, F ret2func)
        {
            _ptr = ptr;
            #ifdef __x86_64__
            push(0);
            if ((uint64_t)_ptr % 16 == 0) push(0);
            push(ret2func);

            // make room for rbx, rbp, r12~r15
            (uint64_t*&)_ptr -= 6;
            #elif defined(__aarch64__)
            // make room for r19~r30
            (uint64_t*&)_ptr -= 12;
            push(ret2func);
            push(0);
            #endif
        }
        void** pointer_ref()
        {
            return &_ptr;
        }
        void push(uint64_t x)
        {
            *--(uint64_t*&)_ptr = x;
        }
        template<typename T>
        void push(const T& x)
        {
            push((uint64_t)x);
        }
        uint64_t pop()
        {
            return *((uint64_t*&)_ptr)++;
        }
        uint64_t& operator[](int i)
        {
            return static_cast<uint64_t*>(_ptr)[i];
        }
        void* _ptr;
    };

    struct thread_list;
    struct thread : public intrusive_list_node<thread>
    {
        volatile vcpu_t* vcpu;
        void* arg = nullptr;                /* will be used as thread local storage after thread started */
        states state = states::READY;
        int error_number = 0;
        int idx;                            /* index in the sleep queue array */
        int flags = 0;
        int reserved;
        bool joinable = false;
        bool shutting_down = false;         // the thread should cancel what is doing, and quit
                                            // current job ASAP; not allowed to sleep or block more
                                            // than 10ms, otherwise -1 will be returned and errno == EPERM

        spinlock lock;
        thread_list* waitq = nullptr;       /* the q if WAITING in a queue */

        thread_entry start;
        void* retval;
        void* go()
        {
            auto _arg = arg;
            arg = nullptr;
            return retval = start(_arg);
        }
        vcpu_t* get_vcpu(){
            return (vcpu_t*)vcpu;
        }
        char* buf;
        size_t stack_size;

        Stack stack;
        uint64_t ts_wakeup = 0;             /* Wakeup time when thread is sleeping */
        condition_variable cond;            /* used for join, or timer REUSE */

        int set_error_number()
        {
            if (error_number)
            {
                errno = error_number;
                error_number = 0;
                return -1;
            }
            return 0;
        }

        void dequeue_ready_atomic(states newstat = states::READY);

        bool operator < (const thread &rhs)
        {
            return this->ts_wakeup < rhs.ts_wakeup;
        }

        void dispose()
        {
            auto b = buf;
#ifndef __aarch64__
            madvise(b, stack_size, MADV_DONTNEED);
#endif
            free(b);
        }
    };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    static_assert(offsetof(thread, vcpu) == 16, "...");
    static_assert(offsetof(thread, arg)  == 24, "...");
#pragma GCC diagnostic pop

    struct thread_list : public intrusive_list<thread>
    {
        spinlock lock;
        thread_list() = default;
        thread_list(thread* head) {
            this->node = head;
        }
        thread* eject_whole_atomic() {
            SCOPED_LOCK(lock);
            auto p = node;
            node = nullptr;
            return p;
        }
    };

    class SleepQueue
    {
    public:
        std::vector<thread *> q;
        thread* front() const
        {
            assert(!q.empty());
            return q.front();
        }
        bool empty() const
        {
            return q.empty();
        }

        int push(thread *obj)
        {
            q.push_back(obj);
            obj->idx = q.size() - 1;
            up(obj->idx);
            return 0;
        }

        thread* pop_front()
        {
            auto ret = q[0];
            q[0] = q.back();
            q[0]->idx = 0;
            q.pop_back();
            down(0);
            ret->idx = -1;
            return ret;
        }

        int pop(thread *obj)
        {
            if (obj->idx == -1) return -1;
            if ((size_t)obj->idx == q.size() - 1){
                q.pop_back();
                obj->idx = -1;
                return 0;
            }

            auto id = obj->idx;
            q[obj->idx] = q.back();
            q[id]->idx = id;
            q.pop_back();
            if (!up(id)) down(id);
            obj->idx = -1;

            return 0;
        }

        void update_node(int idx, thread *&obj)
        {
            q[idx] = obj;
            q[idx]->idx = idx;
        }

        // compare m_nodes[idx] with parent node.
        bool up(int idx)
        {
            auto tmp = q[idx];
            bool ret = false;
            while (idx != 0){
                auto cmpIdx = (idx - 1) >> 1;
                if (*tmp < *q[cmpIdx]) {
                    update_node(idx, q[cmpIdx]);
                    idx = cmpIdx;
                    ret = true;
                    continue;
                }
                break;
            }
            if (ret) update_node(idx, tmp);
            return ret;
        }

        // compare m_nodes[idx] with child node.
        bool down(int idx)
        {
            auto tmp = q[idx];
            size_t cmpIdx = (idx << 1) + 1;
            bool ret = false;
            while (cmpIdx < q.size()) {
                if (cmpIdx + 1 < q.size() && *q[cmpIdx + 1] < *q[cmpIdx]) cmpIdx++;
                if (*q[cmpIdx] < *tmp){
                    update_node(idx, q[cmpIdx]);
                    idx = cmpIdx;
                    cmpIdx = (idx << 1) + 1;
                    ret = true;
                    continue;
                }
                break;
            }
            if (ret) update_node(idx, tmp);
            return ret;
        }
    };

    inline void spin_wait() {
#ifdef __aarch64__
        asm volatile("isb" : : : "memory");
#else
        _mm_pause();
#endif

    }

    // A special spinlock that distinguishes a foreground vCPU among
    // background vCPUs, and makes the foreground as fast as possible.
    // Generic spinlock uses atomic exchange to obtain the lock, which
    // depends on bus lock and costs much CPU cycles than this design.
    class asymmetric_spinLock {
        std::atomic_bool foreground_locked {false},
                         background_locked {false};

        void wait_while(std::atomic_bool& x) {
            while (unlikely(x.load(std::memory_order_acquire))) {
                do { spin_wait(); }
                while(likely(x.load(std::memory_order_relaxed)));
            }
        }

    public:
        void foreground_lock() {
            // lock
            foreground_locked.store(true, std::memory_order_release);

            // wait if (unlikely) background locked
            wait_while(background_locked);
        }
        bool background_try_lock() {
            while(true) {
                // wait if (unlikely) foreground locked
                wait_while(foreground_locked);

                // try lock
                if (background_locked.exchange(true, std::memory_order_acquire))
                    return false;   // avoid wait while holding the lock

                // check to make sure it is still unlocked
                if (likely(!foreground_locked.load(std::memory_order_acquire)))
                    return true;

                // otherwise release lock, wait, and repeat again
                background_locked.store(false, std::memory_order_release);
                spin_wait();
            }
            return true;
        }
        void foreground_unlock() {
            foreground_locked.store(false, std::memory_order_release);
        }
        void background_unlock() {
            background_locked.store(false, std::memory_order_release);
        }
    };

    struct vcpu_t : public vcpu_base
    {
        states state = states::READY;
        // standby queue stores the threads that are running, but not
        // yet added to the run queue, until the run queue becomes empty
        thread_list standbyq;
        template<typename T>
        void move_to_standbyq_atomic(T x)
        {
            _move_to_standbyq_atomic(x);
            master_event_engine->cancel_wait();
        }
        void _move_to_standbyq_atomic(thread_list* lst)
        {
            SCOPED_LOCK(standbyq.lock);
            auto head = lst->front();
            auto tail = lst->back();
            standbyq.push_back(std::move(*lst));
            for (auto th = head; th != tail; th = th->next()) {
                assert(this == th->vcpu);
                th->lock.unlock();
            }
            assert(this == tail->vcpu);
            tail->lock.unlock();
        }
        void _move_to_standbyq_atomic(thread* th)
        {
            assert(this == th->vcpu);
            SCOPED_LOCK(standbyq.lock);
            standbyq.push_back(th);
        }

        SleepQueue sleepq;

        thread* idle_worker;

        asymmetric_spinLock runq_lock;

        NullEventEngine _default_event_engine;
        vcpu_t() {
            state = states::READY;
            master_event_engine = &_default_event_engine;
        }
        bool is_master_event_engine_default() {
            return &_default_event_engine == master_event_engine;
        }
        void reset_master_event_engine_default() {
            auto& mee = master_event_engine;
            if (&_default_event_engine == mee) return;
            delete mee;
            mee = &_default_event_engine;
        }
    };

    #define SCOPED_FOREGROUND_LOCK(x) \
        auto __px = &(x); __px->foreground_lock(); DEFER(__px->foreground_unlock());
    #define SCOPED_BACKGROUND_LOCK(x) \
        (x).background_lock(); DEFER((x).background_unlock());

    struct Switch { thread *from, *to; };
    class AtomicRunQ : public intrusive_list<thread> {
    public:
        static void prefetch_context(thread* from, thread* to)
        {
#ifdef CONTEXT_PREFETCHING
            const int CACHE_LINE_SIZE = 64;
            auto f = *from->stack.pointer_ref();
            __builtin_prefetch(f, 1);
            __builtin_prefetch((char*)f + CACHE_LINE_SIZE, 1);
            auto t = *to->stack.pointer_ref();
            __builtin_prefetch(t, 0);
            __builtin_prefetch((char*)t + CACHE_LINE_SIZE, 0);
#endif
        }
        Switch remove_current(states new_state) {
            assert(!single());
            auto from = node;
            SCOPED_FOREGROUND_LOCK(node->get_vcpu()->runq_lock);
            auto to = node = node->remove_from_list();
            prefetch_context(from, to);
            from->state = new_state;
            to->state = states::RUNNING;
            return {from, to};
        }
        Switch _do_goto(thread* to) {
            auto from = node;
            prefetch_context(from, to);
            from->state = states::READY;
            to->state = states::RUNNING;
            node = to;
            return {from, to};
        }
        Switch goto_next() {
            assert(!single());
            SCOPED_FOREGROUND_LOCK(node->get_vcpu()->runq_lock);
            return _do_goto(node->next());
        }
        Switch try_goto(thread* th) {
            auto vcpu = node ->get_vcpu();
            SCOPED_FOREGROUND_LOCK(vcpu->runq_lock);
            if (unlikely(th->vcpu != vcpu)) {
                auto r = Switch{0, 0};
                LOG_ERROR_RETURN(EINVAL, r, "target thread ` must be run by the same vcpu as CURRENT!", th);
            }
            return _do_goto(th);
        }
        bool single() {
            SCOPED_FOREGROUND_LOCK(node->get_vcpu()->runq_lock);
            return node->single();
        }
        bool size_1or2() {
            SCOPED_FOREGROUND_LOCK(node->get_vcpu()->runq_lock);
            return node->next() == node->prev();
        }
        void insert_tail(thread* th) {
            SCOPED_FOREGROUND_LOCK(node->get_vcpu()->runq_lock);
            node->insert_tail(th);
        }
        void insert_list_before(thread* th) {
            SCOPED_FOREGROUND_LOCK(node->get_vcpu()->runq_lock);
            node->insert_list_before(th);
        }
        void remove_from_list(thread* th) {
            assert(th->state == states::READY);
            assert(th->vcpu == node->vcpu);
            SCOPED_FOREGROUND_LOCK(node->get_vcpu()->runq_lock);
            th->remove_from_list();
        }
        bool defer_to_new_thread() {
            auto vcpu = node->get_vcpu();
            auto idle_worker = vcpu->idle_worker;
            SCOPED_FOREGROUND_LOCK(vcpu->runq_lock);
            if (node->next() == idle_worker) {
                if (idle_worker->next() == node) {
                    // if defer_func is executed in idle_worker and it yields,
                    // photon will be broken. so we should return true and
                    // create a new thread to execute the defer func.
                    return true;
                }

                // postpone idle worker
                auto next = idle_worker->remove_from_list();
                next->insert_after(idle_worker);
            }
            return false;
        }
    };

    AtomicRunQ* atomic_runq() {
        return (AtomicRunQ*) &CURRENT;
    }

    inline void thread::dequeue_ready_atomic(states newstat)
    {
        assert("this is not in runq, and this->lock is locked");
        if (waitq) {
            assert(waitq->front());
            SCOPED_LOCK(waitq->lock);
            waitq->erase(this);
            waitq = nullptr;
        } else {
            assert(this->single());
        }
        state = newstat;
    }

    __thread thread* CURRENT;
    static void thread_die(thread* th)
    {
        // die with lock, so other context keeps safe
        assert(th->state == states::DONE);
        th->dispose();
    }

    static void spinlock_unlock(void* m_);
    inline void switch_context_defer(thread* from, thread* to,
        void(*defer)(void*), void* arg);

    void photon_switch_context_defer_die(thread* dying_th, void** dest_context,
        void(*th_die)(thread*)) asm ("_photon_switch_context_defer_die");

    extern void photon_switch_to(void**, void**) asm("_photon_switch_to");

    // Since thread may be moved from one vcpu to another
    // thread local CURRENT *** MUST *** re-load before cleanup
    // We found that in GCC ,compile with -O3 may leads compiler
    // load CURRENT address only once, that will leads improper result
    // to ready list after thread migration.
    // Here seperate a standalone function threqad_stub_cleanup, and forbidden
    // inline optimization for it, to make sure load CURRENT again before clean up
    __attribute__((noinline))
    static void thread_stub_cleanup()
    {
        deallocate_tls();
        // if CURRENT is idle stub and during vcpu_fini
        // main thread waiting for idle stub joining, now idle might be only
        // thread in run-queue. To keep going, wake up waiter before remove
        // current from run-queue.
        auto th = CURRENT;
        th->lock.lock();
        th->state = states::DONE;
        th->cond.notify_one();
        th->get_vcpu()->nthreads--;
        auto sw = atomic_runq()->remove_current(states::DONE);
        if (!th->joinable)
        {
            photon_switch_context_defer_die(th, sw.to->stack.pointer_ref(),
                                            &thread_die);
        } else {
            switch_context_defer(th, sw.to, spinlock_unlock, &th->lock);
        }
    }

    static void thread_stub()
    {
        // Since compiler may assumpt that th is always equal to CURRENT value
        // and do not knowing CURRENT address(thread local address) may change.
        auto th = (thread*)*(volatile thread**)&CURRENT;
        thread_yield_to((thread*)th->retval);
        th->go();
        thread_stub_cleanup();
    }

    thread* thread_create(thread_entry start, void* arg, uint64_t stack_size)
    {
        auto current = CURRENT;
        if (current == nullptr) {
            LOG_ERROR_RETURN(ENOSYS, nullptr, "Photon not initialized in this vCPU (OS thread)");
        }
        size_t randomizer = (rand() % 32) * (1024 + 8);
        stack_size =
            align_up(randomizer + stack_size + sizeof(thread), PAGE_SIZE);
        char* ptr = nullptr;
        int err = posix_memalign((void**)&ptr, PAGE_SIZE, stack_size);
        if (err)
            LOG_ERROR_RETURN(err, nullptr, "Failed to allocate photon stack! ", ERRNO(err));
        auto p = ptr + stack_size - sizeof(thread) - randomizer;
        (uint64_t&)p &= ~63;
        auto th = new (p) thread;
        th->buf = ptr;
        th->idx = -1;
        th->start = start;
        th->arg = arg;
        th->stack_size = stack_size;
        th->stack.init(p, &thread_stub);
        th->state = states::READY;
       (th->vcpu = current->vcpu) -> nthreads++;
        atomic_runq()->insert_tail(th);
        th->retval = current;
        thread_yield_to(th);
        return th;
    }

#if defined(__x86_64__) && defined(__linux__) && defined(ENABLE_MIMIC_VDSO)
#include <sys/auxv.h>
    struct MimicVDSOTimeX86 {
        static constexpr size_t BASETIME_MAX = 12;
        static constexpr size_t REALTIME_CLOCK = 2;
        static constexpr size_t US_PER_SEC = 1ULL * 1000 * 1000;
        static constexpr size_t NS_PER_US = 1ULL * 1000;

        struct vgtod_ts {
            volatile uint64_t sec;
            volatile uint64_t nsec;
        };

        struct vgtod_data {
            unsigned int seq;

            int vclock_mode;
            volatile uint64_t cycle_last;
            uint64_t mask;
            uint32_t mult;
            uint32_t shift;

            struct vgtod_ts basetime[BASETIME_MAX];

            int tz_minuteswest;
            int tz_dsttime;
        };

        vgtod_data* vp = nullptr;
        uint64_t last_now = 0;

        MimicVDSOTimeX86() {
            vp = get_vvar_addr();
        }

        static vgtod_data* get_vvar_addr() {
            // quickly parse /proc/self/maps to find [vvar] mapping
            auto mmapsfile = fopen("/proc/self/maps", "r");
            if (!mmapsfile) {
                return nullptr;
            }
            DEFER(fclose(mmapsfile));
            size_t len = 0;
            char* line = nullptr;
            // getline will alloc buffer and realloc when line point to nullptr
            // so always free before return once
            DEFER(free(line));
            while ((getline(&line, &len, mmapsfile)) != EOF) {
                if (strstr(line, "[vvar]"))
                    return (vgtod_data*)(strtol(line, NULL, 16) + 0x80);
            }
            return nullptr;
        }

        __attribute__((always_inline)) static inline uint64_t rdtsc64() {
            uint32_t low, hi;
            asm volatile("rdtsc" : "=a"(low), "=d"(hi) : :);
            return ((uint64_t)hi << 32) | low;
        }

        operator bool() const { return vp; }

        uint64_t get_now(bool accurate = false) {
            if (!vp) {
                return -1;
            }
            uint64_t sec, ns, last;
            do {
                last = vp->cycle_last;
                sec = vp->basetime[REALTIME_CLOCK].sec;
                ns = vp->basetime[REALTIME_CLOCK].nsec;
            } while (unlikely(last != vp->cycle_last));
            if (unlikely(accurate)) {
                auto rns = ns;
                auto cycles = rdtsc64();
                if (likely(cycles > last))
                    rns += ((cycles - last) * vp->mult) >> vp->shift;
                return last_now = sec * US_PER_SEC + rns / NS_PER_US;
            }
            auto ret = sec * US_PER_SEC + ns / NS_PER_US;
            if (ret < last_now) return last_now;
            return last_now = ret;
        }
    } __mimic_vdso_time_x86;
#endif

    volatile uint64_t now;
    static std::atomic<pthread_t> ts_updater(0);
    static inline uint64_t update_now()
    {
#if defined(__x86_64__) && defined(__linux__) && defined(ENABLE_MIMIC_VDSO)
        if (likely(__mimic_vdso_time_x86))
            return photon::now = __mimic_vdso_time_x86.get_now();
#endif
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t nnow = tv.tv_sec;
        nnow *= 1000 * 1000;
        nnow += tv.tv_usec;
        now = nnow;
        return nnow;
    }
    __attribute__((always_inline))
    static inline uint32_t _rdtsc()
    {
    #if defined(__x86_64__)
        uint32_t low, hi;
        asm volatile(
            "rdtsc"
            : "=a"(low), "=d"(hi)
            :
            :);
        // assume working in 2Ghz, therefore 1ms ~ 2M = 1<<21
        // keep higher bits of tsc is enough
        return (hi << 12) | (low >> 20);
    #elif defined(__aarch64__)
        uint64_t val;
        asm volatile("mrs %0, cntvct_el0" : "=r" (val));
        return (uint32_t)(val >> 20);
    #endif
    }
    static uint32_t last_tsc = 0;
    static inline uint64_t if_update_now(bool accurate = false) {
#if defined(__x86_64__) && defined(__linux__) && defined(ENABLE_MIMIC_VDSO)
        if (likely(__mimic_vdso_time_x86)) {
            return photon::now = __mimic_vdso_time_x86.get_now(accurate);
        }
#endif
        if (ts_updater.load(std::memory_order_relaxed)) {
            return photon::now;
        }
        if (accurate)
            return update_now();
        uint32_t tsc = _rdtsc();
        if (last_tsc != tsc) {
            last_tsc = tsc;
            return update_now();
        }
        return photon::now;
    }
    int timestamp_updater_init() {
        if (!ts_updater) {
            std::thread([&]{
                pthread_t current_tid = pthread_self(), pid = 0;
                if (!ts_updater.compare_exchange_weak(pid, current_tid, std::memory_order_acq_rel))
                    return;
                while (current_tid == ts_updater.load(std::memory_order_relaxed)) {
                    usleep(500);
                    update_now();
                }
            }).detach();
            return 0;
        }
        LOG_WARN("Timestamp updater already started");
        return -1;
    }
    int timestamp_updater_fini() {
        if (ts_updater.load()) {
            ts_updater = 0;
            return 0;
        }
        LOG_WARN("Timestamp updater not launch or already stopped");
        return -1;
    }
    extern void photon_switch_context(void**, void**) asm ("_photon_switch_context");
    extern void photon_switch_context_defer(void**, void**,
        void(*defer)(void*), void* arg) asm ("_photon_switch_context_defer");
    inline void switch_context(thread* from, thread* to)
    {
        assert(from->vcpu == to->vcpu);
        to->state = states::RUNNING;
        to->get_vcpu()->switch_count++;
        photon_switch_context(from->stack.pointer_ref(), to->stack.pointer_ref());
    }
    // switch `to` a context and make a call `defer(arg)`
    inline void switch_context_defer(thread* from, thread* to,
        void(*defer)(void*), void* arg)
    {
        assert(from->vcpu == to->vcpu);
        to->state   = states::RUNNING;
        to->get_vcpu()->switch_count ++;
        photon_switch_context_defer(from->stack.pointer_ref(),
            to->stack.pointer_ref(), defer, arg);
    }
    inline void switch_context_defer(thread* from, states new_state, thread* to,
        void(*defer)(void*), void* arg)
    {
        from->state = new_state;
        switch_context_defer(from, to, defer, arg);
    }

    static int resume_threads()
    {
        int count = 0;
        auto vcpu = CURRENT->get_vcpu();
        auto& standbyq = vcpu->standbyq;
        auto& sleepq = vcpu->sleepq;
        if (!standbyq.empty())
        {   // threads interrupted by other vcpus were not popped from sleepq
            auto q = standbyq.eject_whole_atomic();
            if (q) {
                thread_list list(q);
                for (auto th: list) {
                    assert(th->state == states::STANDBY);
                    th->state = states::READY;
                    sleepq.pop(th);
                    ++count;
                }
                list.node = nullptr;
                atomic_runq()->insert_list_before(q);
            }
            return count;
        }

        if_update_now();
        while(!sleepq.empty())
        {
            auto th = sleepq.front();
            if (th->ts_wakeup > now) break;
            SCOPED_LOCK(th->lock);
            sleepq.pop_front();
            if (th->state == states::SLEEPING) {
                th->dequeue_ready_atomic();
                atomic_runq()->insert_tail(th);
                count++;
            }
        }
        return count;
    }

    states thread_stat(thread* th)
    {
        return th->state;
    }

    void thread_yield()
    {
        assert(!atomic_runq()->single());
        auto sw = atomic_runq()->goto_next();
        if_update_now();
        switch_context(sw.from, sw.to);
    }

    void thread_yield_to(thread* th)
    {
        if (unlikely(th == nullptr)) { // yield to any thread
            return thread_yield();
        } else if (unlikely(th == CURRENT)) { // yield to current should just update time
            if_update_now();
            return;
        } else if (unlikely(th->vcpu != CURRENT->vcpu)) {
            LOG_ERROR_RETURN(EINVAL, , "target thread ` must be run by the same vcpu as CURRENT!", th);
        } else if (unlikely(th->state == states::STANDBY)) {
            while (th->state == states::STANDBY)
                resume_threads();
            assert(th->state == states::READY);
        } else if (unlikely(th->state != states::READY)) {
            LOG_ERROR_RETURN(EINVAL, , "target thread ` must be READY!", th);
        }

        auto sw = atomic_runq()->try_goto(th);
        if (!sw.to) return;
        if_update_now();
        switch_context(sw.from, sw.to);
    }

    static Switch prepare_usleep(uint64_t useconds, thread_list* waitq)
    {
        SCOPED_MEMBER_LOCK(waitq);
        SCOPED_LOCK(CURRENT->lock);
        assert(!atomic_runq()->single());
        auto sw = atomic_runq()->remove_current(states::SLEEPING);
        if (waitq) {
            waitq->push_back(sw.from);
            sw.from->waitq = waitq;
        }
        if_update_now(true);
        sw.from->ts_wakeup = sat_add(now, useconds);
        sw.from->get_vcpu()->sleepq.push(sw.from);
        return sw;
    }

    // returns 0 if slept well (at lease `useconds`), -1 otherwise
    static int thread_usleep(uint64_t useconds, thread_list* waitq)
    {
        if (unlikely(useconds == 0)) {
            thread_yield();
            return 0;
        }

        auto r = prepare_usleep(useconds, waitq);
        switch_context(r.from, r.to);
        assert(r.from->waitq == nullptr);
        return r.from->set_error_number();
    }

    typedef void (*defer_func)(void*);
    static int thread_usleep_defer(uint64_t useconds,
        thread_list* waitq, defer_func defer, void* defer_arg)
    {
        auto r = prepare_usleep(useconds, waitq);
        switch_context_defer(r.from, r.to, defer, defer_arg);
        assert(r.from->waitq == nullptr);
        return r.from->set_error_number();
    }

    int thread_usleep_defer(uint64_t useconds, defer_func defer, void* defer_arg) {
        if (CURRENT == nullptr) {
            LOG_ERROR_RETURN(ENOSYS, -1, "Photon not initialized in this thread");
        }
        if (unlikely(atomic_runq()->defer_to_new_thread())) {
            thread_create((thread_entry&)defer, defer_arg);
            return thread_usleep(useconds);
        }
        if (unlikely(CURRENT->shutting_down && useconds > 10*1000)) {
            int ret = thread_usleep_defer(10*1000, nullptr, defer, defer_arg);
            if (ret >= 0)
                errno = EPERM;
            return -1;
        }
        return thread_usleep_defer(useconds, nullptr, defer, defer_arg);
    }

    int thread_usleep(uint64_t useconds)
    {
        if (CURRENT == nullptr) {
            LOG_ERROR_RETURN(ENOSYS, -1, "Photon not initialized in this thread");
        }
        if (CURRENT->shutting_down && useconds > 10*1000)
        {
            int ret = thread_usleep(10*1000, nullptr);
            if (ret >= 0)
                errno = EPERM;
            return -1;
        }
        return thread_usleep(useconds, nullptr);
    }

    static void prelocked_thread_interrupt(thread* th, int error_number)
    {
        vcpu_t* vcpu = th->get_vcpu();
        assert(th && th->state == states::SLEEPING);
        assert("th->lock is locked");
        assert(th != CURRENT);
        th->error_number = error_number;
        if (!CURRENT || vcpu != CURRENT->get_vcpu()) {
            th->dequeue_ready_atomic(states::STANDBY);
            vcpu->move_to_standbyq_atomic(th);
        } else {
            th->dequeue_ready_atomic();
            vcpu->sleepq.pop(th);
            atomic_runq()->insert_tail(th);
        }
    }
    void thread_interrupt(thread* th, int error_number)
    {
        if (!th)
            LOG_ERROR_RETURN(EINVAL, , "invalid parameter");
        if (th->state != states::SLEEPING) return;
        SCOPED_LOCK(th->lock);
        if (th->state != states::SLEEPING) return;

        prelocked_thread_interrupt(th, error_number);
    }

    static void do_stack_pages_gc(void* arg) {
#ifndef __aarch64__
        auto th = (thread*)arg;
        assert(th->vcpu == CURRENT->vcpu);
        auto buf = th->buf;
        if (buf == nullptr) {
            // th is the main thread of the vcpu
            return;
        }
        auto rsp = (char*)th->stack._ptr;
        auto len = align_down(rsp - buf, PAGE_SIZE);
        madvise(buf, len, MADV_DONTNEED);
#endif
    }

    int stack_pages_gc(thread* th) {
        if (!th || th->vcpu != CURRENT->vcpu)
            LOG_ERROR_RETURN(EINVAL, -1, "target thread ` must be run on CURRENT vCPU", th);
        if (th->state == RUNNING) {
            auto next = atomic_runq()->goto_next().to;
            switch_context_defer(th, next, do_stack_pages_gc, th);
        } else {
            do_stack_pages_gc(th);
        }
        return 0;
    }

    template<typename T, typename PT = T*> inline
    T* indirect_lock(volatile PT* ppt, T* end)
    {
    again:
        T* x = *ppt;
        if (!x || x == end)
            return nullptr;
        x->lock.lock();
        if (x == *ppt)
            return x;
        x->lock.unlock();
        goto again;
    }
    inline thread* indirect_lock(thread** ppt)
    {
        return indirect_lock<thread>(ppt, nullptr);
    }
    join_handle* thread_enable_join(thread* th, bool flag)
    {
        th->joinable = flag;
        return (join_handle*)th;
    }

    void thread_join(join_handle* jh)
    {
        auto th = (thread*)jh;
        if (!th->joinable)
            LOG_ERROR_RETURN(ENOSYS, , "join is not enabled for thread ", th);

        th->lock.lock();
        if (th->state != states::DONE) {
            th->cond.wait(th->lock);
        }
        assert(th->state == states::DONE);
        thread_die(th);
    }
    inline void thread_join(thread* th)
    {
        thread_join((join_handle*)th);
    }

    int thread_shutdown(thread* th, bool flag)
    {
        if (!th)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid thread");

        th->shutting_down = flag;
        if (th->state == states::SLEEPING)
            thread_interrupt(th, EPERM);
        return 0;
    }

    void threads_create_join(uint64_t n,
        thread_entry start, void* arg, uint64_t stack_size)
    {
        if (n == 0) return;
        thread* threads[32];
        thread** pthreads = threads;
        std::vector<thread*> _threads;
        if (n > 32)
        {
            _threads.resize(n);
            pthreads = &_threads[0];
        }
        for (uint64_t i = 0; i < n; ++i)
        {
            auto th = thread_create(start, arg, stack_size);
            if (!th) break;
            thread_enable_join(th);
            pthreads[i] = th;
        }
        for (uint64_t i = 0; i < n; ++i) {
            thread_join(pthreads[i]);
        }
    }

    void Timer::stub()
    {
        auto timeout = _default_timeout;
        do {
        again:
            _waiting = true;
            _wait_ready.notify_all();
            int ret = thread_usleep(timeout);
            _waiting = false;
            if (ret < 0)
            {
                int e = errno;
                if (e == ECANCELED) {
                    break;
                } else if (e == EAGAIN) {
                    timeout = _reset_timeout;
                    goto again;
                }
                else assert(false);
            }

            timeout = _on_timer.fire();
            if (!timeout)
                timeout = _default_timeout;
        } while(_repeating);
        _th = nullptr;
    }
    void* Timer::_stub(void* _this)
    {
        static_cast<Timer*>(_this)->stub();
        return nullptr;
    }

    int spinlock::lock() {
        while (_lock.exchange(true, std::memory_order_acquire)) {
            while (_lock.load(std::memory_order_relaxed)) {
                spin_wait();
            }
        }
        return 0;
    }

    void spinlock::unlock() {
        _lock.store(false, std::memory_order_release);
    }

    int spinlock::try_lock() {
        return (!_lock.load(std::memory_order_relaxed) && !_lock.exchange(true, std::memory_order_acquire)) ? 0 : -1;
    }

    int ticket_spinlock::lock() {
        const auto ticket = next.fetch_add(1, std::memory_order_relaxed);
        while (serv.load(std::memory_order_acquire) != ticket) {
#ifdef __aarch64__
            asm volatile("isb" : : : "memory");
#else
            _mm_pause();
#endif
        }
        return 0;
    }

    void ticket_spinlock::unlock() {
        const auto successor = serv.load(std::memory_order_relaxed) + 1;
        serv.store(successor, std::memory_order_release);
    }

    inline int waitq_translate_errno(int ret)
    {
        auto perrno = &errno;
        if (ret == 0)
        {
            *perrno = ETIMEDOUT;
            return -1;
        }
        return (*perrno == ECANCELED) ? 0 : -1;
    }
    int waitq::wait(uint64_t timeout)
    {
        static_assert(sizeof(q) == sizeof(thread_list), "...");
        auto lst = (thread_list*)&q;
        int ret = thread_usleep(timeout, lst);
        return waitq_translate_errno(ret);
    }
    int waitq::wait_defer(uint64_t timeout, void(*defer)(void*), void* arg) {
        static_assert(sizeof(q) == sizeof(thread_list), "...");
        auto lst = (thread_list*)&q;
        int ret = thread_usleep_defer(timeout, lst, defer, arg);
        return waitq_translate_errno(ret);
    }

/*
    void waitq::resume(thread* th, int error_number)
    {
        auto lst = (thread_list*)&q;
        assert(th->waitq == lst);
        if (!th || !q || th->waitq != lst)
            LOG_ERROR_RETURN(EINVAL, , " invalid arguement ", VALUE(th));

        thread_interrupt(th, error_number); // may update q
    }
*/
    struct ScopedLockHead
    {
        thread* _th;
        ScopedLockHead(waitq* waitq) :
            _th(indirect_lock(&waitq->q.th)) { }
        operator thread*()   { return _th; }
        thread* operator->() { return _th; }
        ~ScopedLockHead()    { if (_th) _th->lock.unlock(); }
    };
    thread* waitq::resume_one(int error_number)
    {
        ScopedLockHead h(this);
        if (h)
        {
            assert(h->waitq == (thread_list*)this);
            prelocked_thread_interrupt(h, error_number);
            assert(h->waitq == nullptr);
            assert(this->q.th != h);
        }
        return h;
    }
    int waitq::resume_all(int error_number)
    {
        int r = 0;
        while (resume_one(error_number) != 0) r++;
        return r;
        // auto lst = (thread_list*)&q;
        // return thread_list_interrupt(lst, error_number);
    }
    static void spinlock_unlock(void* s_)
    {
        auto splock = (spinlock*)s_;
        splock->unlock();
    }
    int mutex::lock(uint64_t timeout)
    {
        if (try_lock() == 0)
            return 0;
        splock.lock();
        if (try_lock() == 0) {
            splock.unlock();
            return 0;
        }

        if (timeout == 0) {
            errno = ETIMEDOUT;
            splock.unlock();
            return -1;
        }

        int ret = thread_usleep_defer(timeout,
            (thread_list*)&q, &spinlock_unlock, &splock);
        return waitq_translate_errno(ret);
    }
    int mutex::try_lock()
    {
        thread* ptr = nullptr;
        bool ret = owner.compare_exchange_strong(ptr, CURRENT,
            std::memory_order_release, std::memory_order_relaxed);
        return (int)ret - 1;
    }
    inline void do_mutex_unlock(mutex* m)
    {
        SCOPED_LOCK(m->splock);
        ScopedLockHead h(m);
        m->owner.store(h);
        if (h)
            prelocked_thread_interrupt(h, ECANCELED);
    }
    static void mutex_unlock(void* m_)
    {
        if (!m_) return;
        auto m = (mutex*)m_;
        assert(m->owner);   // should be locked
        do_mutex_unlock(m);
    }
    void mutex::unlock()
    {
        auto th = owner.load();
        if (!th)
            LOG_ERROR_RETURN(EINVAL, , "the mutex was not locked");
        if (th != CURRENT)
            LOG_ERROR_RETURN(EINVAL, , "the mutex was not locked by current thread");
        do_mutex_unlock(this);
    }

    int recursive_mutex::lock(uint64_t timeout) {
        if (owner == CURRENT || mutex::lock(timeout) == 0) {
            recursive_count++;
            return 0;
        }
        return -1;
    }

    int recursive_mutex::try_lock() {
        if (owner == CURRENT || mutex::try_lock() == 0) {
            recursive_count++;
            return 0;
        }
        return -1;
    }

    void recursive_mutex::unlock() {
        auto th = owner.load();
        if (!th) {
            LOG_ERROR_RETURN(EINVAL, , "the mutex was not locked");
        }
        if (th != CURRENT) {
            LOG_ERROR_RETURN(EINVAL, , "the mutex was not locked by current thread");
        }
        if (--recursive_count > 0) {
            return;
        }
        do_mutex_unlock(this);
    }
    int mutex_lock(void* arg) {
        return ((mutex*)arg)->lock();
    }
    int spinlock_lock(void* arg) {
        return ((spinlock*)arg)->lock();
    }
    static int cvar_do_wait(thread_list* q, void* m, uint64_t timeout, int(*lock)(void*), void(*unlock)(void*)) {
        assert(m);
        if (!m)
            LOG_ERROR_RETURN(EINVAL, -1, "there must be a lock");
        int ret = thread_usleep_defer(timeout, q, unlock, m);
        auto en = ret < 0 ? errno : 0;
        while (true) {
            int ret = lock(m);
            if (ret == 0) break;
            LOG_ERROR("failed to get mutex lock, ` `, try again", VALUE(ret), ERRNO());
            thread_usleep(1000, nullptr);
        }
        if (ret < 0) errno = en;
        return waitq_translate_errno(ret);

    }
    int condition_variable::wait(mutex* m, uint64_t timeout)
    {
        return cvar_do_wait((thread_list*)&q, m, timeout, mutex_lock, mutex_unlock);
    }
    int condition_variable::wait(spinlock* m, uint64_t timeout)
    {
        return cvar_do_wait((thread_list*)&q, m, timeout, spinlock_lock, spinlock_unlock);
    }
    int semaphore::wait(uint64_t count, uint64_t timeout)
    {
        if (count == 0) return 0;
        splock.lock();
        CURRENT->retval = (void*)count;
        Timeout tmo(timeout);
        int ret = 0;
        while (!try_substract(count)) {
            ret = waitq::wait_defer(tmo.timeout(), spinlock_unlock, &splock);
            splock.lock();
            if (ret < 0 && errno == ETIMEDOUT) {
                CURRENT->retval = 0;
                try_resume();       // when timeout, we need to try
                splock.unlock();    // to resume next thread(s) in q
                return ret;
            }
        }
        try_resume();
        splock.unlock();
        return 0;
    }
    void semaphore::try_resume()
    {
        auto cnt = m_count.load();
        while(true)
        {
            ScopedLockHead h(this);
            if (!h) return;
            auto th = (thread*)h;
            auto& qfcount = (uint64_t&)th->retval;
            if (qfcount > cnt) break;
            cnt -= qfcount;
            qfcount = 0;
            prelocked_thread_interrupt(th, ECANCELED);
        }
    }
    bool semaphore::try_substract(uint64_t count)
    {
        while(true)
        {
            auto mc = m_count.load();
            if (mc < count)
                return false;
            auto new_mc = mc - count;
            if (m_count.compare_exchange_strong(mc, new_mc))
                return true;
        }
    }
    int rwlock::lock(int mode, uint64_t timeout)
    {
        if (mode != RLOCK && mode != WLOCK)
            LOG_ERROR_RETURN(EINVAL, -1, "mode unknow");
        scoped_lock lock(mtx);
        // backup retval
        void* bkup = CURRENT->retval;
        DEFER(CURRENT->retval = bkup);
        auto mark = (uint64_t)CURRENT->retval;
        // mask mark bits, keep RLOCK WLOCK bit clean
        mark &= ~(RLOCK | WLOCK);
        // mark mode and set as retval
        mark |= mode;
        CURRENT->retval = (void*)(mark);
        int op;
        if (mode == RLOCK) {
            op = 1;
        } else { // WLOCK
            op = -1;
        }
        if (cvar.q.th || (op == 1 && state < 0) || (op == -1 && state != 0)) {
            do {
                int ret = cvar.wait(lock, timeout);
                if (ret < 0)
                    return -1; // break by timeout or interrupt
            } while ((op == 1 && state < 0) || (op == -1 && state != 0));
        }
        state += op;
        return 0;
    }
    int rwlock::unlock()
    {
        assert(state != 0);
        scoped_lock lock(mtx);
        if (state>0)
            state --;
        else
            state ++;
        if (state == 0 && cvar.q.th) {
            if (cvar.q.th && (((uint64_t)cvar.q.th->retval) & WLOCK)) {
                cvar.notify_one();
            } else
                while (cvar.q.th && (((uint64_t)cvar.q.th->retval) & RLOCK)) {
                    cvar.notify_one();
                }
        }
        return 0;
    }
    bool is_master_event_engine_default() {
        return CURRENT->get_vcpu()->is_master_event_engine_default();
    }
    void reset_master_event_engine_default() {
        CURRENT->get_vcpu()->reset_master_event_engine_default();
    }
    static void* idle_stub(void*)
    {
        constexpr uint64_t max = 10 * 1024 * 1024;
        auto vcpu = CURRENT->get_vcpu();
        auto last_idle = now;
        while (vcpu->state != states::DONE) {
            while (!atomic_runq()->single()) {
                thread_yield();
                if (sat_sub(now, last_idle) >= 1000UL) {
                    last_idle = now;
                    vcpu->master_event_engine->wait_and_fire_events(0);
                }
                resume_threads();
            }
            if (vcpu->state == states::DONE)
                break;
            // only idle stub aliving
            // other threads must be sleeping
            // fall in actual sleep
            auto usec = max;
            auto& sleepq = vcpu->sleepq;
            if (!sleepq.empty())
                usec = min(usec,
                    sat_sub(sleepq.front()->ts_wakeup, now));
            last_idle = now;
            vcpu->master_event_engine->wait_and_fire_events(usec);
            resume_threads();
        }
        return nullptr;
    }
    /**
     * Waiting for all current vcpu photon threads finish
     * @return 0 for all done, -1 for failure
     */
    int wait_all() {
        auto vcpu = CURRENT->get_vcpu();
        auto &sleepq = vcpu->sleepq;
        auto &standbyq = vcpu->standbyq;
        while (!atomic_runq()->size_1or2() || !sleepq.empty() || !standbyq.empty()) {
            if (!sleepq.empty()) {
                // sleep till all sleeping threads ends
                thread_usleep(1000UL);
            } else {
                thread_yield();
            }
        }
        return 0;
    }

    struct migrate_args {thread* th; vcpu_base* v;};
    static int do_thread_migrate(thread* th, vcpu_base* v);
    static void do_defer_migrate(void* m_) {
        auto m = (migrate_args*)m_;
        do_thread_migrate(m->th, m->v);
    }
    static int defer_migrate_current(vcpu_base* v) {
        auto sw = atomic_runq()->goto_next();
        migrate_args defer_arg{sw.from, v};
        switch_context_defer(sw.from, sw.to,
            &do_defer_migrate, &defer_arg);
        return 0;
    }
    int thread_migrate(thread* th, vcpu_base* v) {
        if (!th || !v) {
            LOG_ERROR_RETURN(EINVAL, -1, "target thread / vcpu must be specified")
        }
        if (v == CURRENT->vcpu) {
            return 0;
        }
        if (th == CURRENT) {
            return defer_migrate_current(v);
        }
        if (th->vcpu != CURRENT->vcpu) {
            LOG_ERROR_RETURN(EINVAL, -1,
                "Try to migrate thread `, which is not on current vcpu.", th)
        }
        if (th->state != READY) {
            LOG_ERROR_RETURN(EINVAL, -1,
                "Try to migrate thread `, which is not ready.", th)
        }
        return do_thread_migrate(th, v);
    }
    static int do_thread_migrate(thread* th, vcpu_base* vb) {
        assert(vb != th->vcpu);
        atomic_runq()->remove_from_list(th);
        th->get_vcpu()->nthreads--;
        th->state = STANDBY;
        th->idx = -1;
        auto vcpu = (vcpu_t*)vb;
        th->vcpu = vcpu;
        vcpu->nthreads++;
        vcpu->move_to_standbyq_atomic(th);
        return 0;
    }

    static std::atomic<uint32_t> _n_vcpu{0};
    uint32_t get_vcpu_num() {
        return _n_vcpu.load(std::memory_order_relaxed);
    }
    int vcpu_init()
    {
        if (CURRENT) return -1;      // re-init has no side-effect
        auto n = ++_n_vcpu;
        CURRENT = new thread;
        auto vcpu = new vcpu_t;
        CURRENT->vcpu = vcpu;
        CURRENT->idx = -1;
        CURRENT->state = states::RUNNING;
        vcpu->state = states::RUNNING;
        vcpu->nthreads = 1;
        vcpu->idle_worker = thread_create(&idle_stub, nullptr);
        thread_enable_join(vcpu->idle_worker);
        // to update timestamp
        if_update_now(true);
        return n;
    }
    int vcpu_fini()
    {
        if (!CURRENT) return -1;
        deallocate_tls();
        wait_all();
        auto vcpu = CURRENT->vcpu;
        assert(!atomic_runq()->single());
        assert(vcpu->nthreads == 2); // idle_stub & current alive
        vcpu->state = states::DONE;  // instruct idle_worker to exit
        thread_join(vcpu->idle_worker);
        _n_vcpu--;
        CURRENT->state = states::DONE;
        safe_delete(CURRENT);
        safe_delete(vcpu);
        return 0;
    }
}
