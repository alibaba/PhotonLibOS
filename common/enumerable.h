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
#include <type_traits>
#include <utility>

// If class A can be enumerable with A::get() and A::next(),
// then objects of A can be enumerated by the `for` statement
// with the help of Enumerable<A>(a).
//
// See the following example for defination of get() and next().

template<typename T>
struct Enumerable
{
    T* obj;
    bool autoDelete;
    ~Enumerable() { if (autoDelete) delete obj; }
    Enumerable(T& obj) : Enumerable(&obj, false) { }
    Enumerable(T* obj, bool autoDelete = false) :
        obj(obj), autoDelete(autoDelete) { }

    struct iterator
    {
        T* obj;
        explicit iterator(T* obj) : obj(obj) {
          if (obj && obj->next() < 0)
            this->obj = nullptr;
        }
        using R = typename std::result_of<decltype(&T::get)(T)>::type;
        R operator*() { return obj->get(); }
        bool operator==(const iterator& rhs) const { return obj == rhs.obj; }
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }
        iterator& operator++()
        {
            if (obj->next() < 0)
                obj = nullptr;
            return *this;
        }
        iterator operator++(int)
        {
            auto rst = *this;
            ++(*this);
            return rst;
        }
    };
    iterator begin() { return iterator(obj); }
    iterator end()   { return iterator(nullptr); }
};

template<typename T>
struct Enumerable_Holder : Enumerable<T>
{
    T _obj;
    Enumerable_Holder(T&& obj) :
        Enumerable<T>(&_obj, false), _obj(std::move(obj)) { }
};

template<typename T>
inline Enumerable<T> enumerable(T& obj)
{
    return {obj};
}

template<typename T>
inline Enumerable<T> enumerable(T* obj)
{
    return {obj};
}

template<typename T>
inline Enumerable_Holder<T> enumerable(T&& obj)
{
    return {std::move(obj)};
}

inline void __example_of_enumerable__()
{
    struct exam
    {
        int next() { return -1; }           // move to next, return 0 for success, -1 for failure
        double* get() { return nullptr; }   // get current result
    };
    exam ex;
    for (auto x: enumerable(ex)) { _unused(x);}
    for (auto x: enumerable(&ex)) { _unused(x);}
    for (auto x: enumerable(exam())) { _unused(x);}
}
