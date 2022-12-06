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

#include "thread-key.h"
#include <string.h>
#include <photon/thread/thread.h>

namespace photon {

constexpr size_t THREAD_KEY_2NDLEVEL_SIZE = 32;
constexpr size_t THREAD_KEY_1STLEVEL_SIZE = (THREAD_KEYS_MAX + THREAD_KEY_2NDLEVEL_SIZE - 1) / THREAD_KEY_2NDLEVEL_SIZE;
constexpr size_t THREAD_DESTRUCTOR_ITERATIONS = 4;

/* An even sequence number means the key slot is unused */
static inline bool key_unused(uint64_t key) {
    return (key & 1) == 0;
}

/* A program would have to create and destroy a key 2^63 times to overflow the sequence */
static inline bool key_usable(uint64_t key) {
    return key < key + 2;
}

struct thread_key_data {
    uint64_t seq;
    void* data;
};

// Per thread storage for key data
struct thread_local_storage {
    thread_local_storage() {
        specific[0] = specific_1stblock;
    }
    thread_key_data specific_1stblock[THREAD_KEY_2NDLEVEL_SIZE] = {};
    thread_key_data* specific[THREAD_KEY_1STLEVEL_SIZE] = {};
    bool specific_used = false;
};

struct thread_key_struct {
    uint64_t seq;
    void (* dtor)(void*);
};

// Global keys
static thread_key_struct thread_keys[THREAD_KEYS_MAX] = {};

int thread_key_create(thread_key_t* key, void (* destr)(void*)) {
    for (uint64_t index = 0; index < THREAD_KEYS_MAX; ++index) {
        /* Find a slot in thread_keys which is unused. */
        uint64_t seq = thread_keys[index].seq;
        if (key_unused(seq) && key_usable(seq) &&
            __sync_bool_compare_and_swap(&thread_keys[index].seq, seq, seq + 1)) {
            thread_keys[index].dtor = destr;
            *key = index;
            return 0;
        }
    }
    return EAGAIN;
}

void* thread_getspecific(thread_key_t key) {
    auto tls = (thread_local_storage*) (((partial_thread*) CURRENT)->tls);
    if (!tls) {
        return nullptr;
    }
    thread_key_data* data;
    if (key < THREAD_KEY_2NDLEVEL_SIZE)
        /* Special case access to the first 2nd-level block. This is the usual case. */
        data = &tls->specific_1stblock[key];
    else {
        if (key >= THREAD_KEYS_MAX)
            return nullptr;
        uint64_t idx1st = key / THREAD_KEY_2NDLEVEL_SIZE;
        uint64_t idx2nd = key % THREAD_KEY_2NDLEVEL_SIZE;
        thread_key_data* level2 = tls->specific[idx1st];
        if (level2 == nullptr)
            /* Not allocated, therefore no data. */
            return nullptr;
        data = &level2[idx2nd];
    }
    // Check seq against the global thread_keys
    void* result = data->data;
    if (result != nullptr) {
        if (data->seq != thread_keys[key].seq)
            result = data->data = nullptr;
    }
    return result;
}

int thread_setspecific(thread_key_t key, const void* value) {
    if (key >= THREAD_KEYS_MAX)
        return EINVAL;
    uint64_t seq = thread_keys[key].seq;
    if (key_unused(seq))
        return EINVAL;

    auto tls = (thread_local_storage*) (((partial_thread*) CURRENT)->tls);
    if (tls == nullptr) {
        tls = new thread_local_storage;
        ((partial_thread*) CURRENT)->tls = tls;
    }

    thread_key_data* level2;
    if (key < THREAD_KEY_2NDLEVEL_SIZE) {
        /* Special case access to the first 2nd-level block. This is the usual case. */
        level2 = &tls->specific_1stblock[key];
        if (value != nullptr)
            tls->specific_used = true;
    } else {
        /* This is the second level array.  Allocate it if necessary. */
        uint64_t idx1st = key / THREAD_KEY_2NDLEVEL_SIZE;
        uint64_t idx2nd = key % THREAD_KEY_2NDLEVEL_SIZE;
        level2 = tls->specific[idx1st];
        if (level2 == nullptr) {
            if (value == nullptr)
                return 0;
            level2 = (thread_key_data*) calloc(THREAD_KEY_2NDLEVEL_SIZE, sizeof(*level2));
            if (level2 == nullptr)
                return ENOMEM;
            tls->specific[idx1st] = level2;
        }
        level2 = &level2[idx2nd];
        tls->specific_used = true;
    }

    level2->seq = seq;
    level2->data = (void*) value;
    return 0;
}

int thread_key_delete(thread_key_t key) {
    int result = EINVAL;
    if (key < THREAD_KEYS_MAX) {
        uint64_t seq = thread_keys[key].seq;
        if (!key_unused(seq) && __sync_bool_compare_and_swap(&thread_keys[key].seq, seq, seq + 1))
            /* We deleted a valid key by making the seq even again */
            result = 0;
    }
    return result;
}

void deallocate_tls(void** tls_) {
    size_t round = 0;
    auto ptr = tls_ ? *tls_ : ((partial_thread*)CURRENT)->tls;
    auto tls = (thread_local_storage*)ptr;
    if (!tls) return;                           /* No key was ever created */
    if (!tls->specific_used) goto free_tls;     /* No specific was ever set */

    do {
        uint64_t idx = 0;
        tls->specific_used = false;
        for (auto level2 : tls->specific) {
            if (level2 != nullptr) {
                for (uint64_t inner = 0; inner < THREAD_KEY_2NDLEVEL_SIZE; ++inner, ++idx) {
                    void* data = level2[inner].data;
                    if (data == nullptr) {
                        continue;
                    }
                    /* Always clear the data.  */
                    level2[inner].data = nullptr;
                    /* Make sure the data corresponds to a valid key. */
                    if (level2[inner].seq == thread_keys[idx].seq && thread_keys[idx].dtor != nullptr)
                        /* Call the user-provided destructor.  */
                        thread_keys[idx].dtor(data);
                }
            } else {
                idx += THREAD_KEY_1STLEVEL_SIZE;
            }
        }
        if (!tls->specific_used) {
            /* Usually no data has been modified,
             * unless users have called thread_setspecific in the destructor */
            goto free_key;
        }
    } /* We only repeat the process a fixed number of times. */
    while (++round < THREAD_DESTRUCTOR_ITERATIONS);

    /* Just clear the memory of the first block for reuse.  */
    memset(&tls->specific_1stblock, 0, sizeof(tls->specific_1stblock));

free_key:
    /* Free the memory for the other blocks.  */
    for (uint64_t cnt = 1; cnt < THREAD_KEY_1STLEVEL_SIZE; ++cnt) {
        thread_key_data* level2 = tls->specific[cnt];
        if (level2 != nullptr) {
            free(level2);
            tls->specific[cnt] = nullptr;
        }
    }
    tls->specific_used = false;

free_tls:
    delete tls;
    if (tls_)
        *tls_ = nullptr;
}

}