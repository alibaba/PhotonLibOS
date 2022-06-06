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

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// size of easy_comutex_t
// is 40 bytes in 64-bit situation
struct __attribute__((packed)) placeholder_easy_comutex_t {
    char content[40];
};
typedef placeholder_easy_comutex_t easy_comutex_t;

#define easy_comutex_cond_broadcast v1_easy_comutex_cond_broadcast
#define easy_comutex_cond_lock v1_easy_comutex_cond_lock
#define easy_comutex_cond_signal v1_easy_comutex_cond_signal
#define easy_comutex_cond_timedwait v1_easy_comutex_cond_timedwait
#define easy_comutex_cond_unlock v1_easy_comutex_cond_unlock
#define easy_comutex_cond_wait v1_easy_comutex_cond_wait
#define easy_comutex_init v1_easy_comutex_init
#define easy_comutex_lock v1_easy_comutex_lock
#define easy_comutex_unlock v1_easy_comutex_unlock

int v1_easy_comutex_init(easy_comutex_t*);

int v1_easy_comutex_cond_wait(easy_comutex_t*);

int v1_easy_comutex_cond_timedwait(easy_comutex_t*, int64_t);

int v1_easy_comutex_cond_signal(easy_comutex_t*);

int v1_easy_comutex_cond_broadcast(easy_comutex_t*);

void v1_easy_comutex_lock(easy_comutex_t*);

void v1_easy_comutex_unlock(easy_comutex_t*);

#ifdef __cplusplus
}
#endif