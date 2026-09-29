/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WslotsANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#ifndef __aarch64__
#include <immintrin.h>
#endif

#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>

#define size_t uint64_t

template <size_t x>
struct Capacity_2expN {
    constexpr static size_t capacity = Capacity_2expN<(x >> 1)>::capacity << 1;
    constexpr static size_t mask = capacity - 1;
    constexpr static size_t shift = Capacity_2expN<(x >> 1)>::shift + 1;
    constexpr static size_t lshift = Capacity_2expN<(x >> 1)>::lshift - 1;

    static_assert(shift + lshift == sizeof(size_t) * 8, "...");
};

template <size_t x>
constexpr size_t Capacity_2expN<x>::capacity;

template <size_t x>
constexpr size_t Capacity_2expN<x>::mask;

template <>
struct Capacity_2expN<0> {
    constexpr static size_t capacity = 2;
    constexpr static size_t mask = 1;
    constexpr static size_t shift = 1;
    constexpr static size_t lshift = 8 * sizeof(size_t) - shift;
};

template <>
struct Capacity_2expN<1> : public Capacity_2expN<0> {};

template <>
struct Capacity_2expN<2> : public Capacity_2expN<0> {};

struct PauseBase {};

struct CPUPause : PauseBase {
    inline static __attribute__((always_inline)) void pause() {
#ifdef __aarch64__
        asm volatile("isb" : : : "memory");
#else
        _mm_pause();
#endif
    }
};

struct ThreadPause : PauseBase {
    inline static __attribute__((always_inline)) void pause() {
        std::this_thread::yield();
    }
};

namespace photon {
int thread_yield();
}
struct PhotonPause : PauseBase {
    inline static __attribute__((always_inline)) void pause() {
        photon::thread_yield();
    }
};

template <typename T>
struct is_shared_ptr : std::false_type {};
template <typename T>
struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};

template <typename T, size_t N>
class LockfreeRingQueueBase {
public:
#if __cplusplus < 201402L
    static_assert((std::has_trivial_copy_constructor<T>::value &&
                   std::has_trivial_copy_assign<T>::value) ||
                      is_shared_ptr<T>::value,
                  "T should be trivially copyable");
#else
    static_assert((std::is_trivially_copy_constructible<T>::value &&
                   std::is_trivially_copy_assignable<T>::value) ||
                      is_shared_ptr<T>::value,
                  "T should be trivially copyable");
#endif

    constexpr static size_t CACHELINE_SIZE = 64;

    constexpr static size_t capacity = Capacity_2expN<N>::capacity;
    constexpr static size_t mask = Capacity_2expN<N>::mask;
    constexpr static size_t shift = Capacity_2expN<N>::shift;
    constexpr static size_t lshift = Capacity_2expN<N>::lshift;

    alignas(CACHELINE_SIZE) std::atomic<size_t> tail{0};
    alignas(CACHELINE_SIZE) std::atomic<size_t> head{0};

    bool empty() {
        return check_empty(head.load(std::memory_order_relaxed),
                           tail.load(std::memory_order_relaxed));
    }

    bool full() {
        return check_full(head.load(std::memory_order_relaxed),
                          tail.load(std::memory_order_relaxed));
    }

    size_t read_available() const {
        return tail.load(std::memory_order_relaxed) -
               head.load(std::memory_order_relaxed);
    }

    size_t write_available() const {
        return head.load(std::memory_order_relaxed) + capacity -
               tail.load(std::memory_order_relaxed);
    }

protected:
    bool check_mask_equal(size_t x, size_t y) const {
        return (x << lshift) == (y << lshift);
    }

    bool check_empty(size_t h, size_t t) const { return h == t; }

    bool check_full(size_t h, size_t t) const {
        return h != t && check_mask_equal(h, t);
    }

    size_t idx(size_t x) const { return x & mask; }

    size_t turn(size_t x) const { return x >> shift; }
};

// !!NOTICE: DO NOT USE LockfreeMPMCRingQueue in IPC
// This queue may block if one of processes crashed during push / pop
// Do not use as IPC base. Use it to collect data and send by
// LockfreeSPSCRingQueue
template <typename T, size_t N>
class LockfreeMPMCRingQueue : public LockfreeRingQueueBase<T, N> {
protected:
    using Base = LockfreeRingQueueBase<T, N>;

    using Base::head;
    using Base::idx;
    using Base::tail;

    std::atomic<uint64_t> marks[Base::capacity]{};
    T slots[Base::capacity];

    uint64_t this_turn_write(const uint64_t x) const {
        return (Base::turn(x) << 1) + 1;
    }

    uint64_t this_turn_read(const uint64_t x) const {
        return (Base::turn(x) << 1) + 2;
    }

    uint64_t last_turn_read(const uint64_t x) const {
        return Base::turn(x) << 1;
    }

public:
    using Base::empty;
    using Base::full;

    bool push(const T& x) {
        auto t = tail.load(std::memory_order_acquire);
        for (;;) {
            auto& slot = slots[idx(t)];
            auto& mark = marks[idx(t)];
            if (mark.load(std::memory_order_acquire) == last_turn_read(t)) {
                if (tail.compare_exchange_strong(t, t + 1)) {
                    slot = x;
                    mark.store(this_turn_write(t), std::memory_order_release);
                    return true;
                }
            } else {
                auto const prevTail = t;
                auto h = head.load(std::memory_order_acquire);
                t = tail.load(std::memory_order_acquire);
                if (t == prevTail && Base::check_full(h, t)) {
                    return false;
                }
            }
        }
    }

    bool pop(T& x) {
        auto h = head.load(std::memory_order_acquire);
        for (;;) {
            auto& slot = slots[idx(h)];
            auto& mark = marks[idx(h)];
            if (mark.load(std::memory_order_acquire) == this_turn_write(h)) {
                if (head.compare_exchange_strong(h, h + 1)) {
                    x = slot;
                    mark.store(this_turn_read(h), std::memory_order_release);
                    return true;
                }
            } else {
                auto const prevHead = h;
                auto t = tail.load(std::memory_order_acquire);
                h = head.load(std::memory_order_acquire);
                if (h == prevHead && Base::check_empty(h, t)) {
                    return false;
                }
            }
        }
    }

    template <typename Pause = ThreadPause>
    void send(const T& x) {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "Pause should be derived by PauseBase");
        auto const t = tail.fetch_add(1);
        auto& slot = slots[idx(t)];
        auto& mark = marks[idx(t)];
        while (mark.load(std::memory_order_acquire) != last_turn_read(t))
            Pause::pause();
        slot = x;
        mark.store(this_turn_write(t), std::memory_order_release);
    }

    template <typename Pause = ThreadPause>
    T recv() {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "Pause should be derived by PauseBase");
        auto const h = head.fetch_add(1);
        auto& slot = slots[idx(h)];
        auto& mark = marks[idx(h)];
        while (mark.load(std::memory_order_acquire) != this_turn_write(h))
            Pause::pause();
        T ret = slot;
        mark.store(this_turn_read(h), std::memory_order_release);
        return ret;
    }
};

template <typename T, size_t N>
class LockfreeBatchMPMCRingQueue : public LockfreeRingQueueBase<T, N> {
protected:
    using Base = LockfreeRingQueueBase<T, N>;
    using Base::check_empty;
    using Base::check_full;
    using Base::check_mask_equal;

    using Base::head;  // read_head
    using Base::idx;
    using Base::tail;  // write_tail

    alignas(Base::CACHELINE_SIZE) std::atomic<uint64_t> write_head{0};
    alignas(Base::CACHELINE_SIZE) std::atomic<uint64_t> read_tail{0};

    T slots[Base::capacity];

    uint64_t this_turn_write(const uint64_t x) const {
        return (Base::turn(x) << 1) + 1;
    }

    uint64_t this_turn_read(const uint64_t x) const {
        return (Base::turn(x) << 1) + 2;
    }

    uint64_t last_turn_read(const uint64_t x) const {
        return Base::turn(x) << 1;
    }

public:
    using Base::empty;
    using Base::full;

    size_t push_batch(const T *x, size_t n) {
        size_t rh, wt;
        wt = tail.load(std::memory_order_acquire);
        for (;;) {
            rh = head.load(std::memory_order_acquire);
            auto wn = std::min(n, Base::capacity - (wt - rh));
            if (wn == 0)
                return 0;
            if (!tail.compare_exchange_strong(wt, wt + wn, std::memory_order_acq_rel))
                continue;
            auto first_idx = idx(wt);
            auto part_length = Base::capacity - first_idx;
            if (likely(part_length >= wn)) {
                memcpy(&slots[first_idx], x, sizeof(T) * wn);
            } else {
                if (likely(part_length))
                    memcpy(&slots[first_idx], x, sizeof(T) * (part_length));
                memcpy(&slots[0], x + part_length, sizeof(T) * (wn - part_length));
            }
            auto wh = wt;
            while (!write_head.compare_exchange_strong(wh, wt + wn, std::memory_order_acq_rel))
                wh = wt;
            return wn;
        }
    }

    bool push(const T &x) {
        return push_batch(&x, 1) == 1;
    }

    size_t pop_batch(T *x, size_t n) {
        size_t rt, wh;
        rt = read_tail.load(std::memory_order_acquire);
        for (;;) {
            wh = write_head.load(std::memory_order_acquire);
            auto rn = std::min(n, wh - rt);
            if (rn == 0)
                return 0;
            if (!read_tail.compare_exchange_strong(rt, rt + rn, std::memory_order_acq_rel))
                continue;
            auto first_idx = idx(rt);
            auto part_length = Base::capacity - first_idx;
            if (likely(part_length >= rn)) {
                memcpy(x, &slots[first_idx], sizeof(T) * rn);
            } else {
                if (likely(part_length))
                    memcpy(x, &slots[first_idx], sizeof(T) * (part_length));
                memcpy(x + part_length, &slots[0], sizeof(T) * (rn - part_length));
            }
            auto rh = rt;
            while (!head.compare_exchange_strong(rh, rt + rn, std::memory_order_acq_rel))
                rh = rt;
            return rn;
        }
    }

    bool pop(T& x) { return pop_batch(&x, 1) == 1; }

    template <typename Pause = ThreadPause>
    T recv() {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "BusyPause should be derived by PauseBase");
        T ret;
        while (!pop(ret)) Pause::pause();
        return ret;
    }

    template <typename Pause = ThreadPause>
    void send(const T& x) {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "BusyPause should be derived by PauseBase");
        while (!push(x)) Pause::pause();
    }

    template <typename Pause = ThreadPause>
    void send_batch(const T* x, size_t n) {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "BusyPause should be derived by PauseBase");
        do {
            size_t cnt;
            while ((cnt = push_batch(x, n)) == 0) Pause::pause();
            x += cnt;
            n -= cnt;
        } while (n);
    }

    template <typename Pause = ThreadPause>
    size_t recv_batch(T* x, size_t n) {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "BusyPause should be derived by PauseBase");
        size_t ret = 0;
        while ((ret = pop_batch(x, n)) == 0) Pause::pause();
        return ret;
    }

    bool empty() {
        return check_empty(head.load(std::memory_order_relaxed),
                           tail.load(std::memory_order_relaxed));
    }

    bool full() {
        return check_full(read_tail.load(std::memory_order_relaxed),
                          write_head.load(std::memory_order_relaxed));
    }

    size_t read_available() const {
        return write_head.load(std::memory_order_relaxed) -
               read_tail.load(std::memory_order_relaxed);
    }

    size_t write_available() const {
        return head.load(std::memory_order_relaxed) + Base::capacity -
               tail.load(std::memory_order_relaxed);
    }
};

template <typename T, size_t N>
class LockfreeSPSCRingQueue : public LockfreeRingQueueBase<T, N> {
protected:
    using Base = LockfreeRingQueueBase<T, N>;
    using Base::head;
    using Base::idx;
    using Base::tail;

    T slots[Base::capacity];

public:
    using Base::empty;
    using Base::full;

    bool push(const T& x) {
        auto t = tail.load(std::memory_order_acquire);
        if (unlikely(Base::check_full(head, t))) return false;
        slots[idx(t)] = x;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& x) {
        auto h = head.load(std::memory_order_acquire);
        if (unlikely(Base::check_empty(h, tail))) return false;
        x = slots[idx(h)];
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    size_t push_batch(const T* x, size_t n) {
        auto t = tail.load(std::memory_order_relaxed);
        n = std::min(
            n, Base::capacity - (t - head.load(std::memory_order_acquire)));
        if (n == 0) return 0;
        auto first_idx = idx(t);
        auto part_length = Base::capacity - first_idx;
        if (likely(part_length >= n)) {
            memcpy(&slots[first_idx], x, sizeof(T) * n);
        } else {
            if (likely(part_length))
                memcpy(&slots[first_idx], x, sizeof(T) * (part_length));
            memcpy(&slots[0], x + part_length, sizeof(T) * (n - part_length));
        }
        tail.store(t + n, std::memory_order_release);
        return n;
    }

    size_t pop_batch(T* x, size_t n) {
        auto h = head.load(std::memory_order_relaxed);
        n = std::min(n, tail.load(std::memory_order_acquire) - h);
        if (n == 0) return 0;
        auto first_idx = idx(h);
        auto part_length = Base::capacity - first_idx;
        if (likely(part_length >= n)) {
            memcpy(x, &slots[first_idx], sizeof(T) * n);
        } else {
            if (likely(part_length))
                memcpy(x, &slots[first_idx], sizeof(T) * (part_length));
            memcpy(x + part_length, &slots[0], sizeof(T) * (n - part_length));
        }
        head.store(h + n, std::memory_order_release);
        return n;
    }

    template <typename Pause = ThreadPause>
    T recv() {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "BusyPause should be derived by PauseBase");
        T ret;
        while (!pop(ret)) Pause::pause();
        return ret;
    }

    template <typename Pause = ThreadPause>
    void send(const T& x) {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "BusyPause should be derived by PauseBase");
        while (!push(x)) Pause::pause();
    }

    template <typename Pause = ThreadPause>
    void send_batch(const T* x, size_t n) {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "BusyPause should be derived by PauseBase");
        do {
            size_t cnt;
            while ((cnt = push_batch(x, n)) == 0) Pause::pause();
            x += cnt;
            n -= cnt;
        } while (n);
    }

    template <typename Pause = ThreadPause>
    size_t recv_batch(T* x, size_t n) {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "BusyPause should be derived by PauseBase");
        size_t ret = 0;
        while ((ret = pop_batch(x, n)) == 0) Pause::pause();
        return ret;
    }
};

namespace photon {
namespace common {

/**
 * @brief A lock-free stack of the park slots of idle (sleeping) consumers.
 *
 * A consumer that finds the queue empty publishes a `Slot`, which lives on its
 * own stack frame, and then sleeps on it. A producer that has just pushed an
 * item claims one slot and wakes its owner up. Claiming is exclusive *by
 * construction*: the claimer takes the whole stack with a single `exchange()`,
 * keeps the top slot and gives the rest back. Hence
 *   - a wake-up can not accumulate: one push wakes at most one consumer, and a
 *     published slot can be claimed exactly once, so the number of in-flight
 *     wake-ups is capped by the number of sleeping consumers -- structurally,
 *     without any counter to maintain;
 *   - the stack needs no version tag: unlike a Treiber pop, `exchange()` never
 *     CAS-es on a slot's `next` field, which is where the ABA hazard would be
 *     (slots are re-published by their owners, so the same address does come
 *     back).
 *
 * Why no notification can be lost:
 *   - `publish()` is a seq_cst RMW, and a producer has a seq_cst fence between
 *     its push and its `idle()` load, so at least one of the two sides sees
 *     the other (Dekker). The consumer also re-checks the queue right after
 *     publishing.
 *   - The owner of a slot never returns from `park()` while the slot is still
 *     linked into the stack. A claimer therefore never dereferences a dead
 *     slot, and its `thread_interrupt()` can never hit a thread that has
 *     already left `park()` and gone on to do something else.
 *   - A claimer holding the whole stack makes it look empty to everybody else,
 *     so a concurrent producer may skip its notification. Whoever gives slots
 *     back is responsible for re-checking the queue afterwards; see
 *     `RingChannel::notify_recvers()`.
 */
class ParkStack {
public:
    struct Slot {
        photon::thread* th = photon::CURRENT;
        Slot* next = nullptr;
        std::atomic<uint32_t> st{PARKED};
        bool interrupted = false;   // a wake-up interrupt was issued to `th`
    };

    // Is any consumer parked? This is the only load a producer pays for on its
    // fast path, and the line is read-mostly while consumers are busy.
    bool idle() const { return top.load(std::memory_order_relaxed) != nullptr; }

    // wake-ups that have been issued but not yet observed by their target
    uint64_t inflight() const {
        return _inflight.load(std::memory_order_acquire);
    }

    void publish(Slot* s) {
        auto head = top.load(std::memory_order_relaxed);
        do { s->next = head; }
        while (!top.compare_exchange_weak(head, s, std::memory_order_seq_cst,
                                                   std::memory_order_relaxed));
    }

    // Claims one parked consumer and wakes it up. Returns false if and only if
    // no slot was published at the moment of the exchange.
    bool unpark_one() {
        auto s = top.exchange(nullptr, std::memory_order_seq_cst);
        if (!s) return false;
        if (s->next) give_back(s->next);
        wake(s);
        return true;
    }

    // Sleeps until `s`, which must have been published, gets claimed. `on_idle`
    // is invoked every time the safety-net timeout expires (or an unrelated
    // thread_interrupt() arrives) without a claim; it must not sleep, because
    // until the slot is re-armed a claimer still believes we are sleeping.
    template <typename OnIdle>
    void park(Slot* s, uint64_t timeout_usec, OnIdle on_idle) {
        bool consumed = false;      // did we consume the wake-up interrupt?
        while (s->st.load(std::memory_order_seq_cst) == PARKED) {
            int r = photon::thread_usleep_defer(timeout_usec, &commit, s);
            if (r < 0 && errno == -1) consumed = true;
            if (s->st.load(std::memory_order_seq_cst) != COMMITTED)
                break;              // claimed, before or while we slept
            on_idle();
            uint32_t expect = COMMITTED;     // re-arm and sleep once more
            if (!s->st.compare_exchange_strong(expect, PARKED,
                        std::memory_order_seq_cst))
                break;
        }
        // The claimer's last touch of the slot is its CLAIMED store. Wait for
        // it, or we would let the slot -- a stack frame -- die under its feet.
        // It is normally already there (a wake-up interrupt costs the claimer a
        // syscall, and only two instructions follow it), so the first pauses
        // almost always suffice. If they do not, the claimer is not running on
        // any CPU, and spinning is then exactly the wrong thing to do: it keeps
        // a core away from the only thread that can end the wait. Hence the
        // escalation to sched_yield(), which blocks this vCPU no more than the
        // spinning already did, and cuts stalls of milliseconds down to
        // microseconds under CPU pressure.
        for (uint64_t i = 0; s->st.load(std::memory_order_acquire) != CLAIMED; ++i) {
            if (i < 64) CPUPause::pause();
            else if (i < 128) photon::thread_yield();
            else ThreadPause::pause();
        }
        _inflight.fetch_sub(1, std::memory_order_release);
        // The claimer found us COMMITTED but the safety-net timeout had already
        // woken us up: its interrupt landed on a running thread and is still
        // pending. Absorb it here, or it would surface at whatever the caller
        // of recv() does next. thread_yield() clears the pending error.
        if (s->interrupted && !consumed) photon::thread_yield();
    }

protected:
    enum : uint32_t {
        PARKED    = 0,  // published, the owner is sleeping or about to
        COMMITTED = 1,  // the owner is provably asleep: a claim must interrupt
        CLAIMING  = 2,  // claimed, the claimer is still working on the slot
        CLAIMED   = 3,  // claimed and released: the owner may return
    };
    std::atomic<Slot*> top{nullptr};
    std::atomic<uint64_t> _inflight{0};

    // Hands a sub-stack back after having taken all of it.
    void give_back(Slot* first) {
        Slot* head = nullptr;
        if (top.compare_exchange_strong(head, first, std::memory_order_seq_cst,
                                                     std::memory_order_relaxed))
            return;                 // common case: nobody published meanwhile
        auto tail = first;
        while (tail->next) tail = tail->next;
        do { tail->next = head; }
        while (!top.compare_exchange_weak(head, first, std::memory_order_seq_cst,
                                                       std::memory_order_relaxed));
    }

    // `s` is exclusively ours, as it has been unlinked by the exchange().
    void wake(Slot* s) {
        auto th = s->th;    // the slot is unreachable after the hand-off below
        _inflight.fetch_add(1, std::memory_order_relaxed);
        if (s->st.exchange(CLAIMING, std::memory_order_seq_cst) == COMMITTED) {
            // It is provably asleep on this slot. Both stores are still safe:
            // the owner may not leave park() before our CLAIMED store, and it
            // reads `interrupted` only after having seen it.
            s->interrupted = true;
            photon::thread_interrupt(th, -1);
        }   // else it is awake and wakes itself up, see commit() below
        s->st.store(CLAIMED, std::memory_order_release);
    }

    // Runs right after the owner of `s` has committed itself to sleeping, so
    // from now on a claimer is allowed to interrupt it.
    static void commit(void* arg) {
        auto s = (Slot*)arg;
        uint32_t expect = PARKED;
        if (s->st.compare_exchange_strong(expect, COMMITTED,
                    std::memory_order_seq_cst))
            return;
        // Already claimed, and the claimer saw us not committed yet, so nobody
        // else is going to wake us up.
        s->interrupted = true;
        photon::thread_interrupt(s->th, -1);
    }
};

/**
 * @brief RingChannel is a photon wrapper to make LockfreeQueue send/recv
 * efficiently wait and spin using photon style sync mechanism.
 *
 * Notification model (multi-producer, multi-consumer safe): a consumer that
 * runs out of both items and spin budget publishes a park slot and sleeps on
 * it; a producer that has just pushed an item claims one park slot and wakes
 * its owner up. See ParkStack for why this neither loses nor accumulates
 * notifications.
 *
 * Watch out that `recv` should run in photon environment (because it has to
 * sleep on a park slot to be notified that new item has sended). `send` could
 * running in photon or std::thread environment (needs to set template `Pause`
 * as `ThreadPause`).
 *
 * @tparam QueueType shoulde be one of LockfreeMPMCRingQueue,
 * LockfreeBatchMPMCRingQueue, or LockfreeSPSCRingQueue, with their own template
 * parameters.
 */
template <typename QueueType>
class RingChannel : public QueueType {
protected:
    ParkStack idlers;      // park slots of the idle consumers
    uint64_t default_yield_turn = 1024;
    uint64_t default_yield_usec = 1024;
    // Safety net only: a parked consumer wakes itself up this often to
    // re-check the queue and re-arm its slot. Correctness does not rely on
    // it, it merely bounds the damage of a hypothetically lost wake-up.
    uint64_t default_park_usec = 100UL * 1000;

    using T = decltype(std::declval<QueueType>().recv());

public:
    using QueueType::empty;
    using QueueType::full;
    using QueueType::pop;
    using QueueType::push;
    using QueueType::read_available;
    using QueueType::write_available;

    RingChannel() = default;
    explicit RingChannel(uint64_t max_yield_turn, uint64_t max_yield_usec)
        : default_yield_turn(max_yield_turn),
          default_yield_usec(max_yield_usec) {}

    template <typename Pause = ThreadPause>
    void send(const T& x) {
        while (!push(x)) Pause::pause();
        notify_recvers();
    }
    T recv(uint64_t max_yield_turn, uint64_t max_yield_usec) {
        T x;
        if (pop(x)) {
            after_recv();
            return x;
        }
        // yield once if failed, so photon::now will be updated
        photon::thread_yield();
        Timeout yield_timeout(max_yield_usec);
        uint64_t yield_turn = max_yield_turn;
        while (!pop(x)) {
            if (yield_turn > 0 && !yield_timeout.expired()) {
                yield_turn--;
                photon::thread_yield();
            } else {
                park();
                // reset yield mark and set into busy wait
                yield_turn = max_yield_turn;
                yield_timeout.timeout(max_yield_usec);
            }
        }
        after_recv();
        return x;
    }
    T recv() { return recv(default_yield_turn, default_yield_usec); }

    // Diagnostic accessor: wake-ups that have been issued to a parked consumer
    // but not yet observed by it. Unlike the semaphore counter that it
    // replaces, this can not accumulate over a producer burst: a park slot is
    // claimed exactly once, so the count is bounded by the number of parked
    // consumers, however long the burst is.
    uint64_t notification_pending() const { return idlers.inflight(); }

protected:
    void unpark_if_ready() { if (!empty()) idlers.unpark_one(); }

    // Called by a producer right after its push.
    void notify_recvers() {
        // Dekker barrier: order the push before the idle() load below, paired
        // with the seq_cst RMW on the stack top in ParkStack::publish(). Hence
        // we can not both miss a parked consumer and be missed by it.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (idlers.idle()) unpark_if_ready();
    }

    // Called by a consumer right after a successful pop: pass the baton on if
    // there is still work and somebody to do it.
    void after_recv() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (idlers.idle()) unpark_if_ready();
    }

    // Publishes a park slot and sleeps on it until a producer claims it.
    void park() {
        ParkStack::Slot slot;
        idlers.publish(&slot);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        unpark_if_ready();     // may well claim our own slot, which is fine
        idlers.park(&slot, default_park_usec, [this] { unpark_if_ready(); });
    }
};

}  // namespace common
}  // namespace photon

#undef size_t

