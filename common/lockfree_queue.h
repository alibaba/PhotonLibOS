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
#include <mutex>
#include <thread>
#include <utility>
#ifndef __aarch64__
#include <immintrin.h>
#endif

#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>

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
void thread_yield();
}
struct PhotonPause : PauseBase {
    inline static __attribute__((always_inline)) void pause() {
        photon::thread_yield();
    }
};

template <typename T, size_t N>
class LockfreeRingQueueBase {
public:
#if __cplusplus < 201402L
    static_assert(std::has_trivial_copy_constructor<T>::value &&
                      std::has_trivial_copy_assign<T>::value,
                  "T should be trivially copyable");
#else
    static_assert(std::is_trivially_copy_constructible<T>::value &&
                      std::is_trivially_copy_assignable<T>::value,
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

    bool push_weak(const T& x) {
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
                t = tail.load(std::memory_order_acquire);
                if (t == prevTail) {
                    return false;
                }
            }
        }
    }

    bool pop_weak(T& x) {
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
                h = head.load(std::memory_order_acquire);
                if (h == prevHead) {
                    return false;
                }
            }
        }
    }

    bool push(const T& x) {
        do {
            if (push_weak(x)) return true;
        } while (!full());
        return false;
    }

    bool pop(T& x) {
        do {
            if (pop_weak(x)) return true;
        } while (!empty());
        return false;
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

    alignas(Base::CACHELINE_SIZE) std::atomic<uint64_t> write_head;
    alignas(Base::CACHELINE_SIZE) std::atomic<uint64_t> read_tail;

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

    size_t push_batch(const T* x, size_t n) {
        size_t rh, wt;
        wt = tail.load(std::memory_order_relaxed);
        for (;;) {
            rh = head.load(std::memory_order_relaxed);
            auto rn = std::min(n, Base::capacity - (wt - rh));
            if (rn == 0) return 0;
            if (tail.compare_exchange_strong(wt, wt + rn,
                                             std::memory_order_acq_rel)) {
                auto first_idx = idx(wt);
                auto part_length = Base::capacity - first_idx;
                if (likely(part_length >= rn)) {
                    memcpy(&slots[first_idx], x, sizeof(T) * rn);
                } else {
                    if (likely(part_length))
                        memcpy(&slots[first_idx], x, sizeof(T) * (part_length));
                    memcpy(&slots[0], x + part_length,
                           sizeof(T) * (rn - part_length));
                }
                auto wh = wt;
                while (!write_head.compare_exchange_weak(
                    wh, wt + rn, std::memory_order_acq_rel)) {
                    ThreadPause::pause();
                    wh = wt;
                }
                return rn;
            }
        }
    }

    bool push(const T& x) { return push_batch(&x, 1) == 1; }

    size_t pop_batch(T* x, size_t n) {
        size_t rt, wh;
        rt = read_tail.load(std::memory_order_relaxed);
        for (;;) {
            wh = write_head.load(std::memory_order_relaxed);
            auto rn = std::min(n, wh - rt);
            if (rn == 0) return 0;
            if (read_tail.compare_exchange_strong(rt, rt + rn,
                                                  std::memory_order_acq_rel)) {
                auto first_idx = idx(rt);
                auto part_length = Base::capacity - first_idx;
                if (likely(part_length >= rn)) {
                    memcpy(x, &slots[first_idx], sizeof(T) * rn);
                } else {
                    if (likely(part_length))
                        memcpy(x, &slots[first_idx], sizeof(T) * (part_length));
                    memcpy(x + part_length, &slots[0],
                           sizeof(T) * (rn - part_length));
                }
                auto rh = rt;
                while (!head.compare_exchange_weak(rh, rt + rn,
                                                   std::memory_order_acq_rel)) {
                    ThreadPause::pause();
                    rh = rt;
                }
                return rn;
            }
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
        auto last_idx = idx(t + n - 1);
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
        auto last_idx = idx(h + n - 1);
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
 * @brief RingChannel is a photon wrapper to make LockfreeQueue send/recv
 * efficiently wait and spin using photon style sync mechanism.
 * In order.
 * In considering of performance, RingChannel will use semaphore to hang-up
 * photon thread when queue is empty, and once it got object by recv, it will
 * trying using `thread_yield` instead of semaphore, to get better performance
 * and load balancing.
 * Watch out that `recv` should run in photon environment (because it has to)
 * use photon semaphore to be notified that new item has sended. `send` could
 * running in photon or std::thread environment (needs to set template `Pause` as
 * `ThreadPause`).
 *
 * @tparam QueueType shoulde be one of LockfreeMPMCRingQueue,
 * LockfreeBatchMPMCRingQueue, or LockfreeSPSCRingQueue, with their own template
 * parameters.
 */
template <typename QueueType>
class RingChannel : public QueueType {
protected:
    photon::semaphore queue_sem;
    std::atomic<uint64_t> idler{0};
    uint64_t m_busy_yield_turn;
    uint64_t m_busy_yield_timeout;

    using T = decltype(std::declval<QueueType>().recv());

public:
    using QueueType::empty;
    using QueueType::full;
    using QueueType::pop;
    using QueueType::push;
    using QueueType::read_available;
    using QueueType::write_available;

    /**
     * @brief Construct a new Ring Channel object
     *
     * @param busy_yield_timeout setting yield timeout, default is template
     * parameter DEFAULT_BUSY_YIELD_TIMEOUT. Ring Channel will try busy yield
     * in `busy_yield_timeout` usecs.
     */
    RingChannel(uint64_t busy_yield_turn = 64,
                uint64_t busy_yield_timeout = 1024)
        : m_busy_yield_turn(busy_yield_turn),
          m_busy_yield_timeout(busy_yield_timeout) {}

    template <typename Pause = ThreadPause>
    void send(const T& x) {
        while (!push(x)) {
            if (!full()) Pause::pause();
        }
        queue_sem.signal(idler.load(std::memory_order_acquire));
    }
    T recv() {
        T x;
        Timeout yield_timeout(m_busy_yield_timeout);
        int yield_turn = m_busy_yield_turn;
        idler.fetch_add(1, std::memory_order_acq_rel);
        DEFER(idler.fetch_sub(1, std::memory_order_acq_rel));
        while (!pop(x)) {
            if (yield_turn > 0 && photon::now < yield_timeout.expire()) {
                yield_turn--;
                photon::thread_yield();
            } else {
                queue_sem.wait(1);
            }
        }
        return x;
    }
};

}  // namespace common
}  // namespace photon