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

#if defined(__linux__)
#include <linux/mman.h>
#endif
#include <errno.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/arch.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <vector>

namespace photon {

template <size_t MIN_ALLOCATION_SIZE = 4UL * 1024,
          size_t MAX_ALLOCATION_SIZE = 64UL * 1024 * 1024>
class PooledStackAllocator {
    constexpr static bool is_power2(size_t n) { return (n & (n - 1)) == 0; }
    static_assert(is_power2(MAX_ALLOCATION_SIZE), "must be 2^n");
    const static size_t N_SLOTS =
        __builtin_ffsl(MAX_ALLOCATION_SIZE / MIN_ALLOCATION_SIZE);

public:
    PooledStackAllocator() {
        for (size_t i = 0; i < N_SLOTS; i++) {
            slots[i].slotsize = MIN_ALLOCATION_SIZE << i;
        }
    }

protected:
    size_t in_pool_size = 0;
    static size_t trim_threshold;

    static void* __alloc(size_t alloc_size) {
        void* ptr;
        int ret = ::posix_memalign(&ptr, PAGE_SIZE, alloc_size);
        if (ret != 0) {
            errno = ret;
            return nullptr;
        }
#if defined(__linux__)
        madvise(ptr, alloc_size, MADV_NOHUGEPAGE);
#endif
        mprotect(ptr, PAGE_SIZE, PROT_NONE);
        return ptr;
    }

    static void __dealloc(void* ptr, size_t size) {
        mprotect(ptr, PAGE_SIZE, PROT_READ | PROT_WRITE);
        madvise(ptr, size, MADV_DONTNEED);
        free(ptr);
    }

    struct Slot {
        std::vector<void*> pool;
        uint32_t slotsize;

        ~Slot() {
            for (auto pt : pool) {
                __dealloc(pt, slotsize);
            }
        }
        void* get() {
            if (!pool.empty()) {
                auto ret = pool.back();
                pool.pop_back();
                return ret;
            }
            return nullptr;
        }
        void put(void* ptr) { pool.emplace_back(ptr); }
    };

    // get_slot(length) returns first slot that larger or equal to length
    uint32_t get_slot(uint32_t length) {
        auto index = log2_round_up(length);
        auto base = log2_truncate(MIN_ALLOCATION_SIZE);
        return (index <= base) ? 0 : (index - base);
    }

    Slot slots[N_SLOTS];

public:
    void* alloc(size_t size) {
        auto idx = get_slot(size);
        if (unlikely(idx >= N_SLOTS)) {
            // larger than biggest slot
            return __alloc(size);
        }
        auto ptr = slots[idx].get();
        // got from pool
        if (ptr) {
            in_pool_size -= slots[idx].slotsize;
            return ptr;
        }
        return __alloc(slots[idx].slotsize);
    }
    int dealloc(void* ptr, size_t size) {
        auto idx = get_slot(size);
        if (unlikely(idx >= N_SLOTS ||
                     (in_pool_size + slots[idx].slotsize >= trim_threshold))) {
            // big block or in-pool buffers reaches to threshold
            __dealloc(ptr, idx >= N_SLOTS ? size : slots[idx].slotsize);
            return 0;
        }
        // Collect into pool
        in_pool_size += slots[idx].slotsize;
        slots[idx].put(ptr);
        return 0;
    }
    size_t trim(size_t keep_size) {
        size_t count = 0;
        for (int i = 0; in_pool_size > keep_size; i = (i + 1) % N_SLOTS) {
            if (!slots[i].pool.empty()) {
                auto ptr = slots[i].pool.back();
                slots[i].pool.pop_back();
                in_pool_size -= slots[i].slotsize;
                count += slots[i].slotsize;
                __dealloc(ptr, slots[i].slotsize);
            }
        }
        return count;
    }
    size_t threshold(size_t x) {
        trim_threshold = x;
        return trim_threshold;
    }
};

template <size_t MIN_ALLOCATION_SIZE, size_t MAX_ALLOCATION_SIZE>
size_t PooledStackAllocator<MIN_ALLOCATION_SIZE,
                            MAX_ALLOCATION_SIZE>::trim_threshold =
    1024UL * 1024 * 1024;

static PooledStackAllocator<>& get_pooled_stack_allocator() {
    thread_local PooledStackAllocator<> _alloc;
    return _alloc;
}

void* pooled_stack_alloc(void*, size_t stack_size) {
    return get_pooled_stack_allocator().alloc(stack_size);
}
void pooled_stack_dealloc(void*, void* stack_ptr, size_t stack_size) {
    get_pooled_stack_allocator().dealloc(stack_ptr, stack_size);
}

size_t pooled_stack_trim_current_vcpu(size_t keep_size) {
    return get_pooled_stack_allocator().trim(keep_size);
}

size_t pooled_stack_trim_threshold(size_t x) {
    return get_pooled_stack_allocator().threshold(x);
}

size_t pooled_stack_trim_current_vcpu(size_t keep_size);
size_t pooled_stack_trim_threshold(size_t x);

void* default_photon_thread_stack_alloc(void*, size_t stack_size) {
    char* ptr = nullptr;
    int err = posix_memalign((void**)&ptr, PAGE_SIZE, stack_size);
    if (unlikely(err))
        LOG_ERROR_RETURN(err, nullptr, "Failed to allocate photon stack! ",
                         ERRNO(err));
#if defined(__linux__)
    madvise(ptr, stack_size, MADV_NOHUGEPAGE);
#endif
    mprotect(ptr, PAGE_SIZE, PROT_NONE);
    return ptr;
}

void default_photon_thread_stack_dealloc(void*, void* ptr, size_t size) {
    mprotect(ptr, PAGE_SIZE, PROT_READ | PROT_WRITE);
#if !defined(_WIN64) && !defined(__aarch64__)
    madvise(ptr, size, MADV_DONTNEED);
#endif
    free(ptr);
}

}  // namespace photon