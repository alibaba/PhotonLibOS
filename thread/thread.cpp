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

#define protected public
#include <photon/thread/thread.h>
#include <photon/thread/timer.h>
#include <photon/common/intrusive_list.h>
#undef protected

#include <memory.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstddef>
#include <cassert>
#include <cerrno>
#include <vector>
#include <new>
#include <thread>
#include <mutex>
#include <condition_variable>

#ifdef _WIN64
#include <processthreadsapi.h>
#include <stdlib.h>
inline int posix_memalign(void** memptr, size_t alignment, size_t size) {
    auto ok = malloc(size);
    if (!ok)
        return ENOMEM;
    *memptr = ok;
    return 0;
}
#else
#include <sys/mman.h>
#endif

#include <photon/io/fd-events.h>
#include <photon/common/timeout.h>
#include <photon/common/alog.h>
#include <photon/common/alog-functionptr.h>
#include <photon/thread/thread-key.h>
#include <photon/thread/arch.h>

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

// Define assembly section header for clang and gcc
#if defined(__APPLE__)
#define DEF_ASM_FUNC(name) ".text\n" \
                           #name": "
#elif defined(_WIN64)
#define DEF_ASM_FUNC(name) ".text\n .p2align 4\n" \
                           ".def "#name"; .scl 3; .type 32; .endef\n" \
                           #name": "
#else
#define DEF_ASM_FUNC(name) ".section .text."#name",\"axG\",@progbits,"#name",comdat\n" \
                           ".type "#name", @function\n" \
                           #name": "
#endif

namespace photon
{
    inline uint64_t min(uint64_t a, uint64_t b) { return (a<b) ? a : b; }
    class NullEventEngine : public MasterEventEngine {
    public:
        std::mutex _mutex;
        std::condition_variable _cvar;
        std::atomic_bool notify{false};

        __attribute__((noinline))
        int wait_for_fd(int fd, uint32_t interests, Timeout timeout) override {
            return -1;
        }

        __attribute__((noinline))
        int cancel_wait() override {
            {
                std::unique_lock<std::mutex> lock(_mutex);
                notify.store(true, std::memory_order_release);
            }
            _cvar.notify_all();
            return 0;
        }

        __attribute__((noinline))
        ssize_t wait_and_fire_events(uint64_t timeout) override {
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

    static Delegate<void*, size_t> photon_thread_alloc(
        &default_photon_thread_stack_alloc, nullptr);
    static Delegate<void, void*, size_t> photon_thread_dealloc(
        &default_photon_thread_stack_dealloc, nullptr);

    struct vcpu_t;
    struct thread;
    class Stack
    {
    public:
        template<typename F>
        void init(void* ptr, F ret2func, thread* th)
        {
            _ptr = ptr;
            assert((uint64_t)_ptr % 16 == 0);
            push(0);
            push(0);
            push(ret2func);
            push(th);   // rbp <== th
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

    #if defined(__has_feature)
    #   if __has_feature(address_sanitizer) // for clang
    #       define __SANITIZE_ADDRESS__ // GCC already sets this
    #   endif
    #endif

    #ifdef __SANITIZE_ADDRESS__
    extern "C" {
    // Check out sanitizer/asan-interface.h in compiler-rt for documentation.
    void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom,
                                        size_t size);
    void __sanitizer_finish_switch_fiber(void* fake_stack_save,
                                         const void** bottom_old, size_t* size_old);
    }

    static void asan_start(void** save, thread* to) {
        void* bottom = to->buf ? to->buf : to->stackful_alloc_top;
        __sanitizer_start_switch_fiber(save, bottom,
                                       to->stack_size);
    }

    static void asan_finish(void* save) {
        __sanitizer_finish_switch_fiber(save, nullptr, nullptr);
    }

#define ASAN_START() asan_finish((void*)nullptr);

#define ASAN_SWITCH(to)      \
        void* __save;                  \
        asan_start(&__save, to);       \
        DEFER({ asan_finish(__save); });

#define ASAN_DIE_SWITCH(to)  \
        asan_start(nullptr, to);

#else
#define ASAN_START(ptr)
#define ASAN_SWITCH(to)
#define ASAN_DIE_SWITCH(to)
#endif

    static void _asan_start() asm("_asan_start");

    __attribute__((used)) static void _asan_start() { ASAN_START(); }

    struct thread_list;
    struct thread : public intrusive_list_node<thread> {
        volatile vcpu_t* vcpu;
        Stack stack;
// offset 32B
        int idx = -1;                       /* index in the sleep queue array */
        int error_number = 0;
        thread_list* waitq = nullptr;       /* the q if WAITING in a queue */
        uint32_t flags = 0;
        uint16_t state = states::READY;
        spinlock lock, _;                   // Current usage of thread.lock:
                                            //  interrupt()
                                            //  die()
                                            //  resume_threads()
                                            //  prepare_usleep()
                                            //  thread_join()
                                            //  ScopedLockHead
                                            //  try_work_stealing()?
        uint64_t ts_wakeup = 0;             /* Wakeup time when thread is sleeping */
// offset 64B
        union {
            void* arg = nullptr;            /* will be reused as thread local */
            void* tls;                      /* storage after thread started  */
        };
        union {
            thread_entry start;
            uint64_t semaphore_count;
            uint64_t rwlock_mark;
            void* retval;
        };
        char* buf;
        char* stackful_alloc_top;
        size_t stack_size;
// offset 96B
        condition_variable cond;            /* used for join */

        enum shift {
            joinable = 0,
            enable_work_stealing = 1,
            pause_work_stealing = 2,
            shutting_down = 3,              // the thread should cancel what is doing, and quit
        };                                  // current job ASAP; not allowed to sleep or block more
                                            // than 10ms, otherwise -1 will be returned and errno == EPERM
        bool is_bit(int i) { return flags & (1<<i); }
        void clear_bit(int i) { flags &= ~(1<<i); }
        void set_bit(int i) { flags |= (1<<i); }
        void set_bit(int i, bool flag) { if (likely(flag)) set_bit(i); else clear_bit(i); }
        bool is_joinable() { return is_bit(shift::joinable); }
        void set_joinable(bool flag = true) { set_bit(shift::joinable, flag); }
        bool is_shutting_down() { return is_bit(shift::shutting_down); }
        void set_shutting_down(bool flag = true) { set_bit(shift::shutting_down, flag); }
        bool allow_work_stealing() { return is_bit(shift::enable_work_stealing) &&
                                          !(flags & THREAD_PAUSE_WORK_STEALING); }
        int set_error_number() {
            if (likely(error_number)) {
                errno = error_number;
                error_number = 0;
                return -1;
            }
            return 0;
        }

        struct stack_alloc_header {
            uint32_t size;
            uint32_t reserved;
        };

        static_assert(sizeof(stack_alloc_header) == sizeof(uint64_t),
                      "stack_alloc_header size not fit");

        void* stackful_malloc(size_t size) {
            auto ret = stackful_alloc_top;
            stackful_alloc_top += size;
            auto head = (stack_alloc_header*)stackful_alloc_top;
            head->size = size;
            stackful_alloc_top += sizeof(stack_alloc_header);
            return ret;
        }

        void stackful_free(void* ptr) {
            assert(((uint64_t)ptr) < ((uint64_t)stackful_alloc_top));
            auto rc = stackful_alloc_top - sizeof(stack_alloc_header);
            (void)rc;
            assert(ptr == (rc - ((stack_alloc_header*)rc)->size));
            stackful_alloc_top = (char*)ptr;
        }

        void init_main_thread_stack() {
#ifdef __APPLE__
            auto self = pthread_self();
            stack_size = pthread_get_stacksize_np(self);
            stackful_alloc_top = (char*) pthread_get_stackaddr_np(self);
#elif defined(_WIN64)
            ULONG_PTR stack_low, stack_high;
            GetCurrentThreadStackLimits(&stack_low, &stack_high);
            stackful_alloc_top = (char*)stack_low;
            stack_size = stack_high - stack_low;
#elif defined(__linux__)
            pthread_attr_t gattr;
            pthread_getattr_np(pthread_self(), &gattr);
            pthread_attr_getstack(&gattr,
                (void**)&stackful_alloc_top, &stack_size);
            pthread_attr_destroy(&gattr);
#else
            static_assert(false, "unsupported platform");
#endif
        }

        void die() __attribute__((always_inline));
        void dequeue_ready_atomic(states newstat = states::READY);
        vcpu_t* get_vcpu() {
            return (vcpu_t*)vcpu;
        }
        bool operator < (const thread &rhs) {
            return this->ts_wakeup < rhs.ts_wakeup;
        }
        void dispose() {
            assert(state == states::DONE);
            // `buf` and `stack_size` will always store on register
            // when calling deallocating.
            photon_thread_dealloc(buf, stack_size);
        }
    };

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    static_assert(offsetof(thread, vcpu) == offsetof(partial_thread, vcpu), "...");
    static_assert(offsetof(thread,  tls) == offsetof(partial_thread,  tls), "...");
    static_assert(offsetof(thread,flags) == offsetof(partial_thread,flags), "...");
#pragma GCC diagnostic pop

    struct thread_list : public intrusive_list<thread>
    {
        spinlock lock;
        thread_list() = default;
        thread_list(thread* head) {
            this->node = head;
        }
        thread* eject_whole_atomic() {
            if (!node) return nullptr;
            SCOPED_LOCK(lock);
            return eject_whole();
        }
        thread* eject_whole() {
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

    // A special spinlock that distinguishes a foreground vCPU among
    // background vCPUs, and makes the foreground as fast as possible.
    // Generic spinlock uses atomic exchange to obtain the lock, which
    // depends on bus lock and costs much CPU cycles than this design.
    class asymmetric_spinLock {
        // TODO: better place the two atomics in separate cache line?
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

    struct vcpu_t0 : public vcpu_base {
// offset 16B
        SleepQueue sleepq;  // sizeof(sleepq) should be 24: ptr, size and capcity
// offset 40B
        asymmetric_spinLock runq_lock;
        uint8_t flags = 0;
        uint8_t state = states::RUNNING;
        std::atomic<uint32_t> nthreads{1};
// offset 48B
        thread* idle_worker;
        // threads scheduled by other vCPUs are added to standbyq by those vCPUs,
        // then moved to runq later by this vCPU at some proper occasion.
        thread_list standbyq;
    };
    struct vcpu_t : public vcpu_t0, intrusive_list_node<vcpu_t> {
// offset 64B
        // vcpu_t *prev, *next;  // embedded in intrusive_list_node
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

        NullEventEngine _default_event_engine;
        bool is_master_event_engine_default() {
            return &_default_event_engine == master_event_engine;
        }
        void reset_master_event_engine_default() {
            auto& mee = master_event_engine;
            if (&_default_event_engine == mee) return;
            delete mee;
            mee = &_default_event_engine;
        }

        static spinlock vcpu_list_lock;    // lock when add, remove, iterate next
        static rwlock vcpu_list_rwlock;    // rlock when iterate, wlock when remove
        static intrusive_list<vcpu_t, false> pvcpu;
        vcpu_t(uint8_t flags_) {
            flags = flags_;
            master_event_engine = &_default_event_engine;
            SCOPED_LOCK(vcpu_list_lock);
            pvcpu.push_back(this);
        }
        void go_offline() { // by removing this from list
            scoped_rwlock _(vcpu_list_rwlock, WLOCK);
            SCOPED_LOCK(vcpu_list_lock);
            pvcpu.erase(this);
        }
    };
    spinlock vcpu_t::vcpu_list_lock;
    rwlock vcpu_t::vcpu_list_rwlock;
    intrusive_list<vcpu_t, false> vcpu_t::pvcpu;

    class RunQ {
    public:
        thread** pc = &CURRENT;
        mutable thread* current;
        RunQ() {
            asm volatile ("" : "=r"(pc) : "0"(pc));
            current = *pc;
        }
    };

    struct Switch { thread *from, *to; };

    class AtomicRunQ : public RunQ {
    public:
        vcpu_t* vcpu;
        mutable asymmetric_spinLock* plock;
        AtomicRunQ(const RunQ& runq = RunQ()) : RunQ(runq) {
            vcpu = current->get_vcpu();
            (plock = &vcpu->runq_lock) -> foreground_lock();
        }
        mutable bool update_current = false;
        void set_current(thread* th) const {
            current = th;
            update_current = true;
        }
        ~AtomicRunQ() {
            if (update_current)
                *pc = current;
            plock->foreground_unlock();
        }
        static void prefetch_context(thread* from, thread* to)
        {
#ifdef CONTEXT_PREFETCHING
            const int CACHE_LINE_SIZE = 64;
            auto f = *from->stack.pointer_ref();
            __builtin_prefetch(f, 1);
            // __builtin_prefetch((char*)f + CACHE_LINE_SIZE, 1);
            auto t = *to->stack.pointer_ref();
            __builtin_prefetch(t, 0);
            // __builtin_prefetch((char*)t + CACHE_LINE_SIZE, 0);
#endif
        }
        Switch remove_current(states new_state) const {
            assert(!current->single());
            auto from = current;
            auto to = from->remove_from_list();
            set_current(to);
            prefetch_context(from, to);
            from->state = new_state;
            to->state = states::RUNNING;
            return {from, to};
        }
        Switch _do_goto(thread* to) const {
            auto from = current;
            prefetch_context(from, to);
            from->state = states::READY;
            to->state = states::RUNNING;
            set_current(to);
            return {from, to};
        }
        Switch goto_next() const {
            assert(!current->single());
            return _do_goto(current->next());
        }
        Switch try_goto(thread* th) const {
            assert(th->vcpu == vcpu);
            return _do_goto(th);
        }
        bool single() const {
            return current->single();
        }
        bool size_1or2() const {
            return current->next() == current->prev();
        }
        void insert_tail(thread* th) const {
            current->insert_tail(th);
        }
        void insert_list_before(thread* th) const {
            current->insert_list_before(th);
        }
        void insert_list_before(thread_list& list) const {
            current->insert_list_before(list.node);
            list.node = nullptr;
        }
        void remove_from_list(thread* th) const {
            assert(th->state == states::READY);
            assert(th->vcpu == vcpu);
            assert(th != current);
            th->remove_from_list();
        }
        bool defer_to_new_thread() const {
            auto idle_worker = vcpu->idle_worker;
            if (current->next() == idle_worker) {
                if (idle_worker->next() == current) {
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

    static void spinlock_unlock(void* m_);

    inline void prepare_switch(thread* from, thread* to) {
        assert(from->vcpu == to->vcpu);
        assert(to->state == states::RUNNING);
        auto& cnt = to->get_vcpu()->switch_count;
        (*(uint64_t*)&cnt)++;   // increment of volatile variable is deprecated
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    // the offsets are used in _photon_thread_stub() assembly code
    static_assert(offsetof(thread, arg)   == 0x40, "...");
    static_assert(offsetof(thread, start) == 0x48, "...");
#pragma GCC diagnostic pop

#if defined(__x86_64__)
#if !defined(_WIN64)
    asm(
DEF_ASM_FUNC(_photon_switch_context) // (void** rdi_to, void** rsi_from)
R"(
        push    %rbp
        mov     %rsp, (%rsi)
        mov     (%rdi), %rsp
        pop     %rbp
        ret
)"

DEF_ASM_FUNC(_photon_switch_context_defer) // (void* rdi_arg, void (*rsi_defer)(void*), void** rdx_to, void** rcx_from)
R"(
        push    %rbp
        mov     %rsp, (%rcx)
)"

DEF_ASM_FUNC(_photon_switch_context_defer_die) // (void* rdi_arg, void (*rsi_defer)(void*), void** rdx_to_th)
R"(
        mov     (%rdx), %rsp
        pop     %rbp
        jmp     *%rsi
)"

DEF_ASM_FUNC(_photon_thread_stub)
R"(
        mov     0x40(%rbp), %rdi
        movq    $0, 0x40(%rbp)
        call    *0x48(%rbp)
        mov     %rax, 0x48(%rbp)
        mov     %rbp, %rdi
        call    _photon_thread_die
)"
    );

    inline void switch_context(thread* from, thread* to) {
        ASAN_SWITCH(to);
        prepare_switch(from, to);
        auto _t_ = to->stack.pointer_ref();
        register auto f asm("rsi") = from->stack.pointer_ref();
        register auto t asm("rdi") = _t_;
        asm volatile("call _photon_switch_context"  // (to, from)
                     : "+r"(t), "+r"(f)
                     :  // "0"(t), "1"(f)
                     : "rax", "rbx", "rcx", "rdx", "r8", "r9", "r10", "r11",
                       "r12", "r13", "r14", "r15");
    }

    inline void switch_context_defer(thread* from, thread* to,
                                     void (*defer)(void*), void* arg) {
        ASAN_SWITCH(to);
        prepare_switch(from, to);
        auto _t_ = to->stack.pointer_ref();
        register auto f asm("rcx") = from->stack.pointer_ref();
        register auto t asm("rdx") = _t_;
        register auto a asm("rdi") = arg;
        register auto d asm("rsi") = defer;
        asm volatile(
            "call _photon_switch_context_defer"  // (arg, defer, to, from)
            : "+r"(t), "+r"(f), "+r"(a), "+r"(d)
            :  // "0"(t), "1"(f), "2"(a), "3"(d)
            : "rax", "rbx", "r8", "r9", "r10", "r11", "r12", "r13", "r14",
              "r15");
    }
#else // _WIN64
    asm(
DEF_ASM_FUNC(_photon_switch_context) // (void** rcx_to, void** rdx_from)
R"(
        push    %rbp
        mov     %rsp, (%rdx)
        mov     (%rcx), %rsp
        pop     %rbp
        ret
)"

DEF_ASM_FUNC(_photon_switch_context_defer) // (void* rcx_arg, void (*rdx_defer)(void*), void** r8_to, void** r9_from)
R"(
        push    %rbp
        mov     %rsp, (%r9)
)"

DEF_ASM_FUNC(_photon_switch_context_defer_die) // (void* rcx_arg, void (*rdx_defer)(void*), void** r8_to)
R"(
        mov     (%r8), %rsp
        pop     %rbp
        jmp     *%rdx
)"

DEF_ASM_FUNC(_photon_thread_stub)
R"(
        call    _asan_start
        mov     0x40(%rbp), %rcx
        movq    $0, 0x40(%rbp)
        call    *0x48(%rbp)
        mov     %rax, 0x48(%rbp)
        mov     %rbp, %rcx
        call    _photon_thread_die
)"
    );

    inline void switch_context(thread* from, thread* to) {
        ASAN_SWITCH(to);
        prepare_switch(from, to);
        auto _t_ = to->stack.pointer_ref();
        register auto f asm("rdx") = from->stack.pointer_ref();
        register auto t asm("rcx") = _t_;
        asm volatile("call _photon_switch_context"  // (to, from)
                     : "+r"(t), "+r"(f)
                     :  // "0"(t), "1"(f)
                     : "rax", "rbx", "rsi", "rdi", "r8", "r9",
                       "r10", "r11", "r12", "r13", "r14", "r15",
                       "xmm6",  "xmm7",  "xmm8",  "xmm9",  "xmm10",
                       "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    }

    inline void switch_context_defer(thread* from, thread* to,
                                     void (*defer)(void*), void* arg) {
        ASAN_SWITCH(to);
        prepare_switch(from, to);
        auto _t_ = to->stack.pointer_ref();
        register auto f asm("r9") = from->stack.pointer_ref();
        register auto t asm("r8") = _t_;
        register auto a asm("rcx") = arg;
        register auto d asm("rdx") = defer;
        asm volatile(
            "call _photon_switch_context_defer"  // (arg, defer, to, from)
            : "+r"(t), "+r"(f), "+r"(a), "+r"(d)
            :  // "0"(t), "1"(f), "2"(a), "3"(d)
            : "rax", "rbx", "rsi", "rdi", "r10",
              "r11", "r12", "r13", "r14", "r15",
              "xmm6",  "xmm7",  "xmm8",  "xmm9",  "xmm10",
              "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    }
#endif // _WIN64

#elif defined(__aarch64__) || defined(__arm64__)

    asm(
DEF_ASM_FUNC(_photon_switch_context) // (void** x0_from, void** x1_to)
R"(
        stp x29, x30, [sp, #-16]!
        mov x29, sp
        str x29, [x0]
        ldr x29, [x1]
        mov sp, x29
        ldp x29, x30, [sp], #16
        ret
)"

DEF_ASM_FUNC(_photon_switch_context_defer) // (void* x0_arg, void (*x1_defer)(void*), void** x2_to, void** x3_from)
R"(
        stp x29, x30, [sp, #-16]!
        mov x29, sp
        str x29, [x3]
)"

DEF_ASM_FUNC(_photon_switch_context_defer_die) // (void* x0_arg, void (*x1_defer)(void*), void** x2_to_th)
R"(
        ldr x29, [x2]
        mov sp, x29
        ldp x29, x30, [sp], #16
        br x1
)"

DEF_ASM_FUNC(_photon_thread_stub)
R"(
        bl _asan_start           //; asan_start()
        ldp x0, x1, [x29, #0x40] //; load arg, start into x0, x1
        str xzr, [x29, #0x40]    //; set arg as 0
        blr x1                   //; start(x0)
        str x0, [x29, #0x48]     //; retval = result
        mov x0, x29              //; move th to x0
        b _photon_thread_die     //; _photon_thread_die(th)
)"
    );

#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winline-asm"
#endif

    inline void switch_context(thread* from, thread* to) {
        ASAN_SWITCH(to);
        prepare_switch(from, to);
        auto _t_ = to->stack.pointer_ref();
        register auto f asm("x0") = from->stack.pointer_ref();
        register auto t asm("x1") = _t_;
        asm volatile("bl _photon_switch_context"
                     : "+r"(t), "+r"(f)
                     :  // "0"(t), "1"(f)
                     :  // Callee saved register, make it as clobber to save
                     "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26",
                     "x27", "x28", "x29", "x30",
                     // Caller saved register, might change, should save
                     "x2", "x3", "x4", "x5", "x6", "x7", "x8",
                     // Corouptable register, may change ,should save
                     "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
                     "x17", "x18");
    }

    inline void switch_context_defer(thread* from, thread* to,
                                     void (*defer)(void*), void* arg) {
        ASAN_SWITCH(to);
        prepare_switch(from, to);
        auto _t_ = to->stack.pointer_ref();
        register auto f asm("x3") = from->stack.pointer_ref();
        register auto t asm("x2") = _t_;
        register auto d asm("x1") = defer;
        register auto a asm("x0") = arg;
        asm volatile("bl _photon_switch_context_defer"
                     : "+r"(t), "+r"(f), "+r"(a), "+r"(d)
                     :  // "0"(t), "1"(f), "2"(a), "3"(d)
                     : "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26",
                       "x27", "x28", "x29", "x30", "x4", "x5", "x6", "x7", "x8",
                       "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
                       "x17", "x18");
    }
#ifdef __clang__
#pragma GCC diagnostic pop
#endif

#endif  // x86 or arm

    extern "C" __attribute__((noreturn))
    void _photon_switch_context_defer_die(void* arg, uint64_t defer_func_addr, void** to)
    asm("_photon_switch_context_defer_die");

    __attribute__((noreturn))
    inline void thread::die() {
        deallocate_tls(&tls);
        // if CURRENT is idle stub and during vcpu_fini
        // main thread waiting for idle stub joining, now idle might be only
        // thread in run-queue. To keep going, wake up waiter before remove
        // current from run-queue.
        lock.lock();
        state = states::DONE;
        cond.notify_one();
        get_vcpu()->nthreads--;
        auto sw = AtomicRunQ().remove_current(states::DONE);
        assert(this == sw.from);
        uint64_t func;
        void* arg;
        if (!is_joinable()) {
            auto f = &thread::dispose;
            func = (uint64_t&)f;
            arg = this;
        } else {
            func = (uint64_t)&spinlock_unlock;
            arg = &lock;
        }
        ASAN_DIE_SWITCH(sw.to);
        _photon_switch_context_defer_die(
            arg, func, sw.to->stack.pointer_ref());
        __builtin_unreachable();
    }

    extern "C" __attribute__((noreturn))
    void _photon_thread_die(thread* th) asm("_photon_thread_die");
    void _photon_thread_die(thread* th) {
        assert(th == CURRENT);
        th->die();
        __builtin_unreachable();
    }

    // the function is written in assembly to make use of the fact
    // that CURRENT is passed in via rbp/x29 as the first argument,
    // eliminating a TLS access.
    extern "C" __attribute__((noreturn))
    void _photon_thread_stub() asm("_photon_thread_stub");

    thread* thread_create(thread_entry start, void* arg, uint64_t stack_size,
                uint32_t reserved_space, uint64_t flags) {
        if (!stack_size)
            stack_size = DEFAULT_STACK_SIZE;
        if (unlikely((uint64_t)reserved_space > stack_size / 2))
            LOG_ERROR_RETURN(EINVAL, nullptr, VALUE(reserved_space), "must be <= half of ", VALUE(stack_size));
        uint64_t FLAGS = THREAD_JOINABLE | THREAD_ENABLE_WORK_STEALING;
        if (unlikely(flags & ~FLAGS))
            LOG_ERRNO_RETURN(EINVAL, nullptr, "invalid flags", HEX(flags));
        RunQ rq;
        if (unlikely(!rq.current))
            LOG_ERROR_RETURN(ENOSYS, nullptr, "Photon not initialized in this vCPU (OS thread)");
        size_t randomizer = (rand() % 512) * 8;
        // stack contains struct, randomizer space, and reserved_space
        size_t least_stack_size = sizeof(thread) + randomizer + 63 + reserved_space;
        // at least a whole page for mprotect
        least_stack_size += PAGE_SIZE;
        // and make sure it's at least 16K and aligned to page size
        least_stack_size = align_up(std::max(16UL * 1024, least_stack_size), PAGE_SIZE);
        stack_size = align_up(stack_size, PAGE_SIZE);
        if (unlikely(stack_size < least_stack_size)) {
            LOG_WARN(VALUE(stack_size), " is less than minimal value `, use the minimal value instead", least_stack_size);
            stack_size = least_stack_size;
        }
        char* ptr = (char*)photon_thread_alloc(stack_size);
        if (unlikely(!ptr))
            return nullptr;
        uint64_t p = (uint64_t)ptr + stack_size - sizeof(thread) - randomizer;
        p = align_down(p, 64);
        auto th = new ((char*)p) thread;
        th->buf = ptr;
        th->stackful_alloc_top = ptr + PAGE_SIZE;
        th->start = start;
        th->stack_size = stack_size;
        th->arg = arg;
        th->flags = flags;
        auto sp = align_down(p - reserved_space, 64);
        th->stack.init((void*)sp, &_photon_thread_stub, th);
        AtomicRunQ arq(rq);
        th->vcpu = arq.vcpu;
        arq.vcpu->nthreads++;
        arq.insert_tail(th);
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
    static inline NowTime update_now()
    {
#if defined(__x86_64__) && defined(__linux__) && defined(ENABLE_MIMIC_VDSO)
        if (likely(__mimic_vdso_time_x86))
            return photon::now = __mimic_vdso_time_x86.get_now();
#endif
        struct timespec tv;
#ifdef CLOCK_BOOTTIME
        clock_gettime(CLOCK_BOOTTIME, &tv);
#else
        clock_gettime(CLOCK_MONOTONIC, &tv);
#endif
        auto usec = tv.tv_nsec / 1000ul;
        uint64_t nnow = tv.tv_sec * 1000ul * 1000ul + usec;
        assert(tv.tv_sec <= UINT32_MAX && usec < 1000000);
        now = nnow;
        return {nnow, ((uint64_t)tv.tv_sec << 32) | (uint32_t)usec};
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
        uint64_t freq;
        asm volatile("mrs %0, cntvct_el0" : "=r" (val));
        asm volatile("mrs %0, cntfrq_el0" : "=r" (freq));
        // cycles * microsec_per_sec / frequency = microsec
        return (val << 10) / freq;
    #endif
    }
    static uint32_t last_tsc = 0;
    static inline bool if_update_now(bool accurate = false) {
#if defined(__x86_64__) && defined(__linux__) && defined(ENABLE_MIMIC_VDSO)
        if (likely(__mimic_vdso_time_x86)) {
            return photon::now = __mimic_vdso_time_x86.get_now(accurate);
        }
#endif
        if (likely(ts_updater.load(std::memory_order_relaxed))) {
            return true;
        }
        if (unlikely(accurate)) {
            update_now();
            return true;
        }
        uint32_t tsc = _rdtsc();
        if (unlikely(last_tsc != tsc)) {
            last_tsc = tsc;
            update_now();
            return true;
        }
        return false;
    }
    NowTime __update_now() {
        last_tsc = _rdtsc();
        return update_now();
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

    inline __attribute__((always_inline))
    int resume_threads_inlined(vcpu_t* vcpu, const RunQ& runq)
    {
        int count = 0;
        thread_list list;
        auto& standbyq = vcpu->standbyq;
        auto& sleepq = vcpu->sleepq;
        // threads interrupted by other vcpus were not popped from sleepq
        if ((list.node = standbyq.eject_whole_atomic())) {
            for (auto th: list) {
                assert(th->state == states::STANDBY);
                th->state = states::READY;
                sleepq.pop(th);
                count++;
            }
            goto insert_list;
        }
        if (likely(sleepq.empty() || !if_update_now())) {
            assert(count == 0);
            return count;
        }
        do {
            auto th = sleepq.front();
            if (th->ts_wakeup > now) break;
            SCOPED_LOCK(th->lock);
            sleepq.pop_front();
            if (likely(th->state == states::SLEEPING)) {
                th->dequeue_ready_atomic();
                list.push_back(th);
                count++;
            } else assert(({ // th got interrupted just after standbyq.eject_whole_atomic()
                SCOPED_LOCK(standbyq.lock);      // we should leave it in standbyq
                th->state == states::STANDBY &&  // and process it in batch next time
                standbyq.contains(th);
            }));
        } while(!sleepq.empty());
        if (count) {
insert_list:
            AtomicRunQ(runq).insert_list_before(list);
        }
        return count;
    }
    __attribute__((noinline))
    static int resume_threads(vcpu_t* vcpu, const RunQ& runq) {
        return resume_threads_inlined(vcpu, runq);
    }

    states thread_stat(thread* th)
    {
        return (states) th->state;
    }

    int thread_yield()
    {
        RunQ rq;
        if_update_now();
        rq.current->error_number = 0;
        auto sw = AtomicRunQ(rq).goto_next();
        switch_context(sw.from, sw.to);
        return rq.current->error_number;
    }

    inline void thread_yield_fast() {
        auto sw = AtomicRunQ().goto_next();
        switch_context(sw.from, sw.to);
    }

    int thread_yield_to(thread* th) {
        if (unlikely(th == nullptr)) { // yield to any thread
            return thread_yield();
        }
        RunQ rq;
        if (unlikely(th == rq.current)) { // yield to current should just update time
            if_update_now();
            return 0;
        } else if (unlikely(th->vcpu != rq.current->vcpu)) {
            LOG_ERROR_RETURN(EINVAL, -1, "target thread ` must be run by the same vcpu as CURRENT!", th);
        } else if (unlikely(th->state == states::STANDBY)) {
            while (th->state == states::STANDBY)
                resume_threads(th->get_vcpu(), rq);
            assert(th->state == states::READY);
        } else if (unlikely(th->state != states::READY)) {
            LOG_ERROR_RETURN(EINVAL, -1, "target thread ` must be READY!", th);
        }

        auto sw = AtomicRunQ(rq).try_goto(th);
        if_update_now();
        rq.current->error_number = 0;
        switch_context(sw.from, sw.to);
        return rq.current->error_number;
    }

    __attribute__((always_inline)) inline
    Switch prepare_usleep(Timeout timeout, thread_list* waitq, RunQ rq = {})
    {
        spinlock* waitq_lock = waitq ? &waitq->lock : nullptr;
        SCOPED_LOCK(waitq_lock, ((bool) waitq) * 2);
        SCOPED_LOCK(rq.current->lock);
        assert(!AtomicRunQ(rq).single());
        auto sw = AtomicRunQ(rq).remove_current(states::SLEEPING);
        if (waitq) {
            waitq->push_back(sw.from);
            sw.from->waitq = waitq;
        }
        if_update_now(true);
        sw.from->ts_wakeup = timeout.expiration();
        sw.from->get_vcpu()->sleepq.push(sw.from);
        return sw;
    }
    inline int yield_as_sleep() {
        int ret = thread_yield();
        if (unlikely(ret)) { errno = ret; return -1; }
        return 0;
    }
    // returns 0 if slept well (at lease `useconds`), -1 otherwise
    static int thread_usleep(Timeout timeout, thread_list* waitq)
    {
        if (unlikely(timeout.expired())) {
            return yield_as_sleep();
        }

        auto r = prepare_usleep(timeout, waitq);
        switch_context(r.from, r.to);
        assert(r.from->waitq == nullptr);
        return r.from->set_error_number();
    }

    typedef void (*defer_func)(void*);
    static int thread_usleep_defer(Timeout timeout,
        thread_list* waitq, defer_func defer, void* defer_arg)
    {
        auto r = prepare_usleep(timeout, waitq);
        switch_context_defer(r.from, r.to, defer, defer_arg);
        assert(r.from->waitq == nullptr);
        return r.from->set_error_number();
    }

    __attribute__((noinline))
    static int do_thread_usleep_defer(Timeout timeout,
            defer_func defer, void* defer_arg, RunQ rq) {
        auto r = prepare_usleep(timeout, nullptr, rq);
        switch_context_defer(r.from, r.to, defer, defer_arg);
        assert(r.from->waitq == nullptr);
        return r.from->set_error_number();
    }
    static int do_shutdown_usleep_defer(Timeout timeout,
                defer_func defer, void* defer_arg, RunQ rq) {
        timeout.timeout_at_most(10 * 1000);
        int ret = do_thread_usleep_defer(timeout, defer, defer_arg, rq);
        if (ret >= 0)
            errno = EPERM;
        return -1;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    int thread_usleep_defer(Timeout timeout, defer_func defer, void* defer_arg) {
        RunQ rq;
        if (unlikely(!rq.current))
            LOG_ERROR_RETURN(ENOSYS, -1, "Photon not initialized in this thread");
        if (unlikely(AtomicRunQ(rq).defer_to_new_thread())) {
            thread_create((thread_entry&)defer, defer_arg);
            return thread_usleep(timeout);
        }
        if (unlikely(rq.current->is_shutting_down()))
            return do_shutdown_usleep_defer(timeout, defer, defer_arg, rq);
        return do_thread_usleep_defer(timeout, defer, defer_arg, rq);
    }
#pragma GCC diagnostic pop

    __attribute__((noinline))
    static int do_thread_usleep(Timeout timeout, RunQ rq) {
        auto r = prepare_usleep(timeout, nullptr, rq);
        switch_context(r.from, r.to);
        assert(r.from->waitq == nullptr);
        return r.from->set_error_number();
    }
    static int do_shutdown_usleep(Timeout timeout, RunQ rq) {
        timeout.timeout_at_most(10 * 1000);
        int ret = do_thread_usleep(timeout, rq);
        if (ret >= 0)
            errno = EPERM;
        return -1;
    }
    int thread_usleep(Timeout timeout) {
        RunQ rq;
        if (unlikely(!rq.current))
            LOG_ERROR_RETURN(ENOSYS, -1, "Photon not initialized in this thread");
        if (unlikely(timeout.expired()))
            return yield_as_sleep();
        if (unlikely(rq.current->is_shutting_down()))
            return do_shutdown_usleep(timeout, rq);
        return do_thread_usleep(timeout, rq);
    }

    static void prelocked_thread_interrupt(thread* th, int error_number)
    {
        vcpu_t* vcpu = th->get_vcpu();
        assert(th && th->state == states::SLEEPING);
        assert("th->lock is locked");
        assert(th != CURRENT);
        th->error_number = error_number;
        RunQ rq;
        if (unlikely(!rq.current || vcpu != rq.current->get_vcpu())) {
            th->dequeue_ready_atomic(states::STANDBY);
            vcpu->move_to_standbyq_atomic(th);
        } else {
            th->dequeue_ready_atomic();
            vcpu->sleepq.pop(th);
            AtomicRunQ(rq).insert_tail(th);
        }
    }
    void thread_interrupt(thread* th, int error_number)
    {
        if (unlikely(!th))
            LOG_ERROR_RETURN(EINVAL, , "invalid parameter");
        auto state = th->state;
        if (unlikely(state != states::SLEEPING)) {
        out: // may have thread_yield()-ed
            if (state == states::READY && th->error_number == 0)
                th->error_number = error_number;
            return;
        }
        SCOPED_LOCK(th->lock);
        state = th->state;
        if (unlikely(state != states::SLEEPING)) goto out;

        prelocked_thread_interrupt(th, error_number);
    }

    static void do_stack_pages_gc(void* arg) {
#if !defined(_WIN64) && !defined(__aarch64__)
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
            auto next = AtomicRunQ().goto_next().to;
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
        th->set_joinable(flag);
        return (join_handle*)th;
    }

    void* thread_join(join_handle* jh) {
        auto th = (thread*)jh;
        assert(th->is_joinable());
        if (!th->is_joinable())
            LOG_ERROR_RETURN(ENOSYS, nullptr, "join is not enabled for thread ", th);

        th->lock.lock();
        while (th->state != states::DONE) {
            th->cond.wait(th->lock);
        }
        auto retval = th->retval;
        th->dispose();
        return retval;
    }
    inline void thread_join(thread* th)
    {
        thread_join((join_handle*)th);
    }
    void thread_exit(void* retval) {
        CURRENT->retval = retval;
        _photon_thread_die(CURRENT);
    }

    int thread_shutdown(thread* th, bool flag)
    {
        if (!th)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid thread");

        th->set_shutting_down(flag);
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
        return (*perrno == -1) ? 0 : -1;
    }
    int waitq::wait(Timeout timeout)
    {
        static_assert(sizeof(q) == sizeof(thread_list), "...");
        auto lst = (thread_list*)&q;
        int ret = thread_usleep(timeout, lst);
        return waitq_translate_errno(ret);
    }
    int waitq::wait_defer(Timeout timeout, void(*defer)(void*), void* arg) {
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
    int mutex::lock(Timeout timeout) {
        if (try_lock() == 0) return 0;
        for (auto re = retries; re; --re) {
            int ret = thread_yield();
            if (unlikely(ret)) { errno = ret; return -1; }
            if (try_lock() == 0) return 0;
        }
        splock.lock();
        if (try_lock() == 0) {
            splock.unlock();
            return 0;
        }

        if (timeout.expired()) {
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
            std::memory_order_acq_rel, std::memory_order_relaxed);
        return (int)ret - 1;
    }
    inline void do_mutex_unlock(mutex* m)
    {
        SCOPED_LOCK(m->splock);
        ScopedLockHead h(m);
        m->owner.store(h);
        if (h)
            prelocked_thread_interrupt(h, -1);
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
        if (unlikely(!th)) {
            if (unlikely(!CURRENT))
                return;
            LOG_ERROR_RETURN(EINVAL, , "the mutex was not locked");
        }
        if (unlikely(th != CURRENT))
            LOG_ERROR_RETURN(EINVAL, , "the mutex was not locked by current thread");
        do_mutex_unlock(this);
    }

    int recursive_mutex::lock(Timeout timeout) {
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
    static int cvar_do_wait(thread_list* q, void* m, Timeout timeout, int(*lock)(void*), void(*unlock)(void*)) {
        assert(m);
        if (!m)
            LOG_ERROR_RETURN(EINVAL, -1, "there must be a lock");
        int ret = thread_usleep_defer(timeout, q, unlock, m);
        auto en = ret < 0 ? errno : 0;
        while (true) {
            int lock_ret = lock(m);
            if (lock_ret == 0) break;
            LOG_ERROR("failed to get mutex lock, ` `, try again", VALUE(ret), ERRNO());
            thread_usleep(1000, nullptr);
        }
        if (ret < 0) errno = en;
        return waitq_translate_errno(ret);

    }
    int condition_variable::wait(mutex* m, Timeout timeout)
    {
        return cvar_do_wait((thread_list*)&q, m, timeout, mutex_lock, mutex_unlock);
    }
    int condition_variable::wait(spinlock* m, Timeout timeout)
    {
        return cvar_do_wait((thread_list*)&q, m, timeout, spinlock_lock, spinlock_unlock);
    }
    int semaphore::wait_interruptible(uint64_t count, Timeout timeout)
    {
        if (count == 0) return 0;
        splock.lock();
        DEFER(splock.unlock());
        auto& counter = CURRENT->semaphore_count;
        counter = count;
        DEFER(counter = 0);
        while (!try_subtract(count)) {
            int ret = waitq::wait_defer(timeout, spinlock_unlock, &splock);
            splock.lock();  // assuming errno NOT changed
            if (unlikely(ret < 0)) {    // got interrupted
                uint64_t cnt;
                if (!m_ooo_resume && (cnt = m_count.load())) {
                    auto eno = errno;
                    try_resume(cnt);
                    errno = eno;
                }
                return ret;
            }
        }
        return 0;
    }
    void semaphore::try_resume(uint64_t cnt) {
        assert(cnt);
        while(true) {
            ScopedLockHead h(this);
            if (!h) break;
            auto th = (thread*)h;
            auto& c = th->semaphore_count;
            if (c > cnt) break;
            cnt -= c;
            prelocked_thread_interrupt(th, -1);
        }
        if (!q.th || !cnt || !m_ooo_resume)
            return;
        SCOPED_LOCK(q.lock);
        for (auto th = q.th->next();
                  th!= q.th && cnt;
                  th = th->next()) {
            SCOPED_LOCK(th->lock);
            auto& c = th->semaphore_count;
            if (c <= cnt) {
                cnt -= c;
                prelocked_thread_interrupt(th, -1);
            }
        }
    }
    inline bool semaphore::try_subtract(uint64_t count) {
        while(true) {
            auto mc = m_count.load();
            if (mc < count)
                return false;
            auto new_mc = mc - count;
            if (m_count.compare_exchange_strong(mc, new_mc))
                return true;
        }
    }
    int rwlock::lock(int mode, Timeout timeout)
    {
        if (mode != RLOCK && mode != WLOCK)
            LOG_ERROR_RETURN(EINVAL, -1, "mode unknow");

        scoped_lock lock(mtx);
        auto mark = CURRENT->rwlock_mark;
        auto bkup = mark;   // backup mark
        DEFER(CURRENT->rwlock_mark = bkup);
        // mask mark bits, keep RLOCK WLOCK bit clean
        mark &= ~(RLOCK | WLOCK);
        mark |= mode;
        CURRENT->rwlock_mark = mark;
        uint64_t op = (mode == RLOCK) ? (1ULL << 63) : -1ULL;
        if (cvar.q.th || (op & state)) {
            do {
                int ret = cvar.wait(lock, timeout);
                if (ret < 0)
                    return -1; // break by timeout or interrupt
            } while (op & state);
        }
#if defined(__x86_64__)
        asm ("rol $1, %0" : "+r"(op) : "r"(op)); // rotate shift left by 1 bit
#elif defined(__aarch64__) || defined(__arm64__)
        asm ("ror %0, %0, #63" : "+r"(op) : "r"(op));
#endif
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
            if (cvar.q.th && (cvar.q.th->rwlock_mark & WLOCK)) {
                cvar.notify_one();
            } else
                while (cvar.q.th && (cvar.q.th->rwlock_mark & RLOCK)) {
                    cvar.notify_one();
                }
        }
        return 0;
    }
    void disposable_semaphore::defer(void* arg) {
        auto _this = (disposable_semaphore*)arg;
        _this->signal();
    }
    bool is_master_event_engine_default() {
        return CURRENT->get_vcpu()->is_master_event_engine_default();
    }
    void reset_master_event_engine_default() {
        CURRENT->get_vcpu()->reset_master_event_engine_default();
    }
    inline __attribute__((always_inline))
    thread* ws_scan_q(vcpu_t* v, thread* first, bool possibly_running) {
        // the first must be unstealable
        assert(!first->allow_work_stealing());
        thread_list stolen;
        uint64_t count = 0;
        auto th = first->next();
        while(th != first) {
            SCOPED_LOCK(th->lock);
            if ((possibly_running && th->state == states::RUNNING) ||
                                    !th->allow_work_stealing()) {
                th = th->next();
            } else {
                auto next = th->remove_from_list();
                stolen.push_back(th); count++;
                th->vcpu->nthreads--;
                th->vcpu = v;
                v->nthreads++;
                th = next;
            }
        }
        (void)count;
        return stolen.eject_whole();
    }
    inline __attribute__((always_inline))
    thread* ws_scan_standbyq(vcpu_t* v, vcpu_t* u) {
        auto& q = u->standbyq;
        if (q.empty()) return nullptr;
        SCOPED_LOCK(q.lock);
        if (q.empty()) return nullptr;
        if (!q.front()->allow_work_stealing())
            return ws_scan_q(v, q.front(), false);

        thread_list stolen;
        do {
            SCOPED_LOCK(q.front()->lock);
            auto th = q.pop_front();
            stolen.push_back(th);
            th->vcpu->nthreads--;
            th->vcpu = v;
            v->nthreads++;
        } while (q.front()->allow_work_stealing());
        return stolen.eject_whole();
    }
    inline __attribute__((always_inline))
    thread* ws_scan_runq(vcpu_t* v, vcpu_t* u) {
        if (!u->runq_lock.background_try_lock()) return 0;
        DEFER(u->runq_lock.background_unlock());
        return ws_scan_q(v, u->idle_worker, true);
    }
    static bool try_work_stealing(vcpu_t* vcpu) {
        assert(CURRENT->get_vcpu() == vcpu);
        assert(CURRENT == vcpu->idle_worker);
        if (0 == (vcpu->flags & VCPU_ENABLE_ACTIVE_WORK_STEALING))
            return false;
        scoped_rwlock _(vcpu_t::vcpu_list_rwlock, RLOCK);
        auto u = vcpu->next();
        while (u != vcpu) {
            if (0 == (u->flags & VCPU_ENABLE_PASSIVE_WORK_STEALING))
                continue;
            thread* th;
            if ((th = ws_scan_standbyq(vcpu, u)) || (th = ws_scan_runq(vcpu, u))) {
                vcpu->idle_worker->insert_list_tail(th);
                return true;
            }
            SCOPED_LOCK(vcpu_t::vcpu_list_lock);
            u = u->next();
        }
        return false;
    }
    static void* idler(void*) {
        RunQ rq;
        auto last_idle = now;
        auto vcpu = rq.current->get_vcpu();
        while (vcpu->state != states::DONE) {
            while (unlikely(resume_threads_inlined(vcpu, rq) > 0) ||
                   likely(!AtomicRunQ(rq).single())   ||
                   likely(try_work_stealing(vcpu))) {
                thread_yield();
                if (vcpu->state == states::DONE)
                    break;
                if (unlikely(sat_sub(now, last_idle) >= 1000UL)) {
                    vcpu->master_event_engine->wait_and_fire_events(0);
                    last_idle = now;
                }
            }
            if (vcpu->state == states::DONE)
                break;
            // only idle stub aliving
            // other threads must be sleeping
            // fall in actual sleep
            auto usec = 10 * 1024 * 1024; // max
            auto& sleepq = vcpu->sleepq;
            if (!sleepq.empty()) usec = min(usec,
                sat_sub(sleepq.front()->ts_wakeup, now));
            vcpu->master_event_engine->wait_and_fire_events(usec);
            last_idle = now;
        }
        return nullptr;
    }
    /**
     * Waiting for all current vcpu photon threads finish
     * @return 0 for all done, -1 for failure
     */
    static int wait_all(RunQ rq, vcpu_t* vcpu) {
        auto &sleepq = vcpu->sleepq;
        auto &standbyq = vcpu->standbyq;
        while (!AtomicRunQ(rq).size_1or2() || !sleepq.empty() || !standbyq.empty()) {
            if (!sleepq.empty()) {
                // sleep till all sleeping threads ends
                thread_usleep(1000UL);
            } else {
                thread_yield();
            }
        }
        return 0;
    }

    int wait_all() {
        RunQ rq;
        return wait_all(rq, rq.current->get_vcpu());
    }

    struct migrate_args {thread* th; vcpu_base* v;};
    static int do_thread_migrate(thread* th, vcpu_base* v);
    static void do_defer_migrate(void* m_) {
        auto m = (migrate_args*)m_;
        do_thread_migrate(m->th, m->v);
    }
    static int defer_migrate_current(vcpu_base* v) {
        auto sw = AtomicRunQ().goto_next();
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
        AtomicRunQ().remove_from_list(th);
        th->get_vcpu()->nthreads--;
        th->state = STANDBY;
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

    int vcpu_init(uint64_t flags) {
        uint64_t FLAGS = VCPU_ENABLE_ACTIVE_WORK_STEALING |
                         VCPU_ENABLE_PASSIVE_WORK_STEALING;
        if (unlikely(flags & ~FLAGS))
            LOG_ERROR_RETURN(EINVAL, -1, "invalid flags ", HEX(flags));
        if (unlikely(PAGE_SIZE == 0))
            PAGE_SIZE = getpagesize();
        RunQ rq;
        if (rq.current) return -1;      // re-init has no side-effect
        char* ptr = nullptr;
        int err = posix_memalign((void**)&ptr, 64, sizeof(vcpu_t));
        if (unlikely(err))
            LOG_ERROR_RETURN(err, -1, "Failed to allocate vcpu ", ERRNO(err));

        auto th = *rq.pc = new thread;
        th->vcpu = (vcpu_t*)ptr;
        th->state = states::RUNNING;
        th->init_main_thread_stack();
        auto vcpu = new (ptr) vcpu_t(uint8_t(flags & FLAGS));
        vcpu->idle_worker = thread_create(&idler, nullptr);
        thread_enable_join(vcpu->idle_worker);
        if_update_now(true);
        return ++_n_vcpu;
    }

    int vcpu_fini() {
        RunQ rq;
        if (!rq.current)
            LOG_ERROR_RETURN(ENOSYS, -1, "vcpu not initialized");

        auto vcpu = rq.current->get_vcpu();
        wait_all(rq, vcpu);
        assert(!AtomicRunQ(rq).single());
        assert(vcpu->nthreads == 2); // idler & current alive
        deallocate_tls(&rq.current->tls);
        vcpu->go_offline();
        vcpu->state = states::DONE;  // instruct idle_worker to exit
        thread_join(vcpu->idle_worker);
        rq.current->state = states::DONE;
        delete rq.current;
        *rq.pc = nullptr;
        vcpu->~vcpu_t();
        free(vcpu);
        return --_n_vcpu;
    }

    void* stackful_malloc(size_t size) {
        return CURRENT->stackful_malloc(size);
    }

    void stackful_free(void* ptr) {
        CURRENT->stackful_free(ptr);
    }

    void set_photon_thread_stack_allocator(
        Delegate<void *, size_t> _photon_thread_alloc,
        Delegate<void, void *, size_t> _photon_thread_dealloc) {
        photon_thread_alloc = _photon_thread_alloc;
        photon_thread_dealloc = _photon_thread_dealloc;
    }

    extern "C" {
    [[gnu::used]]
    void *gdb_get_thread_stack_ptr(void *th) {
      if (!th)
        return nullptr;
      return ((thread *)th)->stack._ptr;
    }
    [[gnu::used]]
    void *gdb_get_current_thread() {
      return CURRENT;
    }
    [[gnu::used]]
    void *gdb_get_next_thread(void *c) {
      if (!c)
        return nullptr;
      return ((thread *)c)->next();
    }
    [[gnu::used]]
    void *gdb_get_vcpu(void *th) {
      if (!th)
        return nullptr;
      return (void *)((thread *)th)->vcpu;
    }
    [[gnu::used]]
    size_t gdb_get_sleepq_size(void *vcpu) {
      if (!vcpu)
        return 0;
      return ((vcpu_t *)vcpu)->sleepq.q.size();
    }
    [[gnu::used]]
    void *gdb_get_sleepq_item(void *vcpu, size_t idx) {
      if (!vcpu)
        return nullptr;
      if (((vcpu_t *)vcpu)->sleepq.q.size() <= idx)
        return nullptr;
      return ((vcpu_t *)vcpu)->sleepq.q[idx];
    }
    }
}
