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
#include <cinttypes>
#include <cassert>
#include <sys/types.h>
#include <cerrno>
#include <utility>
#include <tuple>
#include <type_traits>

#include <photon/common/object.h>
#include <photon/common/callback.h>
#include <photon/thread/thread11.h>

template<typename T>
struct _AsyncResult
{
    typedef T   result_type;
    void*       object;             // the object that performed the async operation
    uint32_t    operation;          // ID of the operation
    int         error_number;       // errno, available only in case of error
    T           result;             // result of the operation
    T           get_result()        { return result; }
};

template<typename T>
struct AsyncResult : public _AsyncResult<T>
{
    bool is_failure() { return this->result < 0; }
    static T failure() { return -1; }
};

template<>
struct AsyncResult<void> : public _AsyncResult<char>
{
    bool is_failure() { return false; }
    static char failure() { return 0; }
    void get_result() { }
};

template<typename Obj>
struct AsyncResult<Obj*> : public _AsyncResult<Obj*>
{
    bool is_failure() { return this->result == nullptr; }
    static Obj* failure() { return nullptr; }
};

// When an I/O operation is done, `done` will be called with an `AsyncResult<T>*`
template<typename T>
using Done = Callback<AsyncResult<T>*>;

template<typename T>
struct done_traits;

template<typename T>
struct done_traits<Done<T>>
{
    using result_type = T;
};


template<typename R, typename T, typename...ARGS>
using AsyncFunc = void (T::*)(ARGS..., Done<R>, uint64_t timeout);

template<typename F>
struct af_traits;

template<typename T, typename...ARGS>
struct af_traits<void (T::*)(ARGS...)>
{
    // using result_type = R;
    using interface_type = T;
    using args_type = std::tuple<ARGS...>;
    const static int nargs = sizeof...(ARGS);
    static_assert(nargs >= 2, "...");
    using done_type = typename std::tuple_element<nargs-2, args_type>::type;
    using result_type = typename done_traits<done_type>::result_type;
};

// async functions have 2 additional arguments: `done` for callback, and `timeout` for timeout
#define EXPAND_FUNC(T, OP, ...) virtual void OP(__VA_ARGS__, Done<T> done, uint64_t timeout = -1)
#define EXPAND_FUNC0(T, OP)     virtual void OP(Done<T> done, uint64_t timeout = -1)

// used for define async functions in interfaces
#define DEFINE_ASYNC(T, OP, ...) EXPAND_FUNC(T, OP, __VA_ARGS__) = 0
#define DEFINE_ASYNC0(T, OP)     EXPAND_FUNC0(T, OP) = 0

// used for override and implement async functions in concrete classes
#define OVERRIDE_ASYNC(T, OP, ...) EXPAND_FUNC(T, OP, __VA_ARGS__) override
#define OVERRIDE_ASYNC0(T, OP)     EXPAND_FUNC0(T, OP) override

#define UNIMPLEMENTED_(T) { callback_umimplemented(done); }

// used for define async functions with a default (un)-implementation
#define UNIMPLEMENTED_ASYNC(T, OP, ...) \
    EXPAND_FUNC(T, OP, __VA_ARGS__) UNIMPLEMENTED_(T)
#define UNIMPLEMENTED_ASYNC0(T, OP) \
    EXPAND_FUNC0(T, OP) UNIMPLEMENTED_(T)

// used for override as unimplemented async functions in concrete classes
#define OVERRIDE_UNIMPLEMENTED_ASYNC(T, OP, ...) \
    OVERRIDE_ASYNC(T, OP, __VA_ARGS__) UNIMPLEMENTED_(T)
#define OVERRIDE_UNIMPLEMENTED_ASYNC0(T, OP) \
    OVERRIDE_ASYNC0(T, OP) UNIMPLEMENTED_(T)

/*
inline int _failed_ret(ssize_t)
{
    return -1;
}

template<typename T>
inline T* _failed_ret(T*)
{
    return nullptr;
}
*/

class IAsyncBase : public Object
{
protected:
    template<typename T>
    void callback(Done<T> done, uint32_t operation, T ret, int error_number)
    {
        AsyncResult<T> r;
        r.object = this;
        r.operation = operation;
        r.error_number = error_number;
        r.result = ret;
        done(&r);
    }
    template<typename T>
    void callback_umimplemented(Done<T> done)
    {
        callback(done, UINT32_MAX, AsyncResult<T>::failure(), ENOSYS);
    }
};

// wraps an *AsyncFunc* running in another kernel thread,
// so that it is callable from a photon thread
template<typename R, typename T, typename...ARGS>
class AsyncFuncWrapper
{
public:
    using AFunc = AsyncFunc<R, T, ARGS...>;
    AsyncFuncWrapper(T* obj, AFunc afunc, uint64_t timeout)
        : _obj(obj), afunc_(afunc), _timeout(timeout) { }

    template<typename..._Args>
    R call(_Args&&...args)
    {
        AsyncReturn aret;
        (_obj->*afunc_)(std::forward<_Args>(args)..., aret.done(),
                        _timeout);
        aret.sem.wait(1);
        assert(aret.gotit);
        if (aret.result.is_failure()) errno = aret.result.error_number;
        return aret.result.get_result();
    }

protected:
    T* _obj;
    AFunc afunc_;
    uint64_t _timeout;

    struct AsyncReturn
    {
        photon::semaphore sem;
        AsyncResult<R> result;
        bool gotit = false;
        Done<R> done()
        {
            return {this, &on_done};
        }
        static int on_done(void* aret_, AsyncResult<R>* ar)
        {
            auto aret = (AsyncReturn*)aret_;
            aret->result = *ar;
            aret->gotit = true;
            aret->sem.signal(1);
            return 0;
        }
    };
};

template<typename R, typename T, typename...ARGS>
inline AsyncFuncWrapper<R, T, ARGS...>
async_func(T* obj, AsyncFunc<R, T, ARGS...> afunc, uint64_t timeout)
{
    return {obj, afunc, timeout};
}

// Wraps a generic async func running in another kernel thread,
// so that it is callable from a photon thread.
// The completion of the func MUST invoke put_result(),
// so as to pass its result, and indicate its completion as well.
template<typename R>
class AsyncFuncWrapper_Generic
{
public:
    template<typename AFunc>
    R call(AFunc afunc)
    {
        gotit = false;
        afunc();
        sem.wait(1);
        assert(gotit);
        if (result.is_failure() && result.error_number)
            errno = result.error_number;
        return result.get_result();
    }
    template<typename Rst>
    void put_result(Rst&& r, int error_number)
    {
        result.result = std::forward<Rst>(r);
        if (result.is_failure() && error_number)
            result.error_number = error_number;
        put_result();
    }
    void put_result(int error_number)
    {
        if (error_number != 0)
            result.error_number = error_number;
        put_result();
    }

protected:
    photon::semaphore sem;
    AsyncResult<R> result;
    bool gotit;

    void put_result()
    {
        gotit = true;
        sem.signal(1);
    }
};

// inline void __example_of_AsyncFuncWrapper_Generic__()
// {
//     class IAsyncStream : public IAsyncBase
//     {
//     public:
//         DEFINE_ASYNC(ssize_t, read, void *buf, size_t count);
//     };
//     IAsyncStream* as = nullptr;
//     // async_func<ssize_t, IAsyncStream, void*, size_t>(as, &IAsyncStream::read);

//     AsyncFuncWrapper_Generic<void*> af;
//     void* r = af.call([&]()
//     {   // to run malloc() async in another thread
//         auto f = [&](){ af.put_result(malloc(100), 0); };
//         std::thread(f).detach();
//     });
// }
