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

#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <vector>

#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>
#include <photon/io/fd-events.h>
#include "events_map.h"

namespace photon {
#ifndef EPOLLRDHUP
#define EPOLLRDHUP 0
#endif

// maps interface event(s) to epoll defined events
using EVMAP = EventsMap<EVUnderlay<EPOLLIN | EPOLLRDHUP, EPOLLOUT, EPOLLERR>>;
constexpr static EVMAP evmap;
constexpr static uint32_t ERRBIT = EVMAP::UNDERLAY_EVENT_ERROR;
constexpr static uint32_t READBITS =
    EVMAP::UNDERLAY_EVENT_READ | ERRBIT | EPOLLHUP;
constexpr static uint32_t WRITEBITS =
    EVMAP::UNDERLAY_EVENT_WRITE | ERRBIT | EPOLLHUP;

struct InFlightEvent {
    uint32_t interests = 0, _;
    void* reader_data;
    void* writer_data;
    void* error_data;
};

class EventEngineEPoll : public MasterEventEngine, public CascadingEventEngine {
public:
    int _evfd = -1;
    int _engine_fd = -1;
    int init() {
        int epfd = epoll_create(1);
        if (epfd < 0) LOG_ERRNO_RETURN(0, -1, "failed to epoll_create(1)");

        DEFER(if_close_fd(epfd));
        _engine_fd = epfd;
        int evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (evfd < 0) LOG_ERRNO_RETURN(0, -1, "failed to create eventfd");

        DEFER(if_close_fd(evfd));
        _evfd = evfd;
        int ret = ctl(evfd, EPOLL_CTL_ADD, EPOLLIN | EPOLLRDHUP | EPOLLET);
        if (ret < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to add eventfd(`) to epollfd(`) ",
                             evfd, epfd);

        epfd = evfd = -1;
        return 0;
    }
    virtual ~EventEngineEPoll() override {
        LOG_INFO("Finish event engine: epoll");
        if_close_fd(_engine_fd);
        if_close_fd(_evfd);
    }
    int if_close_fd(int& fd) {
        if (fd < 0) return 0;
        DEFER(fd = -1);
        return close(fd);
    }
    int ctl(int fd, int op, uint32_t events, int no_log_errno_1 = 0,
            int no_log_errno_2 = 0) {
        struct epoll_event ev;
        ev.events = events;  // EPOLLERR | EPOLLHUP always included
        ev.data.u64 = fd;
        int ret = epoll_ctl(_engine_fd, op, fd, &ev);
        if (ret < 0) {
            ERRNO err;
            if (err.no != no_log_errno_1 &&
                err.no != no_log_errno_2) {  // deleting a non-existing fd is
                                             // considered OK
                auto events = HEX(ev.events);
                auto data = ev.data.ptr;
                LOG_WARN("failed to call epoll_ctl(`, `, `, {`, `})",
                         VALUE(_engine_fd), VALUE(op), VALUE(fd), VALUE(events),
                         VALUE(data), err);
            }
            return -err.no;
        }
        return 0;
    }
    std::vector<InFlightEvent> _inflight_events;
    virtual int add_interest(Event e) override {
        if (e.fd < 0)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid file descriptor ", e.fd);
        if ((size_t)e.fd >= _inflight_events.size())
            _inflight_events.resize(e.fd * 2);
        auto& entry = _inflight_events[e.fd];
        if (e.interests & entry.interests) {
            if (((e.interests & entry.interests & EVENT_READ) &&
                 (entry.reader_data != e.data)) ||
                ((e.interests & entry.interests & EVENT_WRITE) &&
                 (entry.writer_data != e.data)) ||
                ((e.interests & entry.interests & EVENT_ERROR) &&
                 (entry.error_data != e.data))) {
                LOG_ERROR_RETURN(EALREADY, -1, "conflicted interest(s)");
            }
        }

        if (e.interests & EVENT_READ) entry.reader_data = e.data;
        if (e.interests & EVENT_WRITE) entry.writer_data = e.data;
        if (e.interests & EVENT_ERROR) entry.error_data = e.data;
        auto eint = entry.interests & (EVENT_READ | EVENT_WRITE | EVENT_ERROR);
        auto op = eint ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        if (op == EPOLL_CTL_MOD &&
            (e.interests & ONE_SHOT) != (entry.interests & ONE_SHOT)) {
            LOG_ERROR_RETURN(
                EINVAL, -1,
                "do not support ONE_SHOT on no-oneshot interested fd");
        }
        auto x = entry.interests |= e.interests;
        x &= (EVENT_READ | EVENT_WRITE | EVENT_ERROR);
        // since epoll oneshot shows totally different meanning of ONESHOT in
        // photon all epoll action keeps no oneshot
        auto events = evmap.translate_bitwisely(x);
        return ctl(e.fd, op, events);
    }
    virtual int rm_interest(Event e) override {
        if (e.fd < 0 || (size_t)e.fd >= _inflight_events.size())
            LOG_ERROR_RETURN(EINVAL, -1, "invalid file descriptor ", e.fd);
        auto& entry = _inflight_events[e.fd];
        auto intersection = e.interests & entry.interests &
                            (EVENT_READ | EVENT_WRITE | EVENT_ERROR);
        if (intersection == 0) return 0;

        auto x = (entry.interests ^= intersection) &
                 (EVENT_READ | EVENT_WRITE | EVENT_ERROR);
        if (e.interests & EVENT_READ) entry.reader_data = nullptr;
        if (e.interests & EVENT_WRITE) entry.writer_data = nullptr;
        if (e.interests & EVENT_ERROR) entry.error_data = nullptr;
        auto op = x ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        auto events = evmap.translate_bitwisely(x);
        if (op == EPOLL_CTL_DEL) {
            entry.interests = 0;
        }
        return ctl(e.fd, op, events);
    }
    epoll_event _events[16];
    uint16_t _events_remain = 0;
    int do_epoll_wait(uint64_t timeout) {
        assert(_events_remain == 0);
        uint8_t cool_down_ms = 1;
        // since timeout may less than 1ms
        // in such condition, timeout_ms should be at least 1
        // or it may call epoll_wait without any idle
        timeout = (timeout && timeout < 1024) ? 1 : timeout / 1024;
        while (_engine_fd > 0) {
            int ret = epoll_wait(_engine_fd, _events, LEN(_events), timeout);
            if (ret < 0) {
                ERRNO err;
                if (err.no == EINTR) continue;
                usleep(1024L * cool_down_ms);
                timeout = sat_sub(timeout, cool_down_ms);
                if (cool_down_ms < 16) {
                    cool_down_ms *= 2;
                    continue;
                }
                LOG_ERROR_RETURN(err.no, -1, "epoll_wait() failed ", err);
            }
            return _events_remain = ret;
        }
        return -1;
    }
    template <typename DataCB, typename FDCB>
    void wait_for_events(uint64_t timeout, const DataCB& datacb,
                         const FDCB& fdcb) {
        if (!_events_remain) {
            int ret = do_epoll_wait(timeout);
            if (ret < 0) return;
        }

        while (_events_remain && fdcb()) {
            auto& e = _events[--_events_remain];
            if ((int)e.data.u64 == _evfd) {
                uint64_t value;
                eventfd_read(_evfd, &value);
                continue;
            }
            assert(e.data.u64 < _inflight_events.size());
            if (e.data.u64 >= _inflight_events.size()) continue;
            auto& entry = _inflight_events[e.data.u64];
            if ((e.events & ERRBIT) && (entry.interests & EVENT_ERROR)) {
                auto data = entry.error_data;
                if (entry.interests & ONE_SHOT) {
                    rm_interest({.fd = (int)e.data.u64,
                                 .interests = EVENT_ERROR | ONE_SHOT,
                                 .data = nullptr});
                }
                datacb(data);
            }
            if ((e.events & READBITS) && (entry.interests & EVENT_READ)) {
                auto data = entry.reader_data;
                if (entry.interests & ONE_SHOT) {
                    rm_interest({.fd = (int)e.data.u64,
                                 .interests = EVENT_READ | ONE_SHOT,
                                 .data = nullptr});
                }
                datacb(data);
            }
            if ((e.events & WRITEBITS) && (entry.interests & EVENT_WRITE)) {
                auto data = entry.writer_data;
                if (entry.interests & ONE_SHOT) {
                    rm_interest({.fd = (int)e.data.u64,
                                 .interests = EVENT_WRITE | ONE_SHOT,
                                 .data = nullptr});
                }
                datacb(data);
            }
        }
    }
    virtual ssize_t wait_for_events(void** data, size_t count,
                                    uint64_t timeout = -1) override {
        int ret = get_vcpu()->master_event_engine->wait_for_fd_readable(
            _engine_fd, timeout);
        if (ret < 0) {
            return errno == ETIMEDOUT ? 0 : -1;
        }
        auto ptr = data;
        auto end = data + count;
        wait_for_events(
            0, [&](void* data) __INLINE__ { *ptr++ = data; },
            [&]()
                __INLINE__ {  // make sure each fd receives all possible events
                    return (end - ptr) >= 3;
                });
        if (ptr == data) {
            return 0;
        }
        return ptr - data;
    }
    virtual ssize_t wait_and_fire_events(uint64_t timeout = -1) override {
        ssize_t n = 0;
        wait_for_events(
            timeout,
            [&](void* data) __INLINE__ {
                assert(data);
                thread_interrupt((thread*)data, EOK);
                n++;
            },
            [&]() __INLINE__ { return true; });
        return n;
    }
    virtual int cancel_wait() override { return eventfd_write(_evfd, 1); }

    int wait_for_fd(int fd, uint32_t interests, uint64_t timeout) override {
        Event event{fd, interests | ONE_SHOT, CURRENT};
        int ret = add_interest(event);
        if (ret < 0) LOG_ERROR_RETURN(0, -1, "failed to add event interest");
        ret = thread_usleep(timeout);
        ERRNO err;
        if (ret == -1 && err.no == EOK) {
            return 0;  // Event arrived
        } else if (ret == 0) {
            rm_interest(event);  // Timeout
            errno = ETIMEDOUT;
            return -1;
        } else {
            rm_interest(event);  // Interrupted by other thread
            errno = err.no;
            return -1;
        }
    }
};

__attribute__((noinline)) static EventEngineEPoll* new_epoll_engine() {
    LOG_INFO("Init event engine: epoll");
    return NewObj<EventEngineEPoll>()->init();
}

MasterEventEngine* new_epoll_master_engine() { return new_epoll_engine(); }

CascadingEventEngine* new_epoll_cascading_engine() {
    return new_epoll_engine();
}

}  // namespace photon
