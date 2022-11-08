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
#include <cstdlib>
#include <cinttypes>
#include <new>
#include <photon/common/callback.h>
#include <photon/thread/thread.h>
#include <photon/thread/list.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"

class IdentityPoolBase: public intrusive_list_node<IdentityPoolBase> {
public:
    typedef Callback<void**> Constructor;
    typedef Callback<void*>  Destructor;
    void* get();
    void put(void* obj);

    uint64_t do_scale();
    int enable_autoscale();
    int disable_autoscale();

protected:
    uint32_t m_capacity, m_size = 0, m_refcnt = 0;
    uint32_t min_size_in_interval = 0;
    photon::spinlock m_mtx;
    photon::condition_variable m_cvar;
    Constructor m_ctor;
    Destructor  m_dtor;
    void* m_reserved;
    bool autoscale = false;

    void* m_items[0];  // THIS MUST BE THE LAST MEMBER DATA!!!

    ~IdentityPoolBase();
};

// template<typename T, uint32_t CAPACITY>
template<typename T>
class IdentityPoolBaseT: public IdentityPoolBase
{
public:
    typedef Callback<T**> Constructor;
    typedef Callback<T*>  Destructor;
    static IdentityPoolBaseT* new_identity_pool(uint32_t capacity,
        Constructor ctor = {0, &default_constructor},
        Destructor  dtor = {0, &default_destructor})
    {
        auto size = sizeof(IdentityPoolBaseT) +
                    sizeof(m_items[0]) * capacity;
        auto pool = malloc(size);
        return new (pool) IdentityPoolBaseT(capacity, ctor, dtor);
    }
    static void delete_identity_pool(IdentityPoolBaseT* p)
    {
        p->~IdentityPoolBaseT();
        free(p);
    }
    T* get() { return (T*)IdentityPoolBase::get(); }
    void put(T* obj) { IdentityPoolBase::put(obj); }

protected:
    IdentityPoolBaseT(uint32_t capacity,
        Constructor ctor = {0, &default_constructor},
        Destructor  dtor = {0, &default_destructor})
    {
        m_capacity = capacity;
        static_assert(sizeof(m_ctor) == sizeof(ctor), "...");
        static_assert(sizeof(m_dtor) == sizeof(dtor), "...");
        m_ctor = (IdentityPoolBase::Constructor&)ctor;
        m_dtor = (IdentityPoolBase::Destructor&)dtor;
    }
    void set_ctor(Constructor ctor)
    {
        m_ctor = (IdentityPoolBase::Constructor&)ctor;
    }
    void set_dtor(Destructor dtor)
    {
        m_dtor = (IdentityPoolBase::Destructor&)dtor;
    }
    static int default_constructor(void*, T** ptr)
    {
        *ptr = new T;
        return 0;
    }
    static int default_destructor(void*, T* ptr)
    {
        delete ptr;
        return 0;
    }
};

template<typename T, size_t CAPACITY>
class IdentityPool : public IdentityPoolBaseT<T>
{
public:
    using base = IdentityPoolBaseT<T>;
    using typename base::Constructor;
    using typename base::Destructor;
    using base::default_constructor;
    using base::default_destructor;
    IdentityPool(Constructor ctor = {0, &default_constructor},
                 Destructor  dtor = {0, &default_destructor}) :
        IdentityPoolBaseT<T>(CAPACITY, ctor, dtor) { }


protected:
    T* m_items[CAPACITY];  // THIS MUST BE THE FIRST MEMBER!!!
};

template<typename T>
using IdentityPool0 = IdentityPoolBaseT<T>;

template<typename T> inline
IdentityPool0<T>* new_identity_pool(uint32_t capacity)
{
    return IdentityPool0<T>::new_identity_pool(capacity);
}

template<typename T> inline
IdentityPool0<T>* new_identity_pool(uint32_t capacity,
    typename IdentityPool0<T>::Constructor ctor,
    typename IdentityPool0<T>::Destructor dtor)
{
    return IdentityPool0<T>::new_identity_pool(capacity, ctor, dtor);
}

template<typename T> inline
void delete_identity_pool(IdentityPool0<T>* p)
{
    IdentityPool0<T>::delete_identity_pool(p);
}

inline void __example_of_identity_pool__()
{
    IdentityPool<int, 32> pool;
    auto x = pool.get();
    pool.put(x);
}

#pragma GCC diagnostic pop