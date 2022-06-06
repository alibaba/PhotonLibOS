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

namespace photon {

struct EasyMutex {
    easy_comutex_t mtx;
    EasyMutex() { easy_comutex_init(&mtx); }
};

struct EasyLock {
    EasyMutex &mtx;
    bool locked;
    EasyLock(EasyMutex &mtx) : mtx(mtx) {
        lock();
        locked = true;
    }
    ~EasyLock() {
        if (owns_lock()) unlock();
        locked = false;
    }
    bool owns_lock() { return locked; }
    void lock() { easy_comutex_lock(&mtx.mtx); }
    void unlock() { easy_comutex_unlock(&mtx.mtx); }
};

struct EasyCondLock {
    EasyMutex &mtx;
    bool locked;
    EasyCondLock(EasyMutex &mtx) : mtx(mtx) {
        lock();
        locked = true;
    }

    ~EasyCondLock() {
        if (owns_lock()) unlock();
        locked = false;
    }

    bool owns_lock() { return locked; }

    void lock() { easy_comutex_cond_lock(&mtx.mtx); }

    void unlock() { easy_comutex_cond_unlock(&mtx.mtx); }
};

struct EasyCond {
    EasyMutex &mtx;
    EasyCond(EasyMutex &mtx) : mtx(mtx) {}
    void wait() { easy_comutex_cond_wait(&mtx.mtx); }
    bool wait_for(EasyCondLock &lock, int64_t timeout) {
        return easy_comutex_cond_timedwait(&lock.mtx.mtx, timeout) == EASY_OK;
    }
    void notify_one() { easy_comutex_cond_signal(&mtx.mtx); }
    void notify_all() { easy_comutex_cond_broadcast(&mtx.mtx); }
    void lock() { easy_comutex_cond_lock(&mtx.mtx); }
    void unlock() { easy_comutex_cond_unlock(&mtx.mtx); }
};

struct EasyContext {
    using Cond = EasyCond;
    using CondLock = EasyCondLock;
    using Lock = EasyLock;
    using Mutex = EasyMutex;
};

}  // namespace photon
