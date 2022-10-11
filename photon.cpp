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

#include "thread/thread.h"
#include "io/fd-events.h"
#include "io/signal.h"
#include "io/aio-wrapper.h"
#include "net/curl.h"
#include "net/socket.h"
#include "fs/exportfs.h"

namespace photon {

using namespace fs;
using namespace net;
inline int fd_events_signal_init() { return sync_signal_init(); }
inline int fd_events_signal_fini() { return sync_signal_fini(); }
static thread_local uint64_t g_event_engine = 0, g_io_engine = 0;

#define INIT(cond, x)       if (cond) { if (x##_init() < 0) return -1; }
#define INIT_EVENT(flag, x) INIT(INIT_EVENT_##flag & event_engine, fd_events_##x)
#define INIT_IO(flag, x)    INIT(INIT_IO_##flag & io_engine, x)
int init(uint64_t event_engine, uint64_t io_engine) {
    INIT(1, vcpu);
#if defined(__linux__)
    INIT_EVENT(EPOLL, epoll)
#ifdef PHOTON_URING
    else INIT_EVENT(IOURING, iouring)
#endif  // PHOTON_URING
#elif defined(__APPLE__)
    INIT_EVENT(KQUEUE, kqueue)
#endif
    INIT_EVENT(SIGNAL, signal)
    INIT_IO(LIBCURL, libcurl)
#ifdef __linux__
    INIT_IO(LIBAIO, libaio_wrapper)
    INIT_IO(SOCKET_EDGE_TRIGGER, et_poller)
#endif
    INIT_IO(EXPORTFS, exportfs)
    g_event_engine = event_engine;
    g_io_engine = io_engine;
    return 0;
}

#define FINI(cond, x)       if (cond) { x##_fini(); }
#define FINI_EVENT(flag, x) FINI(INIT_EVENT_##flag & g_event_engine, fd_events_##x)
#define FINI_IO(flag, x)    FINI(INIT_IO_##flag & g_io_engine, x)
int fini() {
    FINI_EVENT(SIGNAL, signal)
#ifdef __linux__
    FINI_IO(LIBAIO, libaio_wrapper)
    FINI_IO(SOCKET_EDGE_TRIGGER, et_poller)
#endif
    FINI_IO(LIBCURL, libcurl)
    FINI_IO(EXPORTFS, exportfs)
#if defined(__linux__)
    FINI_EVENT(EPOLL, epoll)
#ifdef PHOTON_URING
    else FINI_EVENT(IOURING, iouring)
#endif // PHOTON_URING
#elif defined(__APPLE__)
    FINI_EVENT(KQUEUE, kqueue)
#endif
    FINI(1, vcpu);
    g_event_engine = g_io_engine = 0;
    return 0;
}

}