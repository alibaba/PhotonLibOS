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

#include <inttypes.h>
#include <photon/common/callback.h>

namespace photon {

#define SHIFT(n) (1 << n)

const uint64_t INIT_EVENT_NONE = 0;
const uint64_t INIT_EVENT_EPOLL = SHIFT(0);
const uint64_t INIT_EVENT_IOURING = SHIFT(1);
const uint64_t INIT_EVENT_SELECT = SHIFT(2);
const uint64_t INIT_EVENT_KQUEUE = SHIFT(3);
const uint64_t INIT_EVENT_IOCP = SHIFT(4);
const uint64_t INIT_EVENT_EPOLL_NG = SHIFT(5);
const uint64_t INIT_EVENT_IOURING_SQPOLL = SHIFT(6);
const uint64_t INIT_EVENT_IOURING_SQ_AFF = SHIFT(7);
const uint64_t INIT_EVENT_IOURING_IOPOLL = SHIFT(8);
const uint64_t INIT_EVENT_SIGNAL = SHIFT(10);

const uint64_t INIT_IO_NONE = 0;
const uint64_t INIT_IO_LIBAIO = SHIFT(0);
const uint64_t INIT_IO_LIBCURL = SHIFT(1);
const uint64_t INIT_IO_SOCKET_EDGE_TRIGGER = SHIFT(2);
const uint64_t INIT_IO_EXPORTFS = SHIFT(10);
const uint64_t INIT_IO_FSTACK_DPDK = SHIFT(20);

#if defined(__linux__)
const uint64_t INIT_EVENT_DEFAULT = INIT_EVENT_IOURING | INIT_EVENT_EPOLL |
                                    INIT_EVENT_SELECT  | INIT_EVENT_SIGNAL;
const uint64_t INIT_IO_DEFAULT    = INIT_IO_LIBAIO     | INIT_IO_LIBCURL;
#elif defined(_WIN32)
const uint64_t INIT_EVENT_DEFAULT = INIT_EVENT_IOCP | INIT_EVENT_SELECT;
const uint64_t INIT_IO_DEFAULT    = INIT_IO_LIBCURL;
#else   // macOS, FreeBSD ...
const uint64_t INIT_EVENT_DEFAULT = INIT_EVENT_KQUEUE | INIT_EVENT_SELECT |
                                    INIT_EVENT_SIGNAL;
const uint64_t INIT_IO_DEFAULT    = INIT_IO_LIBCURL;
#endif

#undef SHIFT

// Values for PhotonOptions::use_pooled_stack_allocator, selecting which
// allocator provides photon thread stacks. The historical `true` equals
// STACK_ALLOCATOR_POOLED, and the field stays 1 byte wide as the former bool,
// so both source and struct layout (ABI) remain compatible.
const uint8_t STACK_ALLOCATOR_DEFAULT = 0;         // no pooling (malloc-based)
const uint8_t STACK_ALLOCATOR_POOLED = 1;          // per-vcpu (thread-local) pool
const uint8_t STACK_ALLOCATOR_GLOBAL_POOLED = 2;   // process-wide pool

struct PhotonOptions {
    int libaio_queue_depth = 32;
    uint32_t iouring_sq_thread_cpu;
    uint32_t iouring_sq_thread_idle_ms = 1000;     // by default polls for 1s
    // One of the STACK_ALLOCATOR_* values above.
    uint8_t use_pooled_stack_allocator = STACK_ALLOCATOR_DEFAULT;
    bool bypass_threadpool = false;
};

/**
 * @brief Initialize the main photon thread and ancillary threads by flags.
 *        Ancillary threads will be running in background.
 * @return 0 for success
 */
int init(uint64_t event_engine = INIT_EVENT_DEFAULT,
         uint64_t io_engine = INIT_IO_DEFAULT,
         const PhotonOptions& options = {});

/**
 * @brief Destroy/join ancillary threads, and finish the main thread.
 */
int fini();

/**
 * @brief add callbacks on fini()
 */
void fini_hook(Delegate<void> handler);

}
