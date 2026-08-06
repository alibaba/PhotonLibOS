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

#include <photon/common/callback.h>
#include <photon/common/delegates.h>
#include <stddef.h>
#include <stdint.h>

namespace photon {

// Statistics common to any photon thread stack allocator that pools memory.
// Concrete pools derive from it to add their own counters, so generic tooling
// (and the StackAllocator facade below) can consume the common part.
struct StackPoolStats {
    size_t   pooled_bytes = 0;   // idle memory kept for reuse
    size_t   live_bytes = 0;     // handed out and in use
    uint64_t hits = 0;           // served from the pool
    uint64_t misses = 0;         // had to go to the slow path
};

// Stack allocator facade, built on photon's Delegates.
//
// An allocator is any object exposing the methods below; binding it produces a
// facade that forwards to them:
//
//     void*          alloc(size_t stack_size);
//     void           dealloc(void* stack_ptr, size_t stack_size);
//     size_t         trim(size_t keep_bytes);     // optional
//     StackPoolStats stats();                     // optional
//
// Binding the object binds every method at once, which is the point: the
// allocator and deallocator can no longer be set independently (setting only
// one of the two, and thus freeing stacks with a different allocator than the
// one that created them, used to be possible and is catastrophic). Methods the
// object does not provide are simply left unbound and return a zeroed value.
DEFINE_DELEGATE_FUNCTION(DStackAlloc, alloc);
DEFINE_DELEGATE_FUNCTION(DStackDealloc, dealloc);
DEFINE_DELEGATE_FUNCTION(DStackTrim, trim);
DEFINE_DELEGATE_FUNCTION(DStackStats, stats);

using StackAllocator = Delegates<
    DFeature::Feature<DStackAlloc,   void*(size_t)>,
    DFeature::Feature<DStackDealloc, void(void*, size_t)>,
    DFeature::Feature<DStackTrim,    size_t(size_t)>,
    DFeature::Feature<DStackStats,   StackPoolStats()>>;

// Set photon allocator/deallocator for photon thread stack
// this is a hook for thread allocation, both alloc and dealloc
// helps user to do more works like mark GC while allocating
void* default_photon_thread_stack_alloc(void*, size_t stack_size);
void default_photon_thread_stack_dealloc(void*, void* stack_ptr,
                                            size_t stack_size);

// Threadlocal Pooled stack allocator
// better performance, and keep thread safe
void* pooled_stack_alloc(void*, size_t stack_size);
void pooled_stack_dealloc(void*, void* stack_ptr, size_t stack_size);

// Free memory in pooled stack allocator till in-pool memory size less than
// `keep_size` for current vcpu
size_t pooled_stack_trim_current_vcpu(size_t keep_size);
// Pooled stack allocator set keep-in-pool size
size_t pooled_stack_trim_threshold(size_t threshold);

void set_photon_thread_stack_allocator(
    Delegate<void*, size_t> photon_thread_alloc = {
        &default_photon_thread_stack_alloc, nullptr},
    Delegate<void, void*, size_t> photon_thread_dealloc = {
        &default_photon_thread_stack_dealloc, nullptr});

// Set the stack allocator from a facade, binding alloc and dealloc together.
// Returns -1 (EINVAL) if `allocator` has no alloc or no dealloc bound.
int set_photon_thread_stack_allocator(StackAllocator allocator);

// Convenience: bind every supported method of `obj` and install it.
template <typename T, typename = typename std::enable_if<
                          !std::is_base_of<DelegatesBase, T>::value>::type>
inline int set_photon_thread_stack_allocator(T& obj) {
    return set_photon_thread_stack_allocator(StackAllocator(obj));
}

// The stack allocator currently in use, for trim()/stats() on whichever
// allocator is installed. Unbound methods return a zeroed value.
StackAllocator& get_photon_thread_stack_allocator();

inline void use_pooled_stack_allocator() {
    set_photon_thread_stack_allocator({&pooled_stack_alloc, nullptr},
                                      {&pooled_stack_dealloc, nullptr});
}

// Global pooled stack allocator
// Unlike `pooled_stack_allocator`, whose pool is thread-local (per-vcpu), this
// allocator keeps a single process-wide pool with per-vcpu magazine caches on
// top of it. It behaves well when photon threads migrate across vcpus (a stack
// allocated on one vcpu and freed on another does not accumulate on either),
// and it bounds the process-wide idle cache (not live allocations).
//
// Backing store is a per-stack mmap (with MAP_NORESERVE). Allocation never
// fails preemptively: a stack allocation failure is catastrophic for a
// coroutine, so if mmap is refused by the OS the allocator first returns its
// entire idle cache (cold + pending) to the OS and retries, giving up only
// when the OS still cannot satisfy the request. The idle cache is bounded by
// the max_*_bytes knobs below, which cap waste without ever failing a live
// allocation. The allocator must be selected before any photon thread is
// created; switching at runtime is not supported.
//
// fork() combined with multiple vcpus is not supported, which is a photon-wide
// limitation rather than one of this allocator: the child inherits the memory
// of every vcpu but only the forking thread. Blocks cached by the vanished
// threads are leaked in the child; the allocator only guarantees that no
// internal lock is held across fork(), so the child never deadlocks on one.
struct GlobalStackPoolOptions {
    // Upper bound of the resident idle cache (blocks kept with their pages
    // resident for zero-syscall reuse). Overflow spills to the pending chain.
    size_t   max_pooled_bytes     = 1ULL << 30;
    // Per-vcpu magazine budget per size class; drives the magazine capacity.
    size_t   per_vcpu_cache_bytes = 64ULL << 20;
    // Back-pressure threshold: freed blocks awaiting reclaim beyond this are
    // madvise'd inline on the free side (the only syscall on the free path).
    size_t   max_pending_bytes    = 256ULL << 20;
    // Upper bound of the cold cache (blocks madvise'd but still mapped, kept
    // for zero-mmap reuse). Overflow is munmap'd back to the OS.
    size_t   max_cold_bytes       = 4ULL << 30;
    // PROT_NONE guard pages at the low end of each stack. The first one is the
    // page photon relies on; extra ones sit below the returned pointer.
    uint32_t guard_pages          = 1;
    // Bytes zeroed at the top of the stack on reuse (defense against info leak
    // between photon threads). 0 disables wiping.
    uint32_t wipe_bytes           = 0;
    // Keep pooled blocks PROT_NONE while idle so use-after-free faults; costs
    // two extra syscalls per reuse. For debugging only.
    bool     paranoid             = false;
    // MADV_NOHUGEPAGE on the stack region, matching the other allocators.
    bool     no_huge_page         = true;
};

struct GlobalStackPoolStats : StackPoolStats {
    size_t   mapped_bytes = 0;   // total bytes currently mmap'd (diagnostic)
    size_t   pending_bytes = 0;  // freed, awaiting reclaim, still resident
    size_t   cold_bytes = 0;     // madvise'd but still mapped
    uint64_t os_maps = 0;        // mmap count
    uint64_t os_unmaps = 0;      // munmap count
    uint64_t corruptions = 0;    // double-free / metadata corruption detections
};

void* global_pooled_stack_alloc(void*, size_t stack_size);
void global_pooled_stack_dealloc(void*, void* stack_ptr, size_t stack_size);

// Select the global pooled stack allocator and capture its options. Must be
// called before photon threads are created, or while the pool is otherwise
// quiescent (no concurrent allocation): the options are read locklessly on the
// hot path, so reconfiguring under concurrent load races. A later quiescent
// call updates the options in place and keeps the existing pools.
int use_global_pooled_stack_allocator(const GlobalStackPoolOptions& options = {});

// Return pooled/pending/cold memory to the OS until the retained bytes are no
// more than `keep_bytes`. Returns the number of bytes actually munmap'd.
size_t global_pooled_stack_trim(size_t keep_bytes = 0);

GlobalStackPoolStats global_pooled_stack_stats();
}  // namespace photon