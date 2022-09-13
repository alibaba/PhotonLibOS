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
#include "io/signalfd.h"
#include "io/aio-wrapper.h"
#include "net/curl.h"
#include "net/socket.h"
#include "net/utils.h"
#include "fs/exportfs.h"

namespace photon {

static thread_local uint64_t g_event_engine = 0, g_io_engine = 0;

int init(uint64_t event_engine, uint64_t io_engine) {
    if (thread_init() < 0) return -1;

    if (event_engine & INIT_EVENT_EPOLL) {
        if (fd_events_epoll_init() < 0) return -1;
    } else if (event_engine & INIT_EVENT_IOURING) {
        if (fd_events_iouring_init() < 0) return -1;
    }
    if (event_engine & INIT_EVENT_SIGNALFD) {
        if (sync_signal_init() < 0) return -1;
    }

    if (io_engine & INIT_IO_LIBAIO) {
        if (libaio_wrapper_init() < 0) return -1;
    }
    if (io_engine & INIT_IO_LIBCURL) {
        if (net::libcurl_init() < 0) return -1;
    }
    if (io_engine & INIT_IO_SOCKET_EDGE_TRIGGER) {
        if (net::et_poller_init() < 0) return -1;
    }
    if (io_engine & INIT_IO_EXPORTFS) {
        if (fs::exportfs_init() < 0) return -1;
    }
    g_event_engine = event_engine;
    g_io_engine = io_engine;
    return 0;
}

int fini() {
    if (g_event_engine & INIT_EVENT_SIGNALFD) {
        sync_signal_fini();
    }
    if (g_io_engine & INIT_IO_LIBAIO) {
        libaio_wrapper_fini();
    }
    if (g_io_engine & INIT_IO_LIBCURL) {
        net::libcurl_fini();
    }
    if (g_io_engine & INIT_IO_SOCKET_EDGE_TRIGGER) {
        net::et_poller_fini();
    }
    if (g_io_engine & INIT_IO_EXPORTFS) {
        fs::exportfs_fini();
    }
    if (g_event_engine & (INIT_EVENT_EPOLL | INIT_EVENT_IOURING)) {
        fd_events_fini();
    }
    thread_fini();
    return 0;
}

}