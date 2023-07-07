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
#include <cerrno>
#include <assert.h>
#include <type_traits>

#include <photon/common/utility.h>
#include <photon/common/PMF.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"

struct Delegate_Base { };

// This is a functor class that represents either a plain function of
// `R (*)(void*, Ts...)`, or a member function of class U `R (U::*)(Ts...)`.
// std::function<void(Ts...)> is not used, as it's less efficient.
template<typename R, typename...Ts>
struct Delegate : public Delegate_Base
{
    using Func = R (*)(void*, Ts...);
    using Func0= R (*)(Ts...);
    void* _obj = nullptr;
    Func _func = nullptr;

    Delegate() = default;

    template<typename U>    // U's Member Function
    using UMFunc = R (U::*)(Ts...);

    template<typename U>    // U's const Member Function
    using UCMFunc = R (U::*)(Ts...) const;

    template<typename U>    // Function with U* as the 1st argument
    using UFunc  = R (*)(U*, Ts...);

    constexpr Delegate(void* obj, Func func) : _obj(obj), _func(func) {}
    constexpr Delegate(Func func, void* obj) : _obj(obj), _func(func) {}
    constexpr Delegate(Func0 func0) : _obj(nullptr), _func((Func&)func0) {}

    template<typename U>
    constexpr Delegate(U* obj, UFunc<U> func) : _obj(obj), _func((Func&)func) {}

    template<typename U>
    Delegate(U* obj, UMFunc<U> func)    { bind(obj, func); }

    template<typename U>
    Delegate(U* obj, UCMFunc<U> func)   { bind(obj, func); }

    template<typename T, typename U, ENABLE_IF_BASE_OF(U, T)>
    Delegate(T* obj, UMFunc<U> func)    { bind<U>(obj, func); }

    template<typename T, typename U, ENABLE_IF_BASE_OF(U, T)>
    Delegate(T* obj, UCMFunc<U> func)   { bind<U>(obj, func); }

    #define ENABLE_IF_NOT_CB(U) \
        ENABLE_IF_NOT_BASE_OF(Delegate_Base, U)

    template<typename U, ENABLE_IF_NOT_CB(U)>
    /*explicit*/ Delegate(U& obj)
    {
        bind(&obj, &U::operator());
    }

    void bind(void* obj, Func func)     { _obj = obj; _func = func; }
    void bind(Func func, void* obj)     { _obj = obj; _func = func; }
    void bind(Func0 func0)          { _obj = nullptr; _func = (Func&)func0; }

    template<typename U>
    void bind(U* obj, UFunc<U> func)    { bind(obj, (Func&)func); }

    template<typename U>
    void bind(U* obj, UMFunc<U> func)   { bind(obj, (UCMFunc<U>&)func); }

    template<typename U>
    void bind(U* obj, UCMFunc<U> func)
    {
        auto pmf = ::get_member_function_address(obj, func);
        _func = (Func&)pmf.f;
        _obj = (void*)pmf.obj;
    }

    template<typename T, typename U, ENABLE_IF_BASE_OF(U, T)>
    void bind(T* obj, UMFunc<U> func)   { bind<U>(obj, func); }

    template<typename T, typename U, ENABLE_IF_BASE_OF(U, T)>
    void bind(T* obj, UCMFunc<U> func)  { bind<U>(obj, func); }

    template<typename U, ENABLE_IF_NOT_CB(U)>
    void bind(U& obj)                   { bind<U>(&obj, &U::operator()); }

    void bind(const Delegate& rhs)      { _func = rhs._func; _obj = rhs._obj; }

    template<typename U, ENABLE_IF_NOT_CB(U)>
    void bind(U&& obj) = delete;        // not allow to bind to a rvalue (temp) object

    void clear()                        { _func = nullptr; }

    operator bool() const               { return _func; }

    R operator()(const Ts&...args) const { return fire(args...); }

    R fire(const Ts&... args) const {
        return _func ? _func(_obj, args...) : R();
    }

    R fire_once(const Ts&... args) {
        DEFER(clear());
        return fire(args...);
    }

    bool operator==(const Delegate& rhs) const {
        return _obj == rhs._obj && _func == rhs._func;
    }

    bool operator!=(const Delegate& rhs) const {
        return !(*this == rhs);
    }
};

template<typename...Ts>
using Callback = Delegate<int, Ts...>;

// a Closure encapslates a Delegate together
// with a ptr to a functor (or lambda object),
// and delete it if DELETE_CLOSURE is returned.

const int64_t DELETE_CLOSURE = -1234567890;

template<typename...ARGS> // closure must return an int64
struct Closure : public Delegate<int64_t, ARGS...> {
    template<typename T>
    explicit Closure(T* pfunctor) {
        bind(pfunctor);
    }

    Closure() = default;

    using base = Delegate<int64_t, ARGS...>;
    using base::base;

    template<typename T>
    void bind(T* pfunctor) {
        this->_obj = pfunctor;
        this->_func = [](void* task, ARGS...args) {
            assert(task);
            auto t = (T*)task;
            int64_t ret = (*t)(args...);
            if (ret == DELETE_CLOSURE) delete t;
            return ret;
        };
    }

    using base::bind;
};

using Closure0 = struct Closure<>;

/*
inline int __Examples_of_Callback(void*, int, double, long)
{
    static char ptr[] = "example";
    typedef Callback<int, double, long> Callback;
    Callback cb1(ptr, &__Examples_of_Callback);
    cb1.bind(ptr, &__Examples_of_Callback);
    cb1(65, 43.21, 0);

    class Example
    {
    public:
        int example(int, double, long) { return 0; }
        virtual int example2(int, double, long) { return -1; }
    };

    // lambda MUST be assigned to a variable before binding,
    // and MUST remain valid during the life-cycle of bond Callback!
    auto lambda = [&](int, double, long) { return 0; };

    Example e;
    Callback cb2(&e, &Example::example);
    cb2.fire_once(65, 43.21, 0);    // equivalent to e.example(65, 43.21, 0);
    cb2.bind(&e, &Example::example2);
    cb2.bind(lambda);
    lambda(1,2,3);

    return 0;
}
*/

#pragma GCC diagnostic pop
