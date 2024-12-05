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
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
#include <cassert>
#include <photon/common/callback.h>
#include <photon/common/identity-pool.h>

// Defines allocator and deallocator for I/O memory,
// which deal with a ranged size.
// Users are free to implement their cusomized allocators.
struct IOAlloc
{
    struct RangeSize { int min, max;};  // must be non-negative

    // allocate a memory buffer of size within [size.min, size.max],
    // the more the better, returning the size actually allocated
    // or <0 indicating failrue
    typedef Callback<RangeSize, void**> Allocator;
    Allocator allocate;

    void* alloc(size_t size)
    {
        if (size > INT32_MAX)
            return nullptr;

        void* ptr = nullptr;
        int ret = allocate({(int)size, (int)size}, &ptr);
        return ret > 0 ?  ptr : nullptr;
    }

    // de-allocate a memory buffer
    typedef Callback<void*> Deallocator;
    Deallocator deallocate;

    int dealloc(void* ptr)
    {
        return deallocate(ptr);
    }

    IOAlloc() :
        allocate(nullptr, &default_allocator),
        deallocate(nullptr, &default_deallocator) { }

    IOAlloc(Allocator alloc, Deallocator dealloc) :
        allocate(alloc), deallocate(dealloc) { }

    void reset()
    {
        allocate.bind(nullptr, &default_allocator);
        deallocate.bind(nullptr, &default_deallocator);
    }

// protected:
    static int default_allocator(void*, RangeSize size, void** ptr)
    {
        assert(size.min > 0 && size.max >= size.min);
        *ptr = ::malloc((size_t)size.max);
        return size.max;
    }
    static int default_deallocator(void*, void* ptr)
    {
        ::free(ptr);
        return 0;
    }
};

struct AlignedAlloc : public IOAlloc
{
    AlignedAlloc(size_t alignment) : IOAlloc(
        Allocator{(void*)alignment, &aligned_allocator},
        Deallocator{(void*)alignment, &default_deallocator}) { }

    void reset()
    {
        auto alignment = allocate._obj;
        allocate.bind(alignment, &aligned_allocator);
        deallocate.bind(alignment, &default_deallocator);
    }

protected:
    static int aligned_allocator(void* alignment, RangeSize size, void** ptr)
    {
        assert(size.min > 0 && size.max >= size.min);
        int err = ::posix_memalign(ptr, (size_t)alignment, (size_t)size.max);
        if (err) {
            errno = err;
            return -1;
        }
        return size.max;
    }
};

template<
    size_t MAX_ALLOCATION_SIZE = 1024 * 1024,
    size_t SLOT_CAPACITY = 32,
    size_t ALIGNMENT = 4096>
class PooledAllocator
{
    constexpr static bool is_power2(size_t n) { return (n & (n-1)) == 0; }
    static_assert(is_power2(ALIGNMENT), "must be 2^n");
    static_assert(is_power2(MAX_ALLOCATION_SIZE), "must be 2^n");
    static_assert(MAX_ALLOCATION_SIZE >= ALIGNMENT, "...");
    const static size_t N_SLOTS = __builtin_ffsl(MAX_ALLOCATION_SIZE / 4096);
public:
    PooledAllocator()
    {
        auto size = ALIGNMENT;
        for (auto& slot: slots)
        {
            slot.set_alloc_size(size);
            size *= 2;
        }
    }
    IOAlloc get_io_alloc()
    {
        return IOAlloc{{this, &PooledAllocator::allocator},{this, &PooledAllocator::deallocator}};
    }

    int enable_autoscale() {
        for (auto &x : slots) {
            x.enable_autoscale();
        }
        return 0;
    }

    int disable_autoscale() {
        for (auto &x : slots) {
            x.disable_autoscale();
        }
        return 0;
    }

protected:
    const int BASE_OFF = log2_round(ALIGNMENT);
    class Slot : public IdentityPool<void, SLOT_CAPACITY>
    {
    public:
        Slot() : IdentityPool<void, SLOT_CAPACITY>(
            {this, &Slot::myalloc}, {this, &IOAlloc::default_deallocator}) { }
        uint32_t get_alloc_size()
        {
            return (uint32_t)(uint64_t)(this->m_reserved);
        }
        void set_alloc_size(uint32_t alloc_size)
        {
            this->m_reserved = (void*)(uint64_t)alloc_size;
        }
        int myalloc(void** ptr)
        {
            int ret = ::posix_memalign((void**)ptr, ALIGNMENT, get_alloc_size());
            if (ret != 0)
            {
                errno = ret;
                return -1;
            }
            return get_alloc_size();
        }
    };

    static inline int log2_round(unsigned int x, bool round_up = false) {
        assert(x > 0);
        int ret = sizeof(x)*8 - 1 - __builtin_clz(x);
        if (round_up && (1U << ret) < x)
            return ret + 1;
        return ret;
    }

    int get_slot(unsigned int x, bool round_up = false) {
        int i = log2_round(x, round_up);
        if (i < BASE_OFF)
            return 0;
        return i - BASE_OFF;
    }

    Slot slots[N_SLOTS];
    int allocator(IOAlloc::RangeSize size, void** ptr)
    {
        assert(size.max > 0);
        if (size.min > (ssize_t)MAX_ALLOCATION_SIZE)
            return -1;
        if (size.max > (ssize_t)MAX_ALLOCATION_SIZE)
            size.max = MAX_ALLOCATION_SIZE;
        *ptr = slots[get_slot(size.max, true)].get();
        return (*ptr) ? size.max : -1;
    }
    int deallocator(void* ptr)
    {
#ifdef __APPLE__
        size_t size = malloc_size(ptr);
#else
        size_t size = malloc_usable_size(ptr);
#endif
        assert(size > 0);
        slots[get_slot(size)].put(ptr);
        return 0;
    }
};

inline void ___example_of_pooled_allocator___()
{
    PooledAllocator<1024 * 1024> x;
}
