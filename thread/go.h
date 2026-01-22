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

#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include <photon/common/lockfree_queue.h>

#include <atomic>
#include <cerrno>
#include <type_traits>
#include <utility>

namespace photon {

// =============================================================================
// go() - Go-style goroutine creation
// =============================================================================

// go() creates a new photon thread (like Go's goroutine) and returns a pointer
// to it. The thread runs the given function with the provided arguments.
//
// Usage:
//   go(func, arg1, arg2, ...);           // with default stack size
//   go(stack_size, func, arg1, arg2, ...); // with custom stack size
//
// Example:
//   go([]{ printf("Hello from goroutine!\n"); });
//   go([](int x){ return x * 2; }, 42);
//   go(&MyClass::method, &obj, arg1, arg2);

template<typename F, typename...ARGUMENTS>
inline thread* go(uint64_t stack_size, F&& f, ARGUMENTS&&...args) {
    return thread_create11(stack_size, std::forward<F>(f), std::forward<ARGUMENTS>(args)...);
}

template<typename F, typename...ARGUMENTS>
inline thread* go(F&& f, ARGUMENTS&&...args) {
    return thread_create11(std::forward<F>(f), std::forward<ARGUMENTS>(args)...);
}

// =============================================================================
// channel<T> - Go-style channel for inter-thread communication
// =============================================================================

// channel<T> provides typed, thread-safe communication between photon threads,
// similar to Go's channels. Channels can be buffered or unbuffered.
//
// Usage:
//   channel<int> ch;           // unbuffered channel
//   channel<int> ch(10);       // buffered channel with capacity 10
//
//   ch.send(42);               // blocking send
//   int val = ch.recv();       // blocking receive
//   ch << 42;                  // operator syntax for send
//   ch >> val;                 // operator syntax for receive
//
//   ch.close();                // close the channel
//
//   // Range-based for loop (receives until channel is closed)
//   for (auto val : ch) { ... }
//
//   // Non-blocking operations
//   if (ch.try_send(42)) { ... }
//   int val; if (ch.try_recv(val)) { ... }
//
//   // Timed operations
//   if (ch.send(42, 1000000_us)) { ... }  // timeout in microseconds

template<typename T>
class channel {
public:
    // Create an unbuffered channel (capacity = 0)
    channel() : channel(0) {}

    // Create a buffered channel with the given capacity
    explicit channel(size_t capacity)
        : m_capacity(capacity), m_queue(nullptr), m_closed(false),
          m_senders_waiting(0), m_receivers_waiting(0),
          m_handoff_ptr(nullptr), m_handoff_ready(false) {
        if (capacity > 0) {
            m_queue = FlexLockfreeMPMCRingQueue<T*>::create(capacity);
        }
    }

    ~channel() {
        close();
        if (m_queue) {
            T* ptr = nullptr;
            while (m_queue->pop(ptr)) {
                delete ptr;
            }
            FlexLockfreeMPMCRingQueue<T*>::destroy(m_queue);
        }
    }

    // Non-copyable, moveable
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    channel(channel&& other) noexcept
        : m_capacity(other.m_capacity), m_queue(other.m_queue),
          m_closed(other.m_closed.load()), m_senders_waiting(0),
          m_receivers_waiting(0), m_handoff_ptr(nullptr), m_handoff_ready(false) {
        other.m_queue = nullptr;
        other.m_capacity = 0;
    }

    channel& operator=(channel&& other) noexcept {
        if (this != &other) {
            close();
            if (m_queue) {
                T* ptr = nullptr;
                while (m_queue->pop(ptr)) delete ptr;
                FlexLockfreeMPMCRingQueue<T*>::destroy(m_queue);
            }
            m_capacity = other.m_capacity;
            m_queue = other.m_queue;
            m_closed = other.m_closed.load();
            other.m_queue = nullptr;
            other.m_capacity = 0;
        }
        return *this;
    }

    // Close the channel
    void close() {
        if (m_closed.exchange(true, std::memory_order_acq_rel)) {
            return;  // Already closed
        }

        if (m_capacity == 0) {
            // Unbuffered: use mutex + cv
            SCOPED_LOCK(m_unbuf_mutex);
            m_unbuf_send_cv.notify_all();
            m_unbuf_recv_cv.notify_all();
        } else {
            // Buffered: wake up all waiting threads via semaphore
            int senders = m_senders_waiting.load(std::memory_order_acquire);
            int receivers = m_receivers_waiting.load(std::memory_order_acquire);
            if (senders > 0) m_send_sem.signal(senders);
            if (receivers > 0) m_recv_sem.signal(receivers);
        }
    }

    bool is_closed() const { return m_closed.load(std::memory_order_acquire); }
    bool empty() const { return m_queue ? m_queue->empty() : true; }
    size_t size() const { return m_queue ? m_queue->read_available() : 0; }
    size_t capacity() const { return m_capacity; }

    // =========================================================================
    // Send operations
    // =========================================================================

    bool send(const T& value, Timeout timeout = {}) {
        return do_send(new T(value), timeout);
    }

    bool send(T&& value, Timeout timeout = {}) {
        return do_send(new T(std::move(value)), timeout);
    }

    // =========================================================================
    // Receive operations
    // =========================================================================

    std::pair<T, bool> recv(Timeout timeout = {}) {
        T value{};
        bool ok = do_recv(value, timeout);
        return {std::move(value), ok};
    }

    bool recv(T& value, Timeout timeout = {}) {
        return do_recv(value, timeout);
    }

    // =========================================================================
    // Non-blocking operations (lock-free for buffered channels)
    // =========================================================================

    bool try_send(const T& value) { return do_try_send(new T(value)); }
    bool try_send(T&& value) { return do_try_send(new T(std::move(value))); }
    bool try_recv(T& value) { return do_try_recv(value); }

    // =========================================================================
    // Operator syntax
    // =========================================================================

    channel& operator<<(const T& value) { send(value); return *this; }
    channel& operator<<(T&& value) { send(std::move(value)); return *this; }
    channel& operator>>(T& value) { recv(value); return *this; }

    // =========================================================================
    // Iterator support for range-based for loops
    // =========================================================================

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        iterator() : m_channel(nullptr), m_valid(false) {}
        explicit iterator(channel* ch) : m_channel(ch), m_valid(true) { advance(); }

        T& operator*() { return m_value; }
        T* operator->() { return &m_value; }
        iterator& operator++() { advance(); return *this; }
        iterator operator++(int) { iterator tmp = *this; advance(); return tmp; }
        bool operator==(const iterator& other) const {
            return (!m_valid && !other.m_valid) ||
                   (m_channel == other.m_channel && m_valid == other.m_valid);
        }
        bool operator!=(const iterator& other) const { return !(*this == other); }

    private:
        void advance() {
            if (m_channel && m_valid) m_valid = m_channel->recv(m_value);
        }
        channel* m_channel;
        T m_value{};
        bool m_valid;
    };

    iterator begin() { return iterator(this); }
    iterator end() { return iterator(); }

private:
    bool do_send(T* ptr, Timeout timeout) {
        if (m_capacity == 0) {
            return unbuffered_send(ptr, timeout);
        } else {
            return buffered_send(ptr, timeout);
        }
    }

    // =========================================================================
    // Buffered channel operations (lock-free using semaphore)
    // =========================================================================

    bool buffered_send(T* ptr, Timeout timeout) {
        while (true) {
            if (m_closed.load(std::memory_order_acquire)) {
                delete ptr;
                errno = ESHUTDOWN;
                return false;
            }

            // Try to push (lock-free)
            if (m_queue->read_available() < m_capacity && m_queue->push(ptr)) {
                // Notify waiting receiver
                if (m_receivers_waiting.load(std::memory_order_acquire) > 0) {
                    m_recv_sem.signal(1);
                }
                return true;
            }

            // Buffer full, wait
            if (timeout.expired()) {
                delete ptr;
                errno = ETIMEDOUT;
                return false;
            }

            m_senders_waiting.fetch_add(1, std::memory_order_acq_rel);
            int ret = m_send_sem.wait(1, timeout.timeout_us());
            m_senders_waiting.fetch_sub(1, std::memory_order_acq_rel);

            if (ret < 0 && errno == ETIMEDOUT) {
                delete ptr;
                return false;
            }
        }
    }

    bool buffered_recv(T& value, Timeout timeout) {
        while (true) {
            // Try to pop (lock-free)
            T* ptr = nullptr;
            if (m_queue->pop(ptr)) {
                value = std::move(*ptr);
                delete ptr;
                // Notify waiting sender
                if (m_senders_waiting.load(std::memory_order_acquire) > 0) {
                    m_send_sem.signal(1);
                }
                return true;
            }

            if (m_closed.load(std::memory_order_acquire)) {
                return false;  // Closed and empty
            }

            if (timeout.expired()) {
                errno = ETIMEDOUT;
                return false;
            }

            m_receivers_waiting.fetch_add(1, std::memory_order_acq_rel);
            int ret = m_recv_sem.wait(1, timeout.timeout_us());
            m_receivers_waiting.fetch_sub(1, std::memory_order_acq_rel);

            if (ret < 0 && errno == ETIMEDOUT) {
                return false;
            }
        }
    }

    bool buffered_try_send(T* ptr) {
        if (m_closed.load(std::memory_order_acquire)) {
            delete ptr;
            return false;
        }

        // Lock-free push
        if (m_queue->read_available() < m_capacity && m_queue->push(ptr)) {
            if (m_receivers_waiting.load(std::memory_order_acquire) > 0) {
                m_recv_sem.signal(1);
            }
            return true;
        }
        delete ptr;
        return false;
    }

    bool buffered_try_recv(T& value) {
        T* ptr = nullptr;
        if (m_queue->pop(ptr)) {
            value = std::move(*ptr);
            delete ptr;
            if (m_senders_waiting.load(std::memory_order_acquire) > 0) {
                m_send_sem.signal(1);
            }
            return true;
        }
        return false;
    }

    // =========================================================================
    // Unbuffered channel operations (uses mutex for handoff coordination)
    // =========================================================================

    bool unbuffered_send(T* ptr, Timeout timeout) {
        SCOPED_LOCK(m_unbuf_mutex);

        m_senders_waiting++;
        DEFER(m_senders_waiting--);

        // Wait for a receiver
        while (!m_closed && m_receivers_waiting == 0 && !m_handoff_ready) {
            if (timeout.expired()) {
                delete ptr;
                errno = ETIMEDOUT;
                return false;
            }
            if (m_unbuf_send_cv.wait(m_unbuf_mutex, timeout) < 0 && errno == ETIMEDOUT) {
                delete ptr;
                return false;
            }
        }

        if (m_closed) {
            delete ptr;
            errno = ESHUTDOWN;
            return false;
        }

        // Place value in handoff slot
        m_handoff_ptr = ptr;
        m_handoff_ready = true;
        m_unbuf_recv_cv.notify_one();

        // Wait for receiver to take it
        while (m_handoff_ready && !m_closed) {
            if (timeout.expired()) {
                if (m_handoff_ready) {
                    delete m_handoff_ptr;
                    m_handoff_ptr = nullptr;
                    m_handoff_ready = false;
                }
                errno = ETIMEDOUT;
                return false;
            }
            m_unbuf_send_cv.wait(m_unbuf_mutex, timeout);
        }

        return !m_closed || !m_handoff_ready;
    }

    bool unbuffered_recv(T& value, Timeout timeout) {
        SCOPED_LOCK(m_unbuf_mutex);

        m_receivers_waiting++;
        DEFER(m_receivers_waiting--);

        m_unbuf_send_cv.notify_one();

        // Wait for handoff
        while (!m_handoff_ready && !m_closed) {
            if (timeout.expired()) {
                errno = ETIMEDOUT;
                return false;
            }
            if (m_unbuf_recv_cv.wait(m_unbuf_mutex, timeout) < 0 && errno == ETIMEDOUT) {
                return false;
            }
        }

        if (m_handoff_ready) {
            value = std::move(*m_handoff_ptr);
            delete m_handoff_ptr;
            m_handoff_ptr = nullptr;
            m_handoff_ready = false;
            m_unbuf_send_cv.notify_one();
            return true;
        }

        return false;
    }

    bool unbuffered_try_send(T* ptr) {
        SCOPED_LOCK(m_unbuf_mutex);

        if (m_closed) {
            delete ptr;
            return false;
        }

        if (m_receivers_waiting > 0 && !m_handoff_ready) {
            m_handoff_ptr = ptr;
            m_handoff_ready = true;
            m_unbuf_recv_cv.notify_one();
            return true;
        }
        delete ptr;
        return false;
    }

    bool unbuffered_try_recv(T& value) {
        SCOPED_LOCK(m_unbuf_mutex);

        if (m_handoff_ready) {
            value = std::move(*m_handoff_ptr);
            delete m_handoff_ptr;
            m_handoff_ptr = nullptr;
            m_handoff_ready = false;
            m_unbuf_send_cv.notify_one();
            return true;
        }
        return false;
    }

    // =========================================================================
    // Dispatch to buffered/unbuffered implementations
    // =========================================================================

    bool do_recv(T& value, Timeout timeout) {
        if (m_capacity == 0) {
            return unbuffered_recv(value, timeout);
        } else {
            return buffered_recv(value, timeout);
        }
    }

    bool do_try_send(T* ptr) {
        if (m_capacity == 0) {
            return unbuffered_try_send(ptr);
        } else {
            return buffered_try_send(ptr);
        }
    }

    bool do_try_recv(T& value) {
        if (m_capacity == 0) {
            return unbuffered_try_recv(value);
        } else {
            return buffered_try_recv(value);
        }
    }

private:
    size_t m_capacity;
    FlexLockfreeMPMCRingQueue<T*>* m_queue;
    std::atomic<bool> m_closed;
    std::atomic<int> m_senders_waiting;
    std::atomic<int> m_receivers_waiting;

    // For buffered channels: lock-free with semaphore notification
    semaphore m_send_sem;
    semaphore m_recv_sem;

    // For unbuffered channels: mutex-based handoff
    T* m_handoff_ptr;
    bool m_handoff_ready;
    mutex m_unbuf_mutex;
    condition_variable m_unbuf_send_cv;
    condition_variable m_unbuf_recv_cv;
};

// =============================================================================
// make_channel<T>() - Helper function to create channels
// =============================================================================

template<typename T>
inline channel<T> make_channel(size_t capacity = 0) {
    return channel<T>(capacity);
}

// =============================================================================
// select - Go-style select statement
// =============================================================================

// select() waits on multiple channel operations and executes the one that
// becomes ready first. If multiple are ready, one is chosen randomly.
// If none are ready and no default case, it blocks until one becomes ready.
//
// Usage:
//   // Blocking select (waits until one case is ready)
//   int result = select(
//       recv_case(ch1, [](int val, bool ok) { /* handle recv */ }),
//       send_case(ch2, 42, []() { /* handle send */ })
//   );
//
//   // Non-blocking select with default
//   int result = select_nonblock(
//       recv_case(ch1, [](int val, bool ok) { /* handle recv */ }),
//       send_case(ch2, 42, []() { /* handle send */ })
//   );
//   // Returns -1 if no case was ready (default case)

// Case types for select
struct select_case_base {
    virtual bool ready() = 0;
    virtual void execute() = 0;
    virtual ~select_case_base() = default;
};

template<typename T, typename F>
struct recv_case : select_case_base {
    channel<T>& ch;
    F func;
    T value{};

    recv_case(channel<T>& c, F&& f) : ch(c), func(std::forward<F>(f)) {}
    recv_case(channel<T>& c, const F& f) : ch(c), func(f) {}

    bool ready() override {
        return ch.try_recv(value);
    }

    void execute() override {
        func(value, true);
    }
};

template<typename T, typename F>
struct send_case : select_case_base {
    channel<T>& ch;
    T value;
    F func;

    send_case(channel<T>& c, const T& v, F&& f)
        : ch(c), value(v), func(std::forward<F>(f)) {}
    send_case(channel<T>& c, const T& v, const F& f)
        : ch(c), value(v), func(f) {}

    bool ready() override {
        return ch.try_send(value);
    }

    void execute() override {
        func();
    }
};

// Helper to create a receive case for select
template<typename T, typename F>
inline recv_case<T, typename std::decay<F>::type> recv_case_of(channel<T>& ch, F&& func) {
    return recv_case<T, typename std::decay<F>::type>(ch, std::forward<F>(func));
}

// Helper to create a send case for select
template<typename T, typename F>
inline send_case<T, typename std::decay<F>::type> send_case_of(channel<T>& ch, const T& value, F&& func) {
    return send_case<T, typename std::decay<F>::type>(ch, value, std::forward<F>(func));
}

namespace detail {

// Try all cases once, return index of ready case or -1 if none ready
template<typename... Cases>
inline int try_select_once(std::tuple<Cases...>& cases, size_t start_idx) {
    constexpr size_t N = sizeof...(Cases);
    for (size_t i = 0; i < N; i++) {
        size_t idx = (start_idx + i) % N;
        bool ready = false;
        // Use compile-time index sequence to access tuple elements
        select_try_at(cases, idx, ready, std::make_index_sequence<N>{});
        if (ready) return static_cast<int>(idx);
    }
    return -1;
}

template<typename Tuple, size_t... Is>
inline void select_try_at(Tuple& t, size_t idx, bool& ready, std::index_sequence<Is...>) {
    // Fold expression alternative for C++14
    using expander = int[];
    (void)expander{0, (idx == Is ? (ready = std::get<Is>(t).ready(), 0) : 0)...};
}

template<typename Tuple, size_t... Is>
inline void select_execute_at(Tuple& t, size_t idx, std::index_sequence<Is...>) {
    using expander = int[];
    (void)expander{0, (idx == Is ? (std::get<Is>(t).execute(), 0) : 0)...};
}

// Simple random starting index using photon::now
inline size_t random_start(size_t n) {
    return static_cast<size_t>(now) % n;
}

} // namespace detail

// Non-blocking select: try each case once, return -1 if none ready
template<typename... Cases>
inline int select_nonblock(Cases&&... cases) {
    auto case_tuple = std::make_tuple(std::forward<Cases>(cases)...);
    constexpr size_t N = sizeof...(Cases);

    size_t start = detail::random_start(N);

    for (size_t i = 0; i < N; i++) {
        size_t idx = (start + i) % N;
        bool ready = false;
        detail::select_try_at(case_tuple, idx, ready, std::make_index_sequence<N>{});
        if (ready) {
            detail::select_execute_at(case_tuple, idx, std::make_index_sequence<N>{});
            return static_cast<int>(idx);
        }
    }
    return -1; // No case ready, like default case in Go
}

// Blocking select: wait until one case is ready
template<typename... Cases>
inline int select(Cases&&... cases) {
    auto case_tuple = std::make_tuple(std::forward<Cases>(cases)...);
    constexpr size_t N = sizeof...(Cases);

    while (true) {
        size_t start = detail::random_start(N);

        for (size_t i = 0; i < N; i++) {
            size_t idx = (start + i) % N;
            bool ready = false;
            detail::select_try_at(case_tuple, idx, ready, std::make_index_sequence<N>{});
            if (ready) {
                detail::select_execute_at(case_tuple, idx, std::make_index_sequence<N>{});
                return static_cast<int>(idx);
            }
        }

        // None ready, yield and try again
        thread_yield();
    }
}

// Blocking select with timeout
template<typename... Cases>
inline int select(Timeout timeout, Cases&&... cases) {
    auto case_tuple = std::make_tuple(std::forward<Cases>(cases)...);
    constexpr size_t N = sizeof...(Cases);

    while (!timeout.expired()) {
        size_t start = detail::random_start(N);

        for (size_t i = 0; i < N; i++) {
            size_t idx = (start + i) % N;
            bool ready = false;
            detail::select_try_at(case_tuple, idx, ready, std::make_index_sequence<N>{});
            if (ready) {
                detail::select_execute_at(case_tuple, idx, std::make_index_sequence<N>{});
                return static_cast<int>(idx);
            }
        }

        // None ready, yield and try again
        thread_yield();
    }
    return -1; // Timeout
}

} // namespace photon