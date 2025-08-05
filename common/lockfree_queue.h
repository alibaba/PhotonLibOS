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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#ifndef __aarch64__
#include <immintrin.h>
#endif

#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>

struct PauseBase {};

struct CPUPause : PauseBase {
    inline static __attribute__((always_inline)) void pause() {
        photon::spin_wait();
    }
};

struct ThreadPause : PauseBase {
    inline static __attribute__((always_inline)) void pause() {
        std::this_thread::yield();
    }
};

struct PhotonPause : PauseBase {
    inline static __attribute__((always_inline)) void pause() {
        photon::thread_yield();
    }
};

struct CPUCacheLine {
#if (defined(__aarch64__) || defined(__arm64__)) && defined(__APPLE__)
    // ARM64 cache line size is 128 bytes on Apple Silicon
    constexpr static size_t SIZE = 128;
#else
    constexpr static size_t SIZE = 64;
#endif
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

    constexpr static size_t SLOTS_NUM =
        N ? 1UL << (8 * sizeof(size_t) - __builtin_clzll(N - 1)) : 0;

    const size_t capacity;
    const size_t mask;
    const size_t shift;
    const size_t lshift;

    alignas(CPUCacheLine::SIZE) std::atomic<size_t> tail{0};
    alignas(CPUCacheLine::SIZE) std::atomic<size_t> head{0};

    // For only flexible queue
    explicit LockfreeRingQueueBase(size_t c)
        : capacity(c > 1 ? 1UL << (8 * sizeof(size_t) - __builtin_clzll(c - 1))
                         : 2),
          mask(capacity - 1),
          shift(__builtin_ctzll(capacity)),
          lshift(8 * sizeof(size_t) - shift) {}

    // For only deterministic queue
    LockfreeRingQueueBase()
        : capacity(N > 1 ? 1UL << (8 * sizeof(size_t) - __builtin_clzll(N - 1))
                         : 2),
          mask(capacity - 1),
          shift(__builtin_ctzll(capacity)),
          lshift(8 * sizeof(size_t) - shift) {}
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

template <typename T, typename Q>
class FlexQueue : public Q {
protected:
    FlexQueue(size_t c) : Q(c) {}
    ~FlexQueue() {}

public:
    static FlexQueue* create(size_t c) {
        size_t space = required_space(c);
        void* ptr = nullptr;
        auto err = posix_memalign(&ptr, CPUCacheLine::SIZE, required_space(c));
        if (err) return nullptr;
        memset(ptr, 0, space);
        return new (ptr) FlexQueue(c);
    }

    static size_t required_space(size_t c) {
        size_t capacity =
            c > 1 ? 1UL << (8 * sizeof(size_t) - __builtin_clzll(c - 1)) : 2;
        return sizeof(Q) + capacity * sizeof(T);
    }

    static FlexQueue* init_on(void* ptr, size_t c) {
        if (!ptr) return nullptr;
        return new (ptr) FlexQueue(c);
    }

    static void deinit(FlexQueue* q) {
        if (!q) return;
        q->~FlexQueue();
    }

    static void destroy(FlexQueue* q) {
        deinit(q);
        free(q);
    }
};

// !!NOTICE: DO NOT USE LockfreeMPMCRingQueue in IPC
// This queue may block if one of processes crashed during push / pop
// Do not use as IPC base. Use it to collect data and send by
// LockfreeSPSCRingQueue
template <typename T, size_t N, typename MarkType = uint64_t>
class LockfreeMPMCRingQueue : public LockfreeRingQueueBase<T, N> {
protected:
    using Base = LockfreeRingQueueBase<T, N>;

    using Base::head;
    using Base::idx;
    using Base::tail;

public:
    struct alignas(CPUCacheLine::SIZE) packedslot {
        T data;
        std::atomic<MarkType> mark{0};
    };

protected:
    packedslot slots[Base::SLOTS_NUM];

    MarkType this_turn_write(const uint64_t x) const {
        return (Base::turn(x) << 1) + 1;
    }

    MarkType this_turn_read(const uint64_t x) const {
        return (Base::turn(x) << 1) + 2;
    }

    MarkType last_turn_read(const uint64_t x) const {
        return Base::turn(x) << 1;
    }

    explicit LockfreeMPMCRingQueue(size_t c) : Base(c) {}

public:
    using Base::empty;
    using Base::full;

    explicit LockfreeMPMCRingQueue() : Base() {}

    bool push(const T& x) {
        auto t = tail.load(std::memory_order_acquire);
        for (;;) {
            auto& ps = slots[idx(t)];
            auto& slot = ps.data;
            auto& mark = ps.mark;
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
            auto& ps = slots[idx(h)];
            auto& slot = ps.data;
            auto& mark = ps.mark;
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
        auto& ps = slots[idx(t)];
        auto& slot = ps.data;
        auto& mark = ps.mark;
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
        auto& ps = slots[idx(h)];
        auto& slot = ps.data;
        auto& mark = ps.mark;
        while (mark.load(std::memory_order_acquire) != this_turn_write(h))
            Pause::pause();
        T ret = slot;
        mark.store(this_turn_read(h), std::memory_order_release);
        return ret;
    }
};

template <typename T>
using FlexLockfreeMPMCRingQueue =
    FlexQueue<typename LockfreeMPMCRingQueue<T, 0>::packedslot,
              LockfreeMPMCRingQueue<T, 0>>;

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

    alignas(CPUCacheLine::SIZE) std::atomic<size_t> write_head;
    alignas(CPUCacheLine::SIZE) std::atomic<size_t> read_tail;

    T slots[Base::SLOTS_NUM];
    explicit LockfreeBatchMPMCRingQueue(size_t c) : Base(c) {}

public:
    using Base::empty;
    using Base::full;

    explicit LockfreeBatchMPMCRingQueue() : Base() {}

    size_t push_batch(const T* x, size_t n) {
        size_t rh, wt;
        wt = tail.load(std::memory_order_acquire);
        for (;;) {
            rh = head.load(std::memory_order_acquire);
            auto wn = std::min(n, Base::capacity - (wt - rh));
            if (wn == 0) return 0;
            if (!tail.compare_exchange_strong(wt, wt + wn,
                                              std::memory_order_acq_rel))
                continue;
            auto first_idx = idx(wt);
            auto part_length = Base::capacity - first_idx;
            if (likely(part_length >= wn)) {
                memcpy(&slots[first_idx], x, sizeof(T) * wn);
            } else {
                if (likely(part_length))
                    memcpy(&slots[first_idx], x, sizeof(T) * (part_length));
                memcpy(&slots[0], x + part_length,
                       sizeof(T) * (wn - part_length));
            }
            auto wh = wt;
            while (!write_head.compare_exchange_strong(
                wh, wt + wn, std::memory_order_acq_rel))
                wh = wt;
            return wn;
        }
    }

    bool push(const T& x) { return push_batch(&x, 1) == 1; }

    size_t pop_batch(T* x, size_t n) {
        size_t rt, wh;
        rt = read_tail.load(std::memory_order_acquire);
        for (;;) {
            wh = write_head.load(std::memory_order_acquire);
            auto rn = std::min(n, wh - rt);
            if (rn == 0) return 0;
            if (!read_tail.compare_exchange_strong(rt, rt + rn,
                                                   std::memory_order_acq_rel))
                continue;
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
            while (!head.compare_exchange_strong(rh, rt + rn,
                                                 std::memory_order_acq_rel))
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

template <typename T>
using FlexLockfreeBatchMPMCRingQueue =
    FlexQueue<T, LockfreeBatchMPMCRingQueue<T, 0>>;

template <typename T, size_t N>
class LockfreeSPSCRingQueue : public LockfreeRingQueueBase<T, N> {
protected:
    using Base = LockfreeRingQueueBase<T, N>;
    using Base::head;
    using Base::idx;
    using Base::tail;

    T slots[Base::SLOTS_NUM];

    explicit LockfreeSPSCRingQueue(size_t c) : Base(c) {}

public:
    using Base::empty;
    using Base::full;

    explicit LockfreeSPSCRingQueue() : Base() {}

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
        return produce_push_batch(n, [&](T* p1, size_t n1, T* p2, size_t n2) {
            memcpy(p1, x, n1 * sizeof(T)); x += n1;
            if (n2) { memcpy(p2, x, n2 * sizeof(T)); }
        });
    }

    // Producer must be callable as produce(T* offset1, size_t n, T* offset2, size_t n),
    // and produce (write) data directly to the provided buffer.
    template<typename Producer>
    size_t produce_push_batch(size_t n, Producer&& produce) {
        auto t = tail.load(std::memory_order_relaxed);
        n = std::min(
            n, Base::capacity - (t - head.load(std::memory_order_acquire)));
        if (n == 0) return 0;
        auto first_idx = idx(t);
        auto part_length = Base::capacity - first_idx;
        if (likely(part_length >= n)) {
            produce(&slots[first_idx], n, nullptr, 0);
        } else {
            produce(&slots[first_idx], part_length,
                    &slots[0], n - part_length);
        }
        tail.store(t + n, std::memory_order_release);
        return n;
    }

    template<typename Producer>
    size_t produce_push_batch_fully(size_t n, Producer&& produce) {
        auto t = tail.load(std::memory_order_relaxed);
        if (n > Base::capacity - (t - head.load(std::memory_order_acquire))) return 0;
        auto first_idx = idx(t);
        auto part_length = Base::capacity - first_idx;
        if (likely(part_length >= n)) {
            produce(&slots[first_idx], n, nullptr, 0);
        } else {
            produce(&slots[first_idx], part_length,
                    &slots[0], n - part_length);
        }
        tail.store(t + n, std::memory_order_release);
        return n;
    }

    size_t pop_batch(T* x, size_t n) {
        return consume_pop_batch(n, [&](const T* p1, size_t n1, const T* p2, size_t n2) {
            memcpy(x, p1, sizeof(T) * n1); x += n1;
            if (n2) { memcpy(x, p2, sizeof(T) * n2); }
        });
    }

    // Consumer must be callable as consume(const T* offset1, size_t length,
    //                                      const T* offset2, size_t length)
    // and consume (read) data directly from the provided buffer.
    template<typename Consumer>
    size_t consume_pop_batch(size_t n, Consumer&& consume) {
        auto h = head.load(std::memory_order_relaxed);
        n = std::min(n, tail.load(std::memory_order_acquire) - h);
        if (n == 0) return 0;
        auto first_idx = idx(h);
        auto part_length = Base::capacity - first_idx;
        if (likely(part_length >= n)) {
            consume(&slots[first_idx], n, nullptr, 0);
        } else {
            consume(&slots[first_idx], part_length,
                    &slots[0], n - part_length);
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

template <typename T>
using FlexLockfreeSPSCRingQueue = FlexQueue<T, LockfreeSPSCRingQueue<T, 0>>;

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
    photon::semaphore queue_sem;
    std::atomic<uint64_t> idler{0};
    uint64_t default_yield_turn = -1UL;
    uint64_t default_yield_usec = 1024;

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
        while (!push(x)) {
            Pause::pause();
        }
        // meke sure that idler load happends after push work done.
        if (idler.load(std::memory_order_seq_cst)) queue_sem.signal(1);
    }
    T recv(uint64_t max_yield_turn, uint64_t max_yield_usec) {
        T x;
        if (pop(x)) return x;
        // yield once if failed, so photon::now will be update
        photon::thread_yield();
        idler.fetch_add(1, std::memory_order_acq_rel);
        DEFER(idler.fetch_sub(1, std::memory_order_acq_rel));
        Timeout yield_timeout(max_yield_usec);
        uint64_t yield_turn = max_yield_turn;
        while (!pop(x)) {
            if (yield_turn > 0 && !yield_timeout.expired()) {
                yield_turn--;
                photon::thread_yield();
            } else {
                // wait for 100ms
                queue_sem.wait(1, 100UL * 1000);
                // reset yield mark and set into busy wait
                yield_turn = max_yield_turn;
                yield_timeout.timeout(max_yield_usec);
            }
        }
        return x;
    }
    T recv() { return recv(default_yield_turn, default_yield_usec); }
};

}  // namespace common
}  // namespace photon
