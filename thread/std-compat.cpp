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

#include <photon/thread/workerpool.h>
#include <photon/common/alog.h>

#include "std-compat.h"

static inline void throw_system_error(int err_num, const char* msg) {
    LOG_ERROR(msg, ": ", ERRNO(err_num));
    throw std::system_error(std::error_code(err_num, std::generic_category()), msg);
}

template<typename FMT, typename...Ts>
static inline void log_error(FMT fmt, Ts&& ...xs) {
    LOG_ERROR(fmt, std::forward<Ts>(xs)...);
}

namespace photon_std {

static photon::WorkPool* g_work_pool = nullptr;

void __throw_system_error(int err_num, const char* msg) {
    throw_system_error(err_num, msg);
}

void thread::do_migrate() {
    if (g_work_pool) {
        g_work_pool->thread_migrate(m_th);
    }
}

int work_pool_init(int vcpu_num, int event_engine, int io_engine) {
    if (g_work_pool != nullptr) {
        log_error("work pool has been initialized");
        return -1;
    }
    if (vcpu_num < 1) {
        log_error("Invalid vcpu_num");
        return -1;
    }
    g_work_pool = new photon::WorkPool(vcpu_num, event_engine, io_engine, -1);
    return 0;
}

int work_pool_fini() {
    if (g_work_pool == nullptr) {
        log_error("work pool is not initialized");
        return -1;
    }
    delete g_work_pool;
    g_work_pool = nullptr;
    return 0;
}

namespace this_thread {

void migrate() {
    if (unlikely(!g_work_pool)) {
        throw_system_error(EPERM, "this_thread::migrate: work pool is not initialized");
    }
    g_work_pool->thread_migrate();
}

}

}