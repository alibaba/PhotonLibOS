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
#include <photon/common/utility.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <vector>

namespace photon {

template <size_t MIN_ALLOCATION_SIZE = 4UL * 1024,
          size_t MAX_ALLOCATION_SIZE = 64UL * 1024 * 1024,
          size_t ALIGNMENT = 64>
class PooledStackAllocator {
    constexpr static bool is_power2(size_t n) { return (n & (n - 1)) == 0; }
    static_assert(is_power2(ALIGNMENT), "must be 2^n");
    static_assert(is_power2(MAX_ALLOCATION_SIZE), "must be 2^n");
    const static size_t N_SLOTS =
        __builtin_ffsl(MAX_ALLOCATION_SIZE / MIN_ALLOCATION_SIZE);

public:
    PooledStackAllocator() {}

protected:
    size_t in_pool_size = 0;
    static size_t trim_threshold;

    static void* __alloc(size_t alloc_size) {
        void* ptr;
        int ret = ::posix_memalign(&ptr, ALIGNMENT, alloc_size);
        if (ret != 0) {
            errno = ret;
            return nullptr;
        }
#if defined(__linux__)
        madvise(ptr, alloc_size, MADV_NOHUGEPAGE);
#endif
        return ptr;
    }

    static void __dealloc(void* ptr, size_t size) {
        madvise(ptr, size, MADV_DONTNEED);
        free(ptr);
    }

    struct Slot {
        std::vector<std::pair<void*, size_t>> pool;

        ~Slot() {
            for (auto pt : pool) {
                __dealloc(pt.first, pt.second);
            }
        }
        std::pair<void*, size_t> get() {
            if (!pool.empty()) {
                auto ret = pool.back();
                pool.pop_back();
                return ret;
            }
            return {nullptr, 0};
        }
        void put(void* ptr, size_t size) { pool.emplace_back(ptr, size); }
    };

    static inline uint32_t get_slot(uint32_t length) {
        static auto base = __builtin_clz(MIN_ALLOCATION_SIZE - 1);
        auto index = __builtin_clz(length - 1);
        return base > index ? base - index : 0;
    }

    Slot slots[N_SLOTS];

public:
    void* alloc(size_t size) {
        auto idx = get_slot(size);
        if (unlikely(idx > N_SLOTS)) {
            // larger than biggest slot
            return __alloc(size);
        }
        auto ptr = slots[idx].get();
        if (unlikely(!ptr.first)) {
            // slots[idx] empty
            return __alloc(size);
        }
        // got from pool
        in_pool_size -= ptr.second;
        return ptr.first;
    }
    int dealloc(void* ptr, size_t size) {
        auto idx = get_slot(size);
        if (unlikely(idx > N_SLOTS ||
                     (in_pool_size + size >= trim_threshold))) {
            // big block or in-pool buffers reaches to threshold
            __dealloc(ptr, size);
            return 0;
        }
        // Collect into pool
        in_pool_size += size;
        slots[idx].put(ptr, size);
        return 0;
    }
    size_t trim(size_t keep_size) {
        size_t count = 0;
        for (int i = 0; in_pool_size > keep_size; i = (i + 1) % N_SLOTS) {
            if (!slots[i].pool.empty()) {
                auto ptr = slots[i].pool.back();
                slots[i].pool.pop_back();
                in_pool_size -= ptr.second;
                count += ptr.second;
                __dealloc(ptr.first, ptr.second);
            }
        }
        return count;
    }
    size_t threshold(size_t x) {
        trim_threshold = x;
        return trim_threshold;
    }
};

template <size_t MIN_ALLOCATION_SIZE, size_t MAX_ALLOCATION_SIZE,
          size_t ALIGNMENT>
size_t PooledStackAllocator<MIN_ALLOCATION_SIZE, MAX_ALLOCATION_SIZE,
                            ALIGNMENT>::trim_threshold = 1024UL * 1024 * 1024;

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

}  // namespace photon