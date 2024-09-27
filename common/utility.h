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
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <assert.h>
#include <utility>
#include "string_view.h"
// #include <string>

#define _unused(x) ((void)(x))

template<typename...Ts> inline
void __unused__(const Ts&...) { }

// usage: auto obj = NewObj<Xxx>(a,b,c) -> init(d,e,f);
template<typename T>
class NewObj
{
    T* _obj;
public:
    template<typename...Ts>
    NewObj(Ts&&... xs)
    {
        _obj = new T(std::forward<Ts>(xs)...);
    }

    template<typename...Ts>
    T* init(Ts&&...xs) const
    {
        auto err = _obj->init(std::forward<Ts>(xs)...);
        if (!err) return _obj;
        delete _obj;
        return nullptr;
    }

    const NewObj<T>* operator->() const
    {
        return this;
    }

    NewObj<T>* operator->()
    {
        return this;
    }
};

template<typename T>
struct ptr_array_t
{
    T* pbegin;
    T* pend;
    T* begin() { return pbegin; }
    T* end()   { return pend; }
    T& front() { return pbegin[0]; }
    T& back()  { return pend[-1]; }
};

template<typename T>
ptr_array_t<T> ptr_array(T* pbegin, size_t n)
{
    return {pbegin, pbegin + n};
}

#define __INLINE__ __attribute__((always_inline))
#define __FORCE_INLINE__ __INLINE__ inline

template<typename T>
class Defer
{
public:
    Defer(T fn) __INLINE__ : m_func(fn) {}
    ~Defer() __INLINE__ { m_func(); }
    void operator=(const Defer<T>&) = delete;
    operator bool () { return true; }   // if (DEFER(...)) { ... }

private:
    T m_func;
};

template<typename T> __FORCE_INLINE__
Defer<T> make_defer(T func) { return Defer<T>(func); }

#define _CONCAT_(a, b) a##b
#define _CONCAT(a, b) _CONCAT_(a, b)

#define DEFER(func) auto _CONCAT(__defer__, __LINE__) = \
    make_defer([&]() __INLINE__ { func; })



template<typename T, size_t n>
constexpr size_t LEN(T (&x)[n]) { return n; }


template<typename T>
struct is_function_pointer
{
    static const bool value = std::is_pointer<T>::value &&
        std::is_function<typename std::remove_pointer<T>::type>::value;
};

#define ENABLE_IF(COND) typename _CONCAT(__x__, __LINE__) = typename std::enable_if<COND>::type
#define IS_SAME(T, P)  (std::is_same<typename std::remove_cv<T>::type, P>::value)
#define ENABLE_IF_SAME(T, P)        ENABLE_IF(IS_SAME(T, P))
#define ENABLE_IF_NOT_SAME(T, P)    ENABLE_IF(!IS_SAME(T, P))
#define IS_BASE_OF(A, B) (std::is_base_of<A, B>::value)
#define ENABLE_IF_BASE_OF(A, B)     ENABLE_IF(IS_BASE_OF(A, B))
#define ENABLE_IF_NOT_BASE_OF(A, B) ENABLE_IF(!IS_BASE_OF(A, B))
#define IS_POINTER(T)  (std::is_pointer<T>::value)
#define ENABLE_IF_POINTER(T)        ENABLE_IF(IS_POINTER(T))
#define ENABLE_IF_NOT_POINTER(T)    ENABLE_IF(!IS_POINTER(T))


inline constexpr bool is_power_of_2(uint64_t x) {
    return !x || __builtin_popcountl(x) == 1;
}

inline constexpr uint8_t log2_truncate(size_t x) {
    assert(x > 0);
    uint8_t exp = sizeof(x) * 8 - 1 - __builtin_clzl(x);
    assert(x & (1UL << exp));
    return exp;
}

inline constexpr uint8_t log2_round(size_t x) {
    assert(x > 0);
    uint8_t exp = log2_truncate(x);
    assert(x & (1UL << exp));
    bool carry = exp && (x & (1UL << (exp - 1)));
    return exp + carry;
}

inline constexpr uint8_t log2_round_up(size_t x) {
    assert(x > 0);
    return (x <= 1) ? x : log2_truncate(x - 1) + 1;
}

inline size_t round_up_to_exp2(size_t x) {
    if (x == 0) return 1;
    uint32_t y =  1UL << log2_truncate(x);
    assert(x&y);
    return y << (!!(x^y));
}

template<typename INT>
struct xrange_t
{
    const INT _begin, _end;
    const int64_t _step;
    struct iterator
    {
        const xrange_t* _xrange;
        INT i;
        iterator& operator++()
        {
            i += _xrange->_step;
            return *this;
        }
        INT operator*()
        {
            return i;
        }
        bool operator == (const iterator& rhs) const
        {
            return _xrange == rhs._xrange && i == rhs.i;
        }
        bool operator != (const iterator& rhs) const
        {
            return !(*this == rhs);
        }
    };
    iterator begin() const
    {
        return iterator{this, _begin};
    }
    iterator end() const
    {
        return iterator{this, _end};
    }
};

// the xrange() series function are utilities to iterate a range of
// integers from begin (inclusive) to end (exclusive), imitating the
// xrange() function of Python
// usage: for (auto i: xrange(2, 8)) { ... }

template<typename T> inline
xrange_t<T> xrange(T begin, T end, int64_t step = 1) {
    static_assert(std::is_integral<T>::value, "...");
    assert(begin < end && (end - begin) % step == 0);
    return xrange_t<T>{begin, end, step};
}

template<typename T> inline
xrange_t<T> xrange(T end) { return xrange<T>(0, end); }
/*
template<typename T, ENABLE_IF(!std::is_signed<T>::value)>
xrange_t<uint64_t> xrange(T begin, T end, int64_t step = 1)
{
    static_assert(std::is_integral<T>::value, "...");
    return xrange_t<uint64_t>{begin, end, step};
}

template<typename T, ENABLE_IF(!std::is_signed<T>::value)>
xrange_t<uint64_t> xrange(T end)
{
    return xrange<T>(0, end);
}
*/

#define FOR_LOOP(N) for (auto i = N; i; --i)

inline uint64_t align_down(uint64_t x, uint64_t alignment)
{
    return x & ~(alignment - 1);
}

inline uint64_t align_up(uint64_t x, uint64_t alignment)
{
    return align_down(x + alignment - 1, alignment);
}

template<typename T>
inline T* align_ptr(T* p, uint64_t alignment)
{
    return (T*)align_up((uint64_t)p, alignment);
}

#define ALIGNED_MEM(name, size, alignment)  \
    char __buf##name[(size) + (alignment)]; \
    char* name = align_ptr(__buf##name, alignment);

#define ALIGNED_MEM4K(buf, size) ALIGNED_MEM(buf, size, 4096)

template<typename T>
void safe_delete(T*& obj)
{
    delete obj;
    obj = nullptr;
}

class OwnedPtr_Base
{
protected:
    void* m_ptr;
    OwnedPtr_Base(void* ptr, bool ownership)
    {
        m_ptr = (void*)((uint64_t)ptr & (uint64_t)ownership);
    }
    const static uint64_t mask = 1;
    void* get()
    {
        return (void*)((uint64_t)m_ptr & ~mask);
    }
    bool owned()
    {
        return (uint64_t)m_ptr & mask;
    }
};

template<typename T>
class OwnedPtr : OwnedPtr_Base
{
public:
    OwnedPtr(T* ptr, bool ownership) :
        OwnedPtr_Base(ptr, ownership) { }
    ~OwnedPtr()
    {
        if (owned())
            delete (T*)get();
    }
    T* operator->()
    {
        return (T*)get();
    }
    operator T* ()
    {
        return (T*)get();
    }
};

// release resource by RAII
#define WITH(init_expr) if (init_expr)

// release resource explicitly
#define WITH_Release(init_expr, release_expr)  \
    if (init_expr) if (DEFER(release_expr))

/* example of WITH
	WITH_Release(auto x=getx(), release(x)) {
		printf("x=%d, y=%d\n", x, __y);
	}

	WITH(auto x = getx()) {
                printf("x=%d, y=%d\n", x, __y);
	}
*/

constexpr bool likely(bool expr) { return __builtin_expect(expr, true); }
constexpr bool unlikely(bool expr) { return __builtin_expect(expr, false); }

int version_compare(std::string_view a, std::string_view b, int& result);
int kernel_version_compare(std::string_view dst, int& result);
void print_stacktrace();

namespace photon {

// Saturating addition, no upward overflow
__attribute__((always_inline)) inline
uint64_t sat_add(uint64_t x, uint64_t y) {
	uint64_t z, c = __builtin_uaddl_overflow(x, y, (unsigned long*)&z);
	return -c | z;
}

// Saturating subtract, no downward overflow
__attribute__((always_inline)) inline
uint64_t sat_sub(uint64_t x, uint64_t y) {
    uint64_t z, c = __builtin_usubl_overflow(x, y, (unsigned long*)&z);
    return c ? 0 : z;
}

}



