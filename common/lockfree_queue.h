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
#include <mutex>
#include <thread>
#include <utility>
#ifndef __aarch64__
#include <immintrin.h>
#endif

#include <photon/common/utility.h>

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

    bool check_full(size_t h, size_t t) const { return check_mask_equal(h, t + 1); }

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

    struct alignas(Base::CACHELINE_SIZE) Entry {
        std::atomic<uint64_t> mark{0};
        T x;
    };

    static_assert(sizeof(Entry) % Base::CACHELINE_SIZE == 0,
                  "Entry should aligned to cacheline");
    alignas(Base::CACHELINE_SIZE) Entry slots[Base::capacity];
    static_assert(sizeof(slots) % Base::CACHELINE_SIZE == 0,
                  "Entry should aligned to cacheline");

    uint64_t this_turn_write(const uint64_t x) const {
        return (Base::turn(x) << 1) + 1;
    }

    uint64_t this_turn_read(const uint64_t x) const {
        return (Base::turn(x) << 1) + 2;
    }

    uint64_t last_turn_read(const uint64_t x) const { return Base::turn(x) << 1; }

public:
    using Base::empty;
    using Base::full;

    bool push(const T& x) {
        auto h = head.load(std::memory_order_acquire);
        for (;;) {
            auto& slot = slots[idx(h)];
            if (slot.mark.load(std::memory_order_acquire) ==
                last_turn_read(h)) {
                if (head.compare_exchange_strong(h, h + 1)) {
                    slot.x = x;
                    slot.mark.store(this_turn_write(h),
                                    std::memory_order_release);
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

    bool pop(T& x) {
        auto t = tail.load(std::memory_order_acquire);
        for (;;) {
            auto& slot = slots[idx(t)];
            if (slot.mark.load(std::memory_order_acquire) ==
                this_turn_write(t)) {
                if (tail.compare_exchange_strong(t, t + 1)) {
                    x = slot.x;
                    slot.mark.store(this_turn_read(t),
                                    std::memory_order_release);
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

    template <typename Pause = ThreadPause>
    void send(const T&  x) {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "Pause should be derived by PauseBase");
        auto const h = head.fetch_add(1);
        auto& slot = slots[idx(h)];
        while (slot.mark.load(std::memory_order_acquire) != last_turn_read(h))
            Pause::pause();
        slot.x = x;
        slot.mark.store(this_turn_write(h), std::memory_order_release);
    }

    template <typename Pause = ThreadPause>
    T recv() {
        static_assert(std::is_base_of<PauseBase, Pause>::value,
                      "Pause should be derived by PauseBase");
        auto const t = tail.fetch_add(1);
        auto& slot = slots[idx(t)];
        while (slot.mark.load(std::memory_order_acquire) != this_turn_write(t))
            Pause::pause();
        T ret = slot.x;
        slot.mark.store(this_turn_read(t), std::memory_order_release);
        return ret;
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

    bool push(const T&  x) {
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
};
