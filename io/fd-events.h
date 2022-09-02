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
#include <photon/thread/thread.h>
#include <photon/io/iouring-wrapper.h>

namespace photon {

const static uint32_t EVENT_READ = 1;
const static uint32_t EVENT_WRITE = 2;
const static uint32_t EVENT_ERROR = 4;
const static uint32_t EDGE_TRIGGERED = 0x4000;
const static uint32_t ONE_SHOT = 0x8000;

const static int EOK = ENXIO;   // the Event of NeXt I/O

class MasterEventEngine {
public:
    virtual ~MasterEventEngine() = default;

    /**
     * @param interests bitwisely OR-ed EVENT_READ, EVENT_WRITE
     * @return 0 for success, which means event arrived in time
     *         -1 for failure, could be timeout or interrupted by another thread
     */
    virtual int wait_for_fd(int fd, uint32_t interests, uint64_t timeout) = 0;

    int wait_for_fd_readable(int fd, uint64_t timeout = -1) {
        return wait_for_fd(fd, EVENT_READ, timeout);
    }

    int wait_for_fd_writable(int fd, uint64_t timeout = -1) {
        return wait_for_fd(fd, EVENT_WRITE, timeout);
    }

    /**
     * @brief Wait for events, and fire them by photon::thread_interrupt()
     * @param timeout The *maximum* amount of time to sleep. The sleep can be interrupted by invoking
     *        wait_and_fire_events(-1) from another vcpu thread, and get -1 && errno == EINTR.
     * @return 0 if slept well, or -1 if error occurred.
     * @warning Do NOT invoke photon::usleep() or photon::sleep() in this function, because their
     *          implementations also rely on this function.
     */
    virtual ssize_t wait_and_fire_events(uint64_t timeout = -1) = 0;

    virtual int cancel_wait() = 0;
};

inline int wait_for_fd_readable(int fd, uint64_t timeout = -1) {
    return get_vcpu()->master_event_engine->wait_for_fd_readable(fd, timeout);
}

inline int wait_for_fd_writable(int fd, uint64_t timeout = -1) {
    return get_vcpu()->master_event_engine->wait_for_fd_writable(fd, timeout);
}

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
     * @param[out] data
     * @return -1 for error, positive integer for the number of events, 0 for no events and should run it again
     * @warning Do NOT block vcpu
     */
    virtual ssize_t wait_for_events(void** data, size_t count, uint64_t timeout = -1) = 0;
};

MasterEventEngine* new_epoll_master_engine();
MasterEventEngine* new_select_master_engine();
MasterEventEngine* new_iouring_master_engine();
// MasterEventEngine* new_kqueue_master_engine();
// MasterEventEngine* new_iocp_master_engine();

CascadingEventEngine* new_epoll_cascading_engine();
CascadingEventEngine* new_select_cascading_engine();
CascadingEventEngine* new_iouring_cascading_engine();
// CascadingEventEngine* new_kqueue_cascading_engine();
// CascadingEventEngine* new_iocp_cascading_engine();

inline int fd_events_epoll_init() {
    assert(is_master_event_engine_default());
    auto ee = get_vcpu()->master_event_engine = new_epoll_master_engine();
    return -!ee;
}

inline int fd_events_select_init() {
    assert(is_master_event_engine_default());
    auto ee = get_vcpu()->master_event_engine = new_select_master_engine();
    return -!ee;
}

inline int fd_events_iouring_init() {
    assert(is_master_event_engine_default());
    auto ee = get_vcpu()->master_event_engine = new_iouring_master_engine();
    return -!ee;
}

inline int fd_events_init() {
    return fd_events_epoll_init();
}

inline int fd_events_fini() {
    reset_master_event_engine_default();
    return 0;
}

// a helper class to translate events into underlay representation
template<uint32_t UNDERLAY_EVENT_READ_,
        uint32_t UNDERLAY_EVENT_WRITE_,
        uint32_t UNDERLAY_EVENT_ERROR_>
struct EventsMap {
    const static uint32_t UNDERLAY_EVENT_READ = UNDERLAY_EVENT_READ_;
    const static uint32_t UNDERLAY_EVENT_WRITE = UNDERLAY_EVENT_WRITE_;
    const static uint32_t UNDERLAY_EVENT_ERROR = UNDERLAY_EVENT_ERROR_;
    static_assert(UNDERLAY_EVENT_READ != UNDERLAY_EVENT_WRITE, "...");
    static_assert(UNDERLAY_EVENT_READ != UNDERLAY_EVENT_ERROR, "...");
    static_assert(UNDERLAY_EVENT_ERROR != UNDERLAY_EVENT_WRITE, "...");
    static_assert(UNDERLAY_EVENT_READ, "...");
    static_assert(UNDERLAY_EVENT_WRITE, "...");
    static_assert(UNDERLAY_EVENT_ERROR, "...");

    uint64_t ev_read, ev_write, ev_error;

    EventsMap(uint64_t event_read, uint64_t event_write, uint64_t event_error) {
        ev_read = event_read;
        ev_write = event_write;
        ev_error = event_error;
        assert(ev_read);
        assert(ev_write);
        assert(ev_error);
        assert(ev_read != ev_write);
        assert(ev_read != ev_error);
        assert(ev_error != ev_write);
    }

    uint32_t translate_bitwisely(uint64_t events) const {
        uint32_t ret = 0;
        if (events & ev_read)
            ret |= UNDERLAY_EVENT_READ;
        if (events & ev_write)
            ret |= UNDERLAY_EVENT_WRITE;
        if (events & ev_error)
            ret |= UNDERLAY_EVENT_ERROR;
        return ret;
    }

    uint32_t translate_byval(uint64_t event) const {
        if (event == ev_read)
            return UNDERLAY_EVENT_READ;
        if (event == ev_write)
            return UNDERLAY_EVENT_WRITE;
        if (event == ev_error)
            return UNDERLAY_EVENT_ERROR;
    }
};

} // namespace photon


