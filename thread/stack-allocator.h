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

#include <photon/thread/thread.h>
#include <stddef.h>

namespace photon {

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
inline void use_pooled_stack_allocator() {
    set_photon_thread_stack_allocator({&pooled_stack_alloc, nullptr},
                                      {&pooled_stack_dealloc, nullptr});
}
}  // namespace photon