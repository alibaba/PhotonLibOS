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

#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <system_error>

#include <photon/photon.h>
#include <photon/thread/thread11.h>
#include <photon/thread/thread-local.h>
#include <photon/thread/future.h>

namespace photon_std {

using cv_status = ::std::cv_status;
using defer_lock_t = ::std::defer_lock_t;
using try_to_lock_t = ::std::try_to_lock_t;
using adopt_lock_t = ::std::adopt_lock_t;

void __throw_system_error(int err_num, const char* msg);

template<typename Rep, typename Period>
inline uint64_t __duration_to_microseconds(const ::std::chrono::duration<Rep, Period>& d) {
    using namespace ::std::chrono;
    // Don't use the evil duration::max() and duration::min(). Use fixed number as a boundary.
    static constexpr auto MAX_DURATION = hours(24UL * 365 * 100);
    static constexpr auto MIN_DURATION = microseconds(1);
    if (d <= d.zero()) {
        return 0;
    } else if (d < MIN_DURATION) {
        return 1;
    } else if (d > MAX_DURATION) {
        return -1;
    } else {
        return duration_cast<microseconds>(d).count();
    }
}

class thread {
public:
    using id = photon::thread*;

    thread() = default;

    ~thread() {
        if (joinable()) {
            ::std::terminate();
        }
    }

    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;

    thread(thread&& other) noexcept {
        m_th = other.m_th;
        other.m_th = nullptr;
    }

    thread& operator=(thread&& other) noexcept {
        if (joinable()) {
            ::std::terminate();
        }
        m_th = other.m_th;
        other.m_th = nullptr;
        return *this;
    }

    template<typename Function, typename... Args>
    explicit thread(Function&& f, Args&& ... args) {
        m_th = photon::thread_create11(::std::forward<Function>(f), ::std::forward<Args>(args)...);
        photon::thread_enable_join(m_th, true);
        do_migrate();
    }

    bool joinable() const {
        return m_th != nullptr;
    }

    id get_id() const noexcept {
        return m_th;
    }

    static unsigned int hardware_concurrency() noexcept {
        return photon::get_vcpu_num();
    }

    void join() {
        photon::thread_join((photon::join_handle*) m_th);
        m_th = nullptr;
    }

    void detach() {
        if (!joinable())
            __throw_system_error(EPERM, "thread::detach: thread is not able to detach");
        photon::thread_enable_join(m_th, false);
        m_th = nullptr;
    }

    void swap(photon_std::thread& other) noexcept {
        ::std::swap(this->m_th, other.m_th);
    }

private:
    void do_migrate();
    photon::thread* m_th = nullptr;
};

class mutex : public photon::mutex {
public:
    bool try_lock() {
        return photon::mutex::try_lock() == 0;
    }

    template<class Rep, class Period>
    bool try_lock_for(const ::std::chrono::duration<Rep, Period>& d) {
        uint64_t timeout = __duration_to_microseconds(d);
        return lock(timeout) == 0;
    }

    template<class Clock, class Duration>
    bool try_lock_until(const ::std::chrono::time_point<Clock, Duration>& timeout_time) {
        return try_lock_for(timeout_time - Clock::now());
    }
};

class recursive_mutex : public photon::recursive_mutex {
public:
    bool try_lock() {
        return photon::recursive_mutex::try_lock() == 0;
    }
};

using timed_mutex = mutex;

template<class Mutex>
using lock_guard = photon::locker<Mutex>;

template<class Mutex>
class unique_lock {
public:
    unique_lock() noexcept: m_mutex(nullptr), m_owns(false) {}

    unique_lock(unique_lock&& other) noexcept: m_mutex(other.m_mutex), m_owns(other.m_owns) {
        other.m_mutex = nullptr;
        other.m_owns = false;
    }

    explicit unique_lock(Mutex& m) : m_mutex(&m), m_owns(true) {
        m_mutex->lock();
    }

    unique_lock(Mutex& m, defer_lock_t t) noexcept: m_mutex(&m), m_owns(false) {}

    unique_lock(Mutex& m, try_to_lock_t t) : m_mutex(&m), m_owns(m_mutex->try_lock()) {}

    unique_lock(Mutex& m, adopt_lock_t t) : m_mutex(&m), m_owns(true) {}

    template<class Rep, class Period>
    unique_lock(Mutex& m, const ::std::chrono::duration<Rep, Period>& timeout_duration) :
            m_mutex(&m), m_owns(m_mutex->try_lock_for(timeout_duration)) {}

    template<class Clock, class Duration>
    unique_lock(Mutex& m, const ::std::chrono::time_point<Clock, Duration>& timeout_time) :
            m_mutex(&m), m_owns(m_mutex->try_lock_until(timeout_time)) {}

    ~unique_lock() {
        if (m_owns)
            m_mutex->unlock();
    }

    void lock() {
        validate_lock();
        m_mutex->lock();
        m_owns = true;
    }

    bool try_lock() {
        validate_lock();
        m_owns = m_mutex->try_lock();
        return m_owns;
    }

    template<class Rep, class Period>
    bool try_lock_for(const ::std::chrono::duration<Rep, Period>& timeout_duration) {
        validate_lock();
        m_owns = m_mutex->try_lock_for(timeout_duration);
        return m_owns;
    }

    template<class Clock, class Duration>
    bool try_lock_until(const ::std::chrono::time_point<Clock, Duration>& timeout_time) {
        validate_lock();
        m_owns = m_mutex->try_lock_until(timeout_time);
        return m_owns;
    }

    void unlock() {
        validate_unlock();
        m_mutex->unlock();
        m_owns = false;
    }

    void swap(unique_lock& other) noexcept {
        ::std::swap(m_mutex, other.m_mutex);
        ::std::swap(m_owns, other.m_owns);
    }

    Mutex* release() noexcept {
        auto* mu = m_mutex;
        m_mutex = nullptr;
        m_owns = false;
        return mu;
    }

    Mutex* mutex() const noexcept {
        return m_mutex;
    }

    bool owns_lock() const noexcept {
        return m_owns;
    }

    explicit operator bool() const noexcept {
        return m_owns;
    }

private:
    void validate_lock() {
        if (m_mutex == nullptr)
            __throw_system_error(EPERM, "unique_lock: references null mutex");
        if (m_owns)
            __throw_system_error(EDEADLK, "unique_lock: already locked");
    }

    void validate_unlock() {
        if (!m_owns)
            __throw_system_error(EPERM, "unique_lock: not locked");
    }

    Mutex* m_mutex;
    bool m_owns;
};

class condition_variable : public photon::condition_variable {
public:
    void wait(unique_lock<mutex>& lock) {
        if (lock.mutex() == nullptr)
            __throw_system_error(EPERM, "condition_variable::wait: not locked");
        photon::condition_variable::wait(lock.mutex(), -1);
    }

    template<class Predicate>
    void wait(unique_lock<mutex>& lock, Predicate stop_waiting) {
        while (!stop_waiting()) {
            wait(lock);
        }
    }

    template<class Rep, class Period>
    cv_status wait_for(unique_lock<mutex>& lock, const ::std::chrono::duration<Rep, Period>& d) {
        return wait_until(lock, ::std::chrono::steady_clock::now() + d);
    }

    template<class Rep, class Period, class Predicate>
    bool wait_for(unique_lock<mutex>& lock, const ::std::chrono::duration<Rep, Period>& d,
                  Predicate stop_waiting) {
        return wait_until(lock, ::std::chrono::steady_clock::now() + d, ::std::move(stop_waiting));
    }

    template<class Clock, class Duration>
    cv_status wait_until(unique_lock<mutex>& lock, const ::std::chrono::time_point<Clock, Duration>& t) {
        auto d = t - ::std::chrono::steady_clock::now();
        uint64_t timeout = __duration_to_microseconds(d);
        int ret = photon::condition_variable::wait(lock.mutex(), timeout);
        if (ret == 0)
            return cv_status::no_timeout;
        // We got a timeout when measured against photon's internal clock,
        // but we need to check against the caller-supplied clock to tell whether we should return a timeout.
        if (Clock::now() < t)
            return cv_status::no_timeout;
        return cv_status::timeout;
    }

    template<class Clock, class Duration, class Predicate>
    bool wait_until(unique_lock<mutex>& lock, const ::std::chrono::time_point<Clock, Duration>& t,
                    Predicate stop_waiting) {
        while (!stop_waiting()) {
            if (wait_until(lock, t) == cv_status::timeout)
                return stop_waiting();
        }
        return true;
    }
};

namespace this_thread {

inline void yield() noexcept {
    photon::thread_yield();
}

inline thread::id get_id() noexcept {
    return photon::CURRENT;
}

template<class Rep, class Period>
inline void sleep_for(const ::std::chrono::duration<Rep, Period>& d) {
    uint64_t timeout = __duration_to_microseconds(d);
    photon::thread_usleep(timeout);
}

template<class Clock, class Duration>
inline void sleep_until(const ::std::chrono::time_point<Clock, Duration>& t) {
    sleep_for(t - Clock::now());
}

/**
 * @brief Migrate current thread to a random vcpu
 */
void migrate();

}   // namespace this_thread

/**
 * @brief Initialize work pool for multi-vcpu environment
 * @note Should be called at the beginning of the main function, after photon::init().
 * @param vcpu_num The maximum vcpu number for the newly created threads, starts from 1. The main thread doesn't count.
 */
int work_pool_init(int vcpu_num = 1, int event_engine = photon::INIT_EVENT_DEFAULT & (~photon::INIT_EVENT_SIGNAL),
                   int io_engine = 0);

/**
 * @brief Destroy work pool
 */
int work_pool_fini();

template<typename T>
class future {
    using pf = photon::Future<T>;
    std::shared_ptr<pf> _fut;
public:
    future(std::shared_ptr<pf>& fut) : _fut(fut) { }
    bool valid() { return true; }
    T get() { return _fut->get(); }

    std::future_status wait(uint64_t timeout = -1) const {
        int ret = _fut->wait(timeout);
        return (ret == 0) ? std::future_status::ready  :
                            std::future_status::timeout;
    }

    template< class Rep, class Period >
    std::future_status wait_for( const std::chrono::duration<Rep,Period>& duration ) const {
        return wait(__duration_to_microseconds(duration));
    }

    template< class Clock, class Duration >
    std::future_status wait_until( const std::chrono::time_point<Clock,Duration>& timeout_time ) const {
        return wait_for(timeout_time - std::chrono::system_clock::now());
    }
};

template<typename T>
class promise {
    using pf = photon::Future<T>;
    std::shared_ptr<pf> _fut { new pf };
public:
    future<T> get_future() {
        return {_fut};
    }
    void swap( promise& other ) noexcept {
        _fut.swap(other._fut);
    }
    template<typename P>
    void set_value( P&& value ) {
        _fut->get_promise().set_value(std::forward<P>(value));
    }
};

template<>
class future<void> : public future<bool> {
public:
    future(future<bool> fut) : future<bool>(fut) { }
};

template<>
class promise<void> : public promise<bool> {
public:
    future<void> get_future() {
        return {promise<bool>::get_future()};
    }
    void set_value() {
        promise<bool>::set_value(true);
    }
};

}   // namespace photon_std

namespace std {

inline void swap(photon_std::thread& lhs, photon_std::thread& rhs) noexcept {
    lhs.swap(rhs);
}

template<class Mutex>
inline void swap(photon_std::unique_lock<Mutex>& lhs, photon_std::unique_lock<Mutex>& rhs) noexcept {
    lhs.swap(rhs);
}

}
