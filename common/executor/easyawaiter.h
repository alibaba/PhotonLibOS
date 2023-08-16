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

#include <easy/easy_io.h>
#include <easy/easy_uthread.h>
#include <photon/thread/awaiter.h>

namespace photon {

struct EasyContext {};

template <>
struct Awaiter<EasyContext> {
    easy_comutex_t mtx;
    Awaiter() {
        easy_comutex_init(&mtx);
        easy_comutex_cond_lock(&mtx);
    }
    ~Awaiter() { easy_comutex_cond_unlock(&mtx); }
    void suspend() { easy_comutex_cond_wait(&mtx); }
    void resume() {
        easy_comutex_cond_lock(&mtx);
        easy_comutex_cond_signal(&mtx);
        easy_comutex_cond_unlock(&mtx);
    }
};

}  // namespace photon
