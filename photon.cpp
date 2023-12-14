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

#include "io/fd-events.h"
#include "io/signal.h"
#include "io/aio-wrapper.h"
#ifdef ENABLE_FSTACK_DPDK
#include "io/fstack-dpdk.h"
#endif
#include "io/reset_handle.h"
#include "net/curl.h"
#include "net/socket.h"
#include "fs/exportfs.h"

namespace photon {

using namespace fs;
using namespace net;

static bool reset_handle_registed = false;
static thread_local uint64_t g_event_engine = 0, g_io_engine = 0;

#define INIT_IO(name, prefix)    if (INIT_IO_##name & io_engine) { if (prefix##_init() < 0) return -1; }
#define FINI_IO(name, prefix)    if (INIT_IO_##name & g_io_engine) { prefix##_fini(); }

// Try to init master engine with the recommended order
#if defined(__linux__)
static const int recommended_order[] = {INIT_EVENT_EPOLL, INIT_EVENT_IOURING, INIT_EVENT_EPOLL_NG, INIT_EVENT_SELECT};
#else   // macOS, FreeBSD ...
static const int recommended_order[] = {INIT_EVENT_KQUEUE, INIT_EVENT_SELECT};
#endif

int init(uint64_t event_engine, uint64_t io_engine) {
    if (vcpu_init() < 0)
        return -1;

    if (event_engine != INIT_EVENT_NONE) {
        bool ok = false;
        for (auto each : recommended_order) {
            if ((each & event_engine) && fd_events_init(each) == 0) {
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
    INIT_IO(LIBAIO, libaio_wrapper)
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

int fini() {
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
