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
#include <sys/types.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/common/timeout.h>

namespace photon {

const static uint32_t EVENT_READ = 1;
const static uint32_t EVENT_WRITE = 2;
const static uint32_t EVENT_ERROR = 4;
const static uint32_t EVENT_RWE = EVENT_READ | EVENT_WRITE | EVENT_ERROR;
const static uint32_t EDGE_TRIGGERED = 0x4000;
const static uint32_t ONE_SHOT = 0x8000;

const static int EOK = ENXIO;   // the Event of NeXt I/O

// Event engine is the abstraction of substrates like epoll,
// io-uring, kqueue, etc.
// Master event engine is the default one used by global functions
// of wait_for_fd_readable/writable(), and it is also invoked by
// the thread scheduler to wait for events when idle.
// Every vCPU that processes events has a dedicated master engine.
class MasterEventEngine {
public:
    virtual ~MasterEventEngine() = default;

    /**
     * @param interest EVENT_READ, EVENT_WRITE, or EVENT_ERROR
     * @return 0 for success, which means event arrived in time
     *         -1 for failure, could be timeout or interrupted by another thread
     */
    virtual int wait_for_fd(int fd, uint32_t interest, Timeout timeout) = 0;

    int wait_for_fd_readable(int fd, Timeout timeout = {}) {
        return wait_for_fd(fd, EVENT_READ, timeout);
    }

    int wait_for_fd_writable(int fd, Timeout timeout = {}) {
        return wait_for_fd(fd, EVENT_WRITE, timeout);
    }

    int wait_for_fd_error(int fd, Timeout timeout = {}) {
        return wait_for_fd(fd, EVENT_ERROR, timeout);
    }

    /**
     * @brief Wait for events, and fire them by photon::thread_interrupt()
     * @param timeout The *maximum* amount of time to sleep. May wake up
     *        earlier in the case of some events happened.
     * @return 0 if slept well, or -1 if error occurred.
     * @warning Do NOT invoke photon::usleep() or photon::sleep() in this function, because their
     *          implementations also rely on this function.
     */
    virtual ssize_t wait_and_fire_events(uint64_t timeout) = 0;

    virtual int cancel_wait() = 0;
};

inline int wait_for_fd_readable(int fd, Timeout timeout = {}) {
    return get_vcpu()->master_event_engine->wait_for_fd_readable(fd, timeout);
}

inline int wait_for_fd_writable(int fd, Timeout timeout = {}) {
    return get_vcpu()->master_event_engine->wait_for_fd_writable(fd, timeout);
}

inline int wait_for_fd_error(int fd, Timeout timeout = {}) {
    return get_vcpu()->master_event_engine->wait_for_fd_error(fd, timeout);
}

// Cascading event engine is used explicitly with a pointer, for complex
// scenarios that the master engine cannot handle, e.g., waiting for multiple
// events with a single invocation.
// Cascading event engines do NOT block the vCPU. Instead, they only block
// current thread, with the help of master event engine.
class CascadingEventEngine {
public:
    virtual ~CascadingEventEngine() = default;

    struct Event {
        int fd;
        uint32_t interests;    // bitwisely OR-ed EVENT_READ, EVENT_WRITE
        void* data;
    };

    /**
     * @brief `add_interests` should allow adding same fd on same event with same data
     */
    virtual int add_interest(Event e) = 0;

    /**
     * @brief The struct Event arg should be equal to that for `add_interest()`,
     * as some engine may identify events by `data`, while some may operate each event independently.
     */
    virtual int rm_interest(Event e) = 0;

    /**
     * @brief Wait for events, returns number of the arrived events, and their associated `data`
     * @note This call will not return until timeout, if there had been no events.
     * @param[out] data
     * @return -1 for error, positive integer for the number of events, 0 for no events and should run it again
     * @warning Do NOT block vcpu
     */
    virtual ssize_t wait_for_events(void** data, size_t count, Timeout timeout = {}) = 0;
};

template<typename Ctor> inline
int _fd_events_init(Ctor new_engine) {
    auto ee = new_engine();
    if (!ee)
        return -1;
    get_vcpu()->master_event_engine = ee;
    return 0;
}

#define DECLARE_MASTER_AND_CASCADING_ENGINE(name)           \
MasterEventEngine* new_##name##_master_engine();            \
CascadingEventEngine* new_##name##_cascading_engine();      \

DECLARE_MASTER_AND_CASCADING_ENGINE(epoll);
DECLARE_MASTER_AND_CASCADING_ENGINE(select);
// DECLARE_MASTER_AND_CASCADING_ENGINE(iouring);
DECLARE_MASTER_AND_CASCADING_ENGINE(kqueue);
DECLARE_MASTER_AND_CASCADING_ENGINE(epoll_ng);

struct iouring_args {
    bool is_master    = true;
    bool setup_sqpoll = false;
    bool setup_sq_aff = false;
    bool setup_iopoll = false;
    bool eager_submit = false;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle_ms = 1000;     // by default polls for 1s
};

void* new_iouring_event_engine(iouring_args args = {});

inline MasterEventEngine*
new_iouring_master_engine(iouring_args args = {}) {
    args.is_master = true;
    auto e = new_iouring_event_engine(args);
    return (MasterEventEngine*)e;
}

inline CascadingEventEngine*
new_iouring_cascading_engine(iouring_args args = {}) {
    args.is_master = false;
    auto e = new_iouring_event_engine(args);
    return (CascadingEventEngine*)e;
}

template<typename...Ts> inline MasterEventEngine*
new_master_event_engine(uint64_t master_engine, const Ts&...xs) {
    switch (master_engine) {
#ifdef __linux__
        case INIT_EVENT_EPOLL:
            return new_epoll_master_engine(xs...);
        case INIT_EVENT_EPOLL_NG:
            return new_epoll_ng_master_engine(xs...);
#endif
        case INIT_EVENT_SELECT:
            return new_select_master_engine(xs...);
#ifdef PHOTON_URING
        case INIT_EVENT_IOURING:
            return new_iouring_master_engine(xs...);
#endif
#ifdef __APPLE__
        case INIT_EVENT_KQUEUE:
            return new_kqueue_master_engine(xs...);
#endif
        default:
            return nullptr;
    }
}

inline int fd_events_init(MasterEventEngine* master_engine) {
    return !master_engine ? -1 :
        (get_vcpu()->master_event_engine = master_engine, 0);
}


inline int fd_events_init(uint64_t master_engine) {
    return fd_events_init(new_master_event_engine(master_engine));
}

inline int fd_events_fini() {
    reset_master_event_engine_default();
    return 0;
}

inline CascadingEventEngine* new_default_cascading_engine() {
#ifdef __APPLE__
    return new_kqueue_cascading_engine();
#else
    return new_epoll_cascading_engine();
#endif
}

#undef DECLARE_MASTER_AND_CASCADING_ENGINE

// TODO: implement select engine in separate cpp files
inline MasterEventEngine* new_select_master_engine() { return nullptr; }
inline CascadingEventEngine* new_select_cascading_engine() { return nullptr; }

} // namespace photon
