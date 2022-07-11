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

#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <memory.h>
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
        vcpu_t* vcpu;
        void* arg = nullptr;
        states state = states::READY;
        // std::atomic<states> state {states::READY};
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
            arg = nullptr;                  // arg will be used as thread-local variable
            return retval = start(_arg);
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
        void dequeue_run_atomic(thread* current);
        void dequeue_standby_atomic();

        bool operator < (const thread &rhs)
        {
            return this->ts_wakeup < rhs.ts_wakeup;
        }

        void dispose()
        {
            auto b = buf;
            madvise(b, stack_size, MADV_DONTNEED);
            free(b);
            // munmap(buf, stack_size);
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
        thread_list(thread* head)
        {
            this->node = head;
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

    struct vcpu_t : public vcpu_base
    {
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
    };

    inline void thread::dequeue_ready_atomic(states newstat)
    {
        assert("this->lock is locked");
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
    inline void thread::dequeue_run_atomic(thread* current) // invoked by this vcpu
    {
        assert("this->lock is locked");
        assert(state == states::SLEEPING);
        assert(current->vcpu == this->vcpu);
        dequeue_ready_atomic();
        current->insert_tail(this);
    }
    inline void thread::dequeue_standby_atomic()   // invoked by other vcpu
    {
        assert("this->lock is locked");
        assert(state == states::SLEEPING);
        dequeue_ready_atomic(states::STANDBY);
        vcpu->move_to_standbyq_atomic(this);
    }

    static vcpu_t vcpus[128];
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
    inline void switch_context(thread* from, thread* to);

    // Since thread may be moved from one vcpu to another
    // thread local CURRENT *** MUST *** re-load before cleanup
    // We found that in GCC ,compile with -O3 may leads compiler
    // load CURRENT address only once, that will leads improper result 
    // to ready list after thread migration.
    // Here seperate a standalone function threqad_stub_cleanup, and forbidden
    // inline optimization for it, to make sure load CURRENT again before clean up
    __attribute__((noinline))
    static void thread_stub_cleanup() {
        auto &current = CURRENT;
        auto th = current;
        th->lock.lock();
        th->state = states::DONE;
        th->cond.notify_all();
        assert(!current->single());
        auto next = current = th->remove_from_list();
        th->vcpu->nthreads--;
        if (!th->joinable)
        {
            photon_switch_context_defer_die(th,
                next->stack.pointer_ref(), &thread_die);
        } else {
            switch_context_defer(th, next, &spinlock_unlock, (void*)&th->lock);
        }
    }

    static void thread_stub()
    {
        auto th = CURRENT;
        thread_yield_to((thread*)th->retval);
        th->go();
        thread_stub_cleanup();
    }

    thread* thread_create(void* (*start)(void*), void* arg, uint64_t stack_size)
    {
        auto current = CURRENT;
        if (current == nullptr) {
            LOG_ERROR_RETURN(ENOSYS, nullptr, "Photon not initialized in this thread");
        }
        size_t randomizer = (rand() % 32) * (1024 + 8);
        stack_size =
            align_up(randomizer + stack_size + sizeof(thread), PAGE_SIZE);
        auto ptr = (char*)aligned_alloc(PAGE_SIZE, stack_size);
        if (!ptr)
            LOG_ERROR_RETURN(0, nullptr, "Failed to allocate photon stack!");
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
        auto vcpu = current->vcpu;
        th->vcpu = vcpu;
        vcpu->nthreads++;
        current->insert_tail(th);
        th->retval = current;
        thread_yield_to(th);
        return th;
    }

    uint64_t now;
    static inline uint64_t update_now()
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t nnow = tv.tv_sec;
        nnow *= 1000 * 1000;
        nnow += tv.tv_usec;
        now = nnow;
        return nnow;
    }
    __attribute__((always_inline))
    static inline uint32_t _rdtscp()
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
    static inline uint64_t if_update_now() {
        uint32_t tsc = _rdtscp();
        if (last_tsc != tsc) {
            last_tsc = tsc;
            return update_now();
        }
        return photon::now;
    }
    static inline void prefetch_context(thread* from, thread* to)
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
    extern void photon_switch_context(void**, void**) asm ("_photon_switch_context");
    extern void photon_switch_context_defer(void**, void**,
        void(*defer)(void*), void* arg) asm ("_photon_switch_context_defer");
    inline void switch_context(thread* from, thread* to)
    {
        assert(from->vcpu == to->vcpu);
        to->state   = states::RUNNING;
        to->vcpu->switch_count ++;
        photon_switch_context(from->stack.pointer_ref(), to->stack.pointer_ref());
    }
    inline void switch_context(thread* from, states new_state, thread* to)
    {
        from->state = new_state;
        switch_context(from, to);
    }
    // switch `to` a context and make a call `defer(arg)`
    inline void switch_context_defer(thread* from, thread* to,
        void(*defer)(void*), void* arg)
    {
        assert(from->vcpu == to->vcpu);
        to->state   = states::RUNNING;
        to->vcpu->switch_count ++;
        photon_switch_context_defer(from->stack.pointer_ref(),
            to->stack.pointer_ref(), defer, arg);
    }
    inline void switch_context_defer(thread* from, states new_state, thread* to,
        void(*defer)(void*), void* arg)
    {
        from->state = new_state;
        switch_context_defer(from, to, defer, arg);
    }

    inline thread* move_list(thread* current, thread_list& standbyq)
    {
        SCOPED_LOCK(standbyq.lock);
        current->insert_list_before(standbyq.node);
        auto head = standbyq.node;
        standbyq.node = nullptr;
        return head;
    }

    static int resume_threads()
    {
        int count = 0;
        auto current = CURRENT;
        auto vcpu = current->vcpu;
        auto& standbyq = vcpu->standbyq;
        auto& sleepq = vcpu->sleepq;
        if (!standbyq.empty())
        {   // threads interrupted by other vcpus were not popped from sleepq
            for (auto th = move_list(current, standbyq);
                      th != current;
                      th = th->next())
            {
                assert(th->state == states::STANDBY);
                sleepq.pop(th);
                th->state = states::READY;
                ++count;
            }
            return count;
        }

        update_now();
        while(!sleepq.empty())
        {
            auto th = sleepq.front();
            if (th->ts_wakeup > now) break;
            SCOPED_LOCK(th->lock);
            sleepq.pop_front();
            if (th->state == states::SLEEPING)
            {
                th->dequeue_run_atomic(current);
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
        auto& current = CURRENT;
        auto t0 = current;
        assert(!t0->single());
        // prefetch_context<1>(t0);
        auto next = current = t0->next();
        // prefetch_context<0>(next);
        if_update_now();
        switch_context(t0, states::READY, next);
    }

    void thread_yield_to(thread* th)
    {
        auto& current = CURRENT;
        auto t0 = current;
        if (th == nullptr) { // yield to any thread
            return thread_yield();
        } else if (th == current) { // yield to current should just update time
            if_update_now();
            return;
        } else if (th->vcpu != t0->vcpu) {
            LOG_ERROR_RETURN(EINVAL, , VALUE(th), " must be at same vcpu as CURRENT!");
        } else if (th->state == states::STANDBY) {
            while (th->state == states::STANDBY)
                resume_threads();
            assert(th->state == states::READY);
        }
        if (th->state != states::READY) {
            LOG_ERROR_RETURN(EINVAL, , VALUE(th), " must be READY!");
        }

        current = th;
        prefetch_context(t0, th);
        if_update_now();
        switch_context(t0, states::READY, th);
    }

    struct PUR {thread* t0; thread* next;};
    static PUR prepare_usleep(thread*& current, uint64_t useconds, thread_list* waitq)
    {
        assert(!current->single());
        auto t0 = current;
        auto next = current = t0->remove_from_list();   // CURRENT moves to the next;
        if (waitq)
        {
            waitq->push_back(t0);
            t0->waitq = waitq;
        }
        prefetch_context(t0, next);
        t0->ts_wakeup = sat_add(now, useconds);
        t0->vcpu->sleepq.push(t0);
        t0->state = states::SLEEPING;
        return {t0, next};
    }

    // returns 0 if slept well (at lease `useconds`), -1 otherwise
    static int thread_usleep(uint64_t useconds, thread_list* waitq)
    {
        if (useconds == 0) {
            thread_yield();
            return 0;
        }

        auto r = ({
            auto& current = CURRENT;
            SCOPED_MEMBER_LOCK(waitq);
            SCOPED_LOCK(current->lock);
            prepare_usleep(current, useconds, waitq);
        });
        if_update_now();
        switch_context(r.t0, r.next);
        assert(r.t0->waitq == nullptr);
        return r.t0->set_error_number();
    }

    typedef void (*defer_func)(void*);
    static int thread_usleep_defer(uint64_t useconds,
        thread_list* waitq, defer_func defer, void* defer_arg)
    {
        auto r = ({
            auto& current = CURRENT;
            SCOPED_MEMBER_LOCK(waitq);
            SCOPED_LOCK(current->lock);
            prepare_usleep(current, useconds, waitq);
        });

        switch_context_defer(r.t0, r.next, defer, defer_arg);
        assert(r.t0->waitq == nullptr);
        return r.t0->set_error_number();
    }

    int thread_usleep_defer(uint64_t useconds, defer_func defer, void* defer_arg) {
        auto current = CURRENT;
        if (current == nullptr) {
            LOG_ERROR_RETURN(ENOSYS, -1, "Photon not initialized in this thread");
        }
        auto idle_worker = current->vcpu->idle_worker;
        if (current->next() == idle_worker)
        {   // if defer_func is executed in idle_worker and it yields, photon will be breaked
            if (idle_worker->next() == current) {
                thread_create((thread_entry&)defer, defer_arg);
                return thread_usleep(useconds);
            } else {
                auto next = idle_worker->remove_from_list();
                next->insert_after(idle_worker);
            }
        }
        if (current->shutting_down && useconds > 10*1000)
        {
            int ret = thread_usleep_defer(10*1000, nullptr, defer, defer_arg);
            if (ret >= 0)
                errno = EPERM;
            return -1;
        }
        return thread_usleep_defer(useconds, nullptr, defer, defer_arg);
    }

    int thread_usleep(uint64_t useconds)
    {
        auto current = CURRENT;
        if (current == nullptr) {
            LOG_ERROR_RETURN(ENOSYS, -1, "Photon not initialized in this thread");
        }
        if (current->shutting_down && useconds > 10*1000)
        {
            int ret = thread_usleep(10*1000, nullptr);
            if (ret >= 0)
                errno = EPERM;
            return -1;
        }
        return thread_usleep(useconds, nullptr);
    }

    static void prelocked_thread_interrupt(thread* th,
        int error_number, thread* current = CURRENT)
    {
        vcpu_t* vcpu;
        assert(th && th->state == states::SLEEPING);
        assert("th->lock is locked");
        assert(th != current);
        th->error_number = error_number;
        if (!current || (vcpu = current->vcpu) != th->vcpu) {
            th->dequeue_standby_atomic();
        } else {
            vcpu->sleepq.pop(th);
            th->dequeue_run_atomic(current);
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
        auto th = (thread*)arg;
        assert(th->vcpu == CURRENT->vcpu);
        // hold `lock` so th will not change state
        auto buf = th->buf;
        if (buf == nullptr) {
            // thread stack it is
            return;
        }
        auto rsp = (char*)th->stack._ptr;
        auto len = align_down(rsp - buf, PAGE_SIZE);
        madvise(buf, len, MADV_DONTNEED);
    }

    int stack_pages_gc(thread* th) {
        if (th->vcpu != CURRENT->vcpu)
            return -1;
        if (th->state == RUNNING) {
            auto next = CURRENT = th->next();
            th->state = READY;
            switch_context_defer(th, next, do_stack_pages_gc, th);
        } else {
            do_stack_pages_gc((void*)th);
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
/*
    template<typename T, typename P>
    struct common_value
    {
        P a, b;
        static_assert(sizeof(T) == sizeof(P), "...");
        common_value(const T& x)
        {
            a = b = (const P&)x;
        }
        void operator << (const T& x)
        {
            a &= (const P&)x;
            b |= (const P&)x;
        }
        operator bool() { return a == b; }
        operator T()    { return (T&)a; }
        T get()         { return (T&)a; }
    };

    template<typename F> inline
    void for_each_th(thread* head, int count, const F& func)
    {
        auto th = head;
        for (int i = 0; i < count; ++i, th = th->next())
        {
            func(th);
        }
    }

    static int thread_list_interrupt(thread_list* lst, int error_number)
    {
        if (!lst) return 0;
        if (!lst->node) return 0;
        auto head = indirect_lock(&lst->node);  // lock the 1st thread
        if (!head) return 0;
        assert(head->waitq == lst);
        head->waitq = nullptr;

        auto tail = head;
        int count = 1;
        common_value<states, int> common_states(head->state);
        common_value<vcpu_t*, uint64_t> common_vcpu(head->vcpu);
        while (tail->next() != head)
        {   // lock the following threads
            auto& next = (thread*&)tail->__next_ptr;
            auto th = indirect_lock(&next, head);
            if (!th) break;
            common_vcpu << th->vcpu;
            common_states << th->state;
            assert(th->waitq == lst);
            th->waitq = nullptr;
            tail = th;
            count++;
        }

        {   // remove the threads from lst
            SCOPED_LOCK(lst->lock);
            assert(lst->front() == head);
            if (lst->back() == tail) {
                lst->node = nullptr;
            } else {
                auto new_head = tail->next();
                auto new_tail = lst->back();
                new_head->__next_ptr = new_tail;
                new_tail->__prev_ptr = new_head;
                lst->node = new_head;
                tail->__next_ptr = head;
                head->__prev_ptr = tail;
            }
        }

        auto current = CURRENT;
        if (!common_vcpu)
        {   // case 1: the threads has no common vcpus
            auto th = head;
            for (int i = 0; i < count; ++i)
            {
                auto next = th->next();
                th->__prev_ptr = th->__next_ptr = th;
                prelocked_thread_interrupt(th, error_number, current);
                th->lock.unlock();
                th = next;
            }
            return count;
        }

        vcpu_t* vcpu = common_vcpu;
        if (current && current->vcpu == vcpu) {
            // case 2: the threads are bind to this vcpu
            current->insert_list_tail(head);
            for_each_th(head, count, [&](thread* th) {
                th->state = states::READY;
                th->error_number = error_number;
                vcpu->sleepq.pop(th);
                th->lock.unlock();
            });
        } else {
            // case 3: the threads are bind to another vcpu
            for_each_th(head, count, [&](thread* th) {
                th->state = states::READY;
                th->error_number = error_number;
            });
            thread_list new_list(head);
            vcpu->move_to_standbyq_atomic(&new_list);
        }
        return count;
    }
*/
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
        if (th->state != states::DONE)
        {
            th->cond.wait(th->lock);
            assert(th->single());
        }
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
#ifdef __aarch64__
                asm volatile("isb" : : : "memory");
#else
                _mm_pause();
#endif
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
        auto current = CURRENT;
        thread* ptr = nullptr;
        bool ret = owner.compare_exchange_strong(ptr, current,
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
        auto current = CURRENT;
        if (th != current)
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
        auto current = CURRENT;
        if (th != current) {
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
        auto current = CURRENT;
        current->retval = (void*)count;
        Timeout tmo(timeout);
        int ret = 0;
        while (!try_substract(count)) {
            ret = waitq::wait_defer(tmo.timeout(), spinlock_unlock, &splock);
            splock.lock();
            if (ret < 0 && errno == ETIMEDOUT) {
                current->retval = 0;
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
        auto current = CURRENT;
        void* bkup = current->retval;
        DEFER(current->retval = bkup);
        auto mark = (uint64_t)current->retval;
        // mask mark bits, keep RLOCK WLOCK bit clean
        mark &= ~(RLOCK | WLOCK);
        // mark mode and set as retval
        mark |= mode;
        current->retval = (void*)(mark);
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

    class NullEventEngine : public MasterEventEngine {
    public:
        std::mutex _mutex;
        std::condition_variable _cvar;
        std::atomic_bool notify{false};
        int wait_for_fd(int fd, uint32_t interests, uint64_t timeout) override {
            return -1;
        }
        int cancel_wait() override {
            notify.store(true, std::memory_order_release);
            _cvar.notify_all();
            return 0;
        }
        ssize_t wait_and_fire_events(uint64_t timeout = -1) override {
            DEFER(notify.store(false, std::memory_order_release));
            if (!timeout) return 0;
            timeout = std::min(timeout, 1000 * 100UL);
            std::unique_lock<std::mutex> lock(_mutex);
            if (notify.load(std::memory_order_acquire)) {
                return 0;
            }
            _cvar.wait_for(lock, std::chrono::microseconds(timeout));
            return 0;
        }
    };

    __thread MasterEventEngine* _default_event_engine;

    bool is_master_event_engine_default() {
        return CURRENT->vcpu->master_event_engine == _default_event_engine;
    }

    void reset_master_event_engine_default() {
        auto& ee = CURRENT->vcpu->master_event_engine;
        if (ee == _default_event_engine) return;
        delete ee;
        ee = _default_event_engine;
    }

    static vcpu_t* _init_current(uint32_t i)
    {
        auto current = CURRENT = new thread;
        auto vcpu = current->vcpu = &vcpus[i];
        vcpu->id = i;
        vcpu->nthreads = 1;
        current->idx = -1;
        current->state = states::RUNNING;
        _default_event_engine = new NullEventEngine;
        vcpu->master_event_engine = _default_event_engine;
        return vcpu;
    }
    static void* idle_stub(void*)
    {
        constexpr uint64_t max = 10 * 1024 * 1024;
        auto current = CURRENT;
        auto vcpu = current->vcpu;
        auto last_idle = now;
        while (vcpu->id != -1U)
        {
            while (!current->single()) {
                thread_yield();
                if (sat_sub(now, last_idle) >= 1000UL) {
                    last_idle = now;
                    vcpu->master_event_engine->wait_and_fire_events(0);
                }
                resume_threads();
            }
            if (vcpu->id == -1U)
                break;
            // only idle stub aliving
            // other threads must be sleeping
            // fall in actual sleep
            auto usec = max;
            auto& sleepq = vcpu->sleepq;
            if (!sleepq.empty())
                usec = std::min(usec,
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
        auto current = CURRENT;
        auto &sleepq = current->vcpu->sleepq;
        do {
            while (!sleepq.empty()) {
                // sleep till all sleeping threads ends
                thread_usleep(1000UL);
            }
            while (current->next() != current->prev()) {
                thread_yield();
            }
        } while (!sleepq.empty());
        return 0;
    }

    static std::atomic<uint32_t> _n_vcpu{0};

    int thread_migrate(photon::thread* th, vcpu_base* v) {
        if (th->state != READY) {
            LOG_ERROR_RETURN(EINVAL, -1,
                             "Try to migrate thread `, which is not ready.", th)
        }
        if (th->vcpu != CURRENT->vcpu) {
            LOG_ERROR_RETURN(
                EINVAL, -1,
                "Try to migrate thread `, which is not on current vcpu.", th)
        }
        if (v == nullptr) {
            v = &vcpus[(th->vcpu - &vcpus[0] + 1) % _n_vcpu];
        }
        if (v == th->vcpu) return 0;
        auto vc = (vcpu_t*)v;
        th->vcpu->nthreads--;
        vc->nthreads++;
        th->remove_from_list();
        th->state = STANDBY;
        th->vcpu = vc;
        th->idx = -1;
        vc->move_to_standbyq_atomic(th);
        return 0;
    }

    int thread_init()
    {
        if (CURRENT) return -1;      // re-init has no side-effect
        auto _vcpuid = _n_vcpu++;
        auto vcpu = _init_current(_vcpuid);
        vcpu->idle_worker = thread_create(&idle_stub, nullptr);
        thread_enable_join(vcpu->idle_worker);
        update_now();
        return _vcpuid;
    }
    int thread_fini()
    {
        auto& current = CURRENT;
        if (!current) return -1;
        auto vcpu = current->vcpu;
        while(current->next() != current->prev()) photon::thread_yield();
        assert(!current->single());
        assert(vcpu->nthreads == 2); // idle_stub & current alive
        assert(vcpu == &vcpus[vcpu->id]);
        vcpu->id = -1;  // instruct idle_worker to exit
        thread_join(vcpu->idle_worker);
        current->state = states::DONE;
        safe_delete(current);
        safe_delete(_default_event_engine);
        return 0;
    }
}
