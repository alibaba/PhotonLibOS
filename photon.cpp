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

#include "photon.h"
#include <inttypes.h>

#include "io/fd-events.h"
#include "io/signal.h"
#include "io/aio-wrapper.h"
#include "thread/thread.h"
#include "thread/thread-pool.h"
#include "thread/stack-allocator.h"
#ifdef ENABLE_FSTACK_DPDK
#include "io/fstack-dpdk.h"
#endif
#include "io/reset_handle.h"
#include "net/curl.h"
#include "net/socket.h"
#include "fs/exportfs.h"
#include "common/callback.h"
#include <vector>

namespace photon {

using namespace fs;
using namespace net;

static bool reset_handle_registed = false;
static thread_local uint64_t g_event_engine = 0, g_io_engine = 0;

#define INIT_IO(name, prefix, ...)    if (INIT_IO_##name & io_engine) { if (prefix##_init(__VA_ARGS__) < 0) return -1; }
#define FINI_IO(name, prefix)    if (INIT_IO_##name & g_io_engine) { prefix##_fini(); }

class Shift {
    uint8_t _n;
public:
    constexpr Shift(uint64_t x) : _n(__builtin_ctz(x)) { }
    operator uint64_t() { return 1UL << _n; }
};

// Try to init master engine with the recommended order
static const Shift recommended_order[] = {
#if defined(__linux__)
    INIT_EVENT_EPOLL, INIT_EVENT_IOURING, INIT_EVENT_EPOLL_NG, INIT_EVENT_SELECT};
#else   // macOS, FreeBSD ...
    INIT_EVENT_KQUEUE, INIT_EVENT_SELECT};
#endif

int __photon_init(uint64_t event_engine, uint64_t io_engine, const PhotonOptions& options) {
    if (options.use_pooled_stack_allocator) {
        use_pooled_stack_allocator();
    }
    if (options.bypass_threadpool) {
        set_bypass_threadpool(true);
    }

    if (vcpu_init() < 0)
        return -1;

    const uint64_t ALL_ENGINES =
            INIT_EVENT_EPOLL   | INIT_EVENT_EPOLL_NG |
            INIT_EVENT_IOURING | INIT_EVENT_KQUEUE |
            INIT_EVENT_SELECT  | INIT_EVENT_IOCP |
            INIT_EVENT_EPOLL_NG;
    if (event_engine & ALL_ENGINES) {
        bool ok = false;
        for (auto x : recommended_order) {
            if ((x & event_engine) && fd_events_init(x) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            LOG_ERROR_RETURN(0, -1, "All master engines init failed");
        }
    }

    if ((INIT_EVENT_SIGNAL & event_engine) && sync_signal_init() < 0)
        return -1;

#ifdef ENABLE_FSTACK_DPDK
    INIT_IO(FSTACK_DPDK, fstack_dpdk);
#endif
    INIT_IO(EXPORTFS, exportfs)
    INIT_IO(LIBCURL, libcurl)
#ifdef __linux__
    INIT_IO(LIBAIO, libaio_wrapper, options.libaio_queue_depth)
    INIT_IO(SOCKET_EDGE_TRIGGER, et_poller)
#endif
    g_event_engine = event_engine;
    g_io_engine = io_engine;
    if (!reset_handle_registed) {
        pthread_atfork(nullptr, nullptr, &reset_all_handle);
        LOG_DEBUG("reset_all_handle registed ", VALUE(getpid()));
        reset_handle_registed = true;
    }
    return 0;
}

int init(uint64_t event_engine, uint64_t io_engine, const PhotonOptions& options) {
    return __photon_init(event_engine, io_engine, options);
}

static std::vector<Delegate<void>>& get_hook_vector() {
    thread_local std::vector<Delegate<void>> hooks;
    return hooks;
}

void fini_hook(Delegate<void> handler) {
    get_hook_vector().emplace_back(handler);
}

int fini() {
    for (auto h : get_hook_vector()) {
        h.fire();
    }
#ifdef __linux__
    FINI_IO(LIBAIO, libaio_wrapper)
    FINI_IO(SOCKET_EDGE_TRIGGER, et_poller)
#endif
    FINI_IO(LIBCURL, libcurl)
    FINI_IO(EXPORTFS, exportfs)
#ifdef ENABLE_FSTACK_DPDK
    FINI_IO(FSTACK_DPDK, fstack_dpdk)
#endif

    if (INIT_EVENT_SIGNAL & g_event_engine)
        sync_signal_fini();
    fd_events_fini();
    vcpu_fini();
    g_event_engine = g_io_engine = 0;
    return 0;
}

}
