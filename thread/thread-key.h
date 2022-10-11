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
#include <cstdlib>

namespace photon {

constexpr size_t THREAD_KEYS_MAX = 1024;

struct thread_local_storage;

using thread_key_t = uint64_t;

int thread_key_create(thread_key_t* key, void (* dtor)(void*));

void* thread_getspecific(thread_key_t key);

int thread_setspecific(thread_key_t key, const void* value);

int thread_key_delete(thread_key_t key);

void deallocate_tls();

}