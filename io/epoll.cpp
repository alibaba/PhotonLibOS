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
#include <bitset>

#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>
#include <photon/io/fd-events.h>
#include "events_map.h"
#include "reset_handle.h"

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

class EPoll : public EventEngine, public ResetHandle {
public:
    int _evfd = -1;
    int _epfd = -1;
    int init() {
        int epfd = epoll_create(1);
        if (epfd < 0) LOG_ERRNO_RETURN(0, -1, "failed to epoll_create(1)");

        DEFER(if_close_fd(epfd));
        int evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (evfd < 0) LOG_ERRNO_RETURN(0, -1, "failed to create eventfd");

        DEFER(if_close_fd(evfd));
        int ret = ctl(evfd, EPOLL_CTL_ADD, EPOLLIN | EPOLLRDHUP | EPOLLET);
        if (ret < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to add eventfd(`) to epollfd(`) ", evfd, epfd);

        _epfd = epfd;
        _evfd = evfd;
        epfd = evfd = -1;
        return 0;
    }
    int reset() override {
        if_close_fd(_epfd);    // close original fd
        if_close_fd(_evfd);
        return init();              // re-init
    }
    virtual ~EPoll() override {
        LOG_INFO("Finish event engine: epoll");
        if_close_fd(_epfd);
        if_close_fd(_evfd);
    }
    int if_close_fd(int& fd) {
        if (fd < 0) return 0;
        DEFER(fd = -1);
        return close(fd);
    }
    int ctl(int fd, int op, uint32_t events) {
        struct epoll_event ev;
        ev.events = events;  // EPOLLERR | EPOLLHUP always included
        ev.data.u64 = fd;
        int ret = epoll_ctl(_epfd, op, fd, &ev);
        if (ret < 0) {
            ERRNO err;
            auto events = HEX(ev.events);
            auto data = ev.data.ptr;
            LOG_WARN("failed to call epoll_ctl(`, `, `, {`, `})",
                        VALUE(_epfd), VALUE(op), VALUE(fd), VALUE(events),
                        VALUE(data), err);
            return -err.no;
        }
        return 0;
    }
    epoll_event _events[128];
    int do_epoll_wait(uint64_t timeout) {
        uint32_t cool_down_ms = 1;
        // since timeout may less than 1ms
        // in such condition, timeout_ms should be at least 1
        // or it may call epoll_wait without any idle
        timeout = (timeout && timeout < 1024) ? 1 : timeout / 1024;
        timeout &= 0x7fffffff;  // make sure less than INT32_MAX
        while (_epfd > 0) {
            int ret = ::epoll_wait(_epfd, _events, LEN(_events), timeout);
            if (ret < 0) {
                ERRNO err;
                if (err.no == EINTR) continue;
                ::usleep(1024L * cool_down_ms);
                if (cool_down_ms > 16)
                    LOG_ERROR_RETURN(err.no, -1, "epoll_wait() failed ", err);
                timeout = sat_sub(timeout, cool_down_ms);
                cool_down_ms *= 2;
            }
            return ret;
        }
        return -1;
    }
};

// parallel events of read, write and error
class ParallelRWE : public EPoll {
public:
    struct InFlightEvent {
        uint32_t interests = 0, flags = 0;
        void* reader_data;
        void* writer_data;
        void* error_data;
    };
    const static uint32_t REGISTERED = 0x10000;
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
            LOG_ERROR_RETURN(EINVAL, -1,
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
        if (e.interests & ONE_SHOT_SYNC) return 0;
        auto op = x ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        auto events = evmap.translate_bitwisely(x);
        if (op == EPOLL_CTL_DEL) {
            entry.interests = 0;
        }
        return ctl(e.fd, op, events);
    }
    int _events_remain = 0;
    template <typename DataCB, typename FDCB>
    int reap_events(uint64_t timeout, const FDCB& condcb, const DataCB& datacb) {
        if (_events_remain <= 0) {
            _events_remain = do_epoll_wait(timeout);
            if (_events_remain < 0) return _events_remain;
        }
        while (_events_remain && condcb()) {
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
                datacb(entry.error_data);
                entry.error_data = 0;
                entry.interests ^= EVENT_ERROR;
            }
            if ((e.events & READBITS) && (entry.interests & EVENT_READ)) {
                datacb(entry.reader_data);
                entry.reader_data = 0;
                entry.interests ^= EVENT_READ;
            }
            if ((e.events & WRITEBITS) && (entry.interests & EVENT_WRITE)) {
                datacb(entry.writer_data);
                entry.writer_data = 0;
                entry.interests ^= EVENT_WRITE;
            }
        }
        return 0;
    }
    int reset() override {
        _events_remain = 0;
        _inflight_events.clear();
        return EPoll::reset();
    }
};

class MasterEpoll : public ParallelRWE {
public:
    ssize_t wait_and_fire_events(uint64_t timeout = -1) override {
        ssize_t n = 0;
        reap_events(timeout,
            [&]() __INLINE__ { return true; },
            [&](void* data) __INLINE__ {
                assert(data);
                thread_interrupt((thread*)data, EOK);
                n++;
            });
        return n;
    }
    int cancel_wait() override {
        return eventfd_write(_evfd, 1);
    }
};

class DyanmicBitset {
public:
    const static size_t UNIT = 256;
    static_assert((UNIT & (UNIT-1)) == 0, "UNIT must be 2^n");
    size_t _size;
    std::vector<std::bitset<UNIT> > _bitset;
    DyanmicBitset() {
        _bitset.resize(1);
        _size = UNIT;
    }
    bool test(size_t i) {
        return (i < _size) ? _bitset[i/UNIT].test(i%UNIT) : false;
    }
    void set(size_t i, bool flag) {
        if (unlikely(i > _size)) {
            size_t j = 1; // 00011010 ==> 00100000 (e.g. 1 << (8-3))
            _size = j << (sizeof(i) * 8 - __builtin_clz(i));
            _bitset.resize(_size / UNIT);
        }
        _bitset[i/UNIT].set(i%UNIT, flag);
    }
    void clear() {
        _bitset.clear();
    }
};

class CascadingEpoll : public EPoll {
    DyanmicBitset _fd_in_epoll;
    virtual int add_interest(Event e) override {
        if (unlikely(e.fd < 0))
            LOG_ERROR_RETURN(EINVAL, -1, "invalid file descriptor ", e.fd);

        auto op = _fd_in_epoll.test(e.fd) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        _fd_in_epoll.set(e.fd, true);
        auto x = e.interests & (EVENT_READ | EVENT_WRITE | EVENT_ERROR);
        auto events = evmap.translate_bitwisely(x);
        if (e.interests & ONE_SHOT)
            x |= EPOLLONESHOT;
        return ctl(e.fd, op, events);
    }
    virtual int rm_interest(Event e) override {
        if (unlikely(e.fd < 0 || _fd_in_epoll.test(e.fd) == false))
            LOG_ERROR_RETURN(EINVAL, -1, "invalid file descriptor ", e.fd);

        return ctl(e.fd, EPOLL_CTL_DEL, 0);
    }
    CascadingWaiter _waiter;
    virtual int wait_for_events(void** data, size_t count,
                                    Timeout timeout) override {
        if (!data || !count)
            LOG_ERROR_RETURN(EINVAL, -1, "both data(`) and count(`) must not be 0", data, count);
        _waiter.wait(_epfd, timeout);
        int ret = do_epoll_wait(0);
        if (ret < 0) return ret;
        if ((size_t)ret < count) count = ret;
        for (size_t i = 0; i < count; ++i) {
            *data++ = _events[i].data.ptr;
        }
        return count;
    }
    int cancel_wait() override {
        _waiter.cancel();
        return 0;
    }
    int reset() override {
        _fd_in_epoll.clear();
        return EPoll::reset();
    }
};

class CascadingEpol_PRWE : public ParallelRWE {
public:
    CascadingWaiter _waiter;
    virtual int wait_for_events(void** data, size_t count,
                                    Timeout timeout) override {
        if (!data || !count)
            LOG_ERROR_RETURN(EINVAL, -1, "both data(`) and count(`) must not be 0", data, count);

        _waiter.wait(_epfd, timeout);

        auto ptr = data;
        auto end = data + count;
        reap_events(0,
            [&]() __INLINE__ { return ptr < end; },
            [&](void* data) __INLINE__ { *ptr++ = data; });
        return ptr - data;
    }
    int cancel_wait() override {
        _waiter.cancel();
        return 0;
    }
};

EventEngine* new_epoll_master_engine() {
    LOG_INFO("Init event engine: master epoll");
    return NewObj<MasterEpoll>()->init();
}

EventEngine* new_epoll_cascading_engine(int flags) {
    bool prwe = flags & CASCADING_FLAG_PARALLEL_RWE;
    LOG_INFO("Init event engine: cascading epoll (parallel read/write/error = `)", prwe);
    if (likely(!prwe)) {
        return NewObj<CascadingEpoll>()->init();
    } else {
        return NewObj<CascadingEpol_PRWE>()->init();
    }
}

}  // namespace photon
