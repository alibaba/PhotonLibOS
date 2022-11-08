#pragma once
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/thread-pool.h>
#include <photon/thread/thread11.h>

#include <array>
#include <atomic>
#include <coroutine>
#include <ranges>
#include <utility>

namespace photon {
namespace coro {

class StackfulAlloc {
public:
    void *operator new(std::size_t size) {
        return photon::stackful_malloc(size);
    }
    void operator delete(void *ptr) { photon::stackful_free(ptr); }
};

class HeapAlloc {};

template <typename T, typename AllocType>
class CoroutinePromiseBase : public AllocType {
public:
    T get_value() { return current_value.value(); }
    static std::suspend_always initial_suspend() { return {}; }
    static std::suspend_always final_suspend() noexcept { return {}; }
    std::suspend_always yield_value(T t) {
        current_value = std::move(t);
        photon::thread_yield();
        return {};
    }
    std::suspend_always return_value(T t) {
        current_value = std::move(t);
        photon::thread_yield();
        return {};
    }
    static void unhandled_exception() { throw; }

private:
    std::optional<T> current_value;
};

template <typename AllocType>
class CoroutinePromiseBase<void, AllocType> : public AllocType {
public:
    static void get_value() {}
    static std::suspend_always initial_suspend() { return {}; }
    static std::suspend_always final_suspend() noexcept { return {}; }
    static std::suspend_always yield_void() {
        photon::thread_yield();
        return {};
    }
    static std::suspend_always return_void() {
        photon::thread_yield();
        return {};
    }
    static void unhandled_exception() { throw; }
};

using GeneratorSentinel = std::default_sentinel_t;

template <typename promise_type>
class GeneratorIterator {
public:
    using value_type = decltype(std::declval<promise_type>().get_value());
    using reference = value_type &;
    using pointer = value_type *;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::input_iterator_tag;

    GeneratorIterator &operator++() {
        handle.resume();
        return *this;
    }

    void operator++(int) { ++(*this); }

    bool operator==(const GeneratorSentinel &) const {
        if (handle) return handle.done();
        return true;
    }

    bool operator!=(const GeneratorSentinel &other) const {
        return !(*this == other);
    }

    bool operator==(const GeneratorIterator &) const { return true; }
    bool operator!=(const GeneratorIterator &) const { return false; }

    value_type operator*() const noexcept {
        if (!handle) return {};
        return handle.promise().get_value();
    }

    pointer operator->() const noexcept { return std::addressof(operator*()); }

    GeneratorIterator() = default;
    explicit GeneratorIterator(std::coroutine_handle<promise_type> g)
        : handle(g) {}
    GeneratorIterator(GeneratorIterator &&rhs) noexcept : handle(rhs.handle) {
        rhs.handle = {};
    }
    GeneratorIterator &operator=(GeneratorIterator &&rhs) {
        handle = rhs.handle;
        rhs.handle = {};
    }

private:
    std::coroutine_handle<promise_type> handle;
};

template <typename T>
class Coro {
public:
    class promise_type : public CoroutinePromiseBase<T, StackfulAlloc> {
    public:
        auto get_return_object() {
            return Coro(handle_type::from_promise(*this));
        }
    };
    using handle_type = std::coroutine_handle<promise_type>;
    handle_type handle;

    Coro(handle_type &&t) : handle(std::move(t)) {}

    Coro(const Coro &) = delete;
    Coro &operator=(const Coro &) = delete;
    Coro(Coro &&other) noexcept : handle{other.handle} { other.handle = {}; }
    Coro &operator=(Coro &&other) noexcept {
        if (this != &other) {
            if (handle) {
                handle.destroy();
            }
            handle = other.handle;
            other.handle = {};
        }
        return *this;
    }
    ~Coro() { handle.destroy(); }

    auto await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> awaiting) {
        while (!handle.done()) {
            handle.resume();
        }
        awaiting.resume();
    }

    T await_resume() { return handle.promise().get_value(); }
};

template <std::movable T>
class Generator {
public:
    class promise_type : public CoroutinePromiseBase<T, HeapAlloc> {
    public:
        auto get_return_object() {
            return Generator(handle_type::from_promise(*this));
        }
        // Disallow co_await in generator coroutines.
        void await_transform() = delete;
        static void unhandled_exception() { throw; }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit Generator(const handle_type handle) : handle{handle} {}

    Generator() = default;
    ~Generator() {
        if (handle) {
            handle.destroy();
        }
    }

    Generator(const Generator &) = delete;
    Generator &operator=(const Generator &) = delete;

    Generator(Generator &&other) noexcept : handle{other.handle} {
        other.handle = {};
    }
    Generator &operator=(Generator &&other) noexcept {
        if (this != &other) {
            if (handle) {
                handle.destroy();
            }
            handle = other.handle;
            other.handle = {};
        }
        return *this;
    }

    T operator()() {
        if (handle && !handle.done()) {
            handle.resume();
        }
        return std::move(handle.promise().get_value());
    }

    GeneratorIterator<promise_type> begin() {
        if (handle && !handle.done()) {
            handle.resume();
        }
        return GeneratorIterator<promise_type>(handle);
    }
    GeneratorSentinel end() { return {}; }

    // Generator unable to be await.
    // so no awaiter methods.
private:
    handle_type handle;
};

template <std::movable T>
class FixedGenerator {
public:
    struct promise_type : public CoroutinePromiseBase<T, StackfulAlloc> {
        auto get_return_object() {
            return FixedGenerator(
                handle_type::from_promise(*this));
        }
        // Disallow co_await in generator coroutines.
        void await_transform() = delete;
        static void unhandled_exception() { throw; }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit FixedGenerator(const handle_type handle) : handle{handle} {}

    FixedGenerator() = default;
    ~FixedGenerator() {
        if (handle) {
            handle.destroy();
        }
    }

    // Fixed means unmovable
    FixedGenerator(const FixedGenerator &) = delete;
    FixedGenerator &operator=(const FixedGenerator &) = delete;
    FixedGenerator(FixedGenerator &&other) = delete;
    FixedGenerator &operator=(FixedGenerator &&other) = delete;

    T operator()() {
        if (handle && !handle.done()) {
            handle.resume();
        }
        return std::move(handle.promise().get_value());
    }

    GeneratorIterator<promise_type> begin() {
        if (handle && !handle.done()) {
            handle.resume();
        }
        return GeneratorIterator<promise_type>(handle);
    }
    GeneratorSentinel end() { return {}; }

    // Generator unable to be await.
    // so no awaiter methods.
private:
    handle_type handle;
};

template <typename T, typename... Args>
inline void async_run(T func, Args &&... args) {
    photon::thread_yield_to(photon::thread_create11(
        [&](photon::thread *th) {
            auto task = func(std::forward<Args>(args)...);
            photon::thread_yield_to((photon::thread *)th);
            task.handle.resume();
        },
        photon::CURRENT));
}

template <typename F, typename... Args>
struct Wrapper {
    std::optional<std::invoke_result_t<F, Args...>> ret;
    Wrapper(F &&f, Args &&... args) { ret = f(std::forward<Args>(args)...); }
    auto await_ready() { return true; }
    void await_suspend(std::coroutine_handle<> awaiting) { awaiting.resume(); }
    auto await_resume() { return std::move(ret.value()); }
};

template <typename F, typename... Args>
Wrapper<F, Args...> wrapper(F &&f, Args &&... args) {
    return {std::forward<F>(f), std::forward<Args>(args)...};
}

template <typename F, typename... Args>
auto run(F &&f, Args &&... args) {
    auto task = f(std::forward<Args>(args)...);
    while (!task.handle.done()) {
        task.handle.resume();
    }
    return task.await_resume();
}

inline auto usleep(uint64_t time) {
    return wrapper(photon::thread_usleep, time);
}

inline auto sleep(uint64_t time) { return wrapper(photon::thread_sleep, time); }

inline Generator<uint64_t> timer(uint64_t interval) {
    auto next = photon::now;
    for (;;) {
        photon::thread_usleep(photon::sat_sub(next, photon::now));
        co_yield (uint64_t)photon::now;
        next = photon::sat_add(photon::now, interval);
    }
}

}  // namespace coro
}  // namespace photon