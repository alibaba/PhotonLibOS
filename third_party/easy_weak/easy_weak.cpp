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

#include "easy_weak.h"

#include <cstdio>
#include <cerrno>

static const char* err_msg = "You are calling a weak implementation. Please link the easy library.\n";

#define RETURN_ERROR(code) do {         \
    fprintf(stderr, "%s", err_msg);   \
    errno = ENXIO;                      \
    return code;                        \
} while (0)

#ifdef __cplusplus
extern "C" {
#endif

int __attribute__((weak)) v1_easy_comutex_init(easy_comutex_t*) {
    RETURN_ERROR(-1);
}

int __attribute__((weak)) v1_easy_comutex_cond_wait(easy_comutex_t*) {
    RETURN_ERROR(-1);
}

int __attribute__((weak))
v1_easy_comutex_cond_timedwait(easy_comutex_t*, int64_t) {
    RETURN_ERROR(-1);
}

int __attribute__((weak)) v1_easy_comutex_cond_signal(easy_comutex_t*) {
    RETURN_ERROR(-1);
}

int __attribute__((weak)) v1_easy_comutex_cond_broadcast(easy_comutex_t*) {
    RETURN_ERROR(-1);
}

void __attribute__((weak)) v1_easy_comutex_lock(easy_comutex_t*) {
    RETURN_ERROR();
}

void __attribute__((weak)) v1_easy_comutex_unlock(easy_comutex_t*) {
    RETURN_ERROR();
}

#ifdef __cplusplus
}
#endif
