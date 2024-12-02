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

#include <photon/io/fd-events.h>
#include <inttypes.h>
#include <unistd.h>
#include <vector>
#include <sys/event.h>
#include <photon/common/alog.h>
#include <photon/thread/thread.h>
#include "events_map.h"
#include "reset_handle.h"

namespace photon {

constexpr static EventsMap<EVUnderlay<EVFILT_READ, EVFILT_WRITE, EVFILT_EXCEPT>>
    evmap;

constexpr static struct timespec tm00 = {0, 0};

class KQueue : public MasterEventEngine, public CascadingEventEngine, public ResetHandle {
public:
    struct InFlightEvent {
        uint32_t interests = 0;
        void* reader_data;
        void* writer_data;
        void* error_data;
    };
    struct kevent _events[32];
    int _kq = -1;
    uint32_t _n = 0;    // # of events to submit

    int init() {
        if (_kq >= 0)
            LOG_ERROR_RETURN(EALREADY, -1, "already init-ed");

        _kq = kqueue();
        if (_kq < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to create kqueue()");

        LOG_DEBUG("kqueue_fd = ", _kq);
        if (enqueue(_kq, EVFILT_USER, EV_ADD | EV_CLEAR, 0, nullptr, true) < 0) {
            DEFER({ close(_kq); _kq = -1; });
            LOG_ERRNO_RETURN(0, -1, "failed to setup self-wakeup EVFILT_USER event by kevent()");
        }
        return 0;
    }

    int reset() override {
        LOG_INFO("Reset event engine: kqueue");
        _kq = -1;                   // kqueue fd is not inherited from the parent process
        _inflight_events.clear();   // reset members
        _n = 0;
        return init();              // re-init
    }

    ~KQueue() override {
        LOG_INFO("Finish event engine: kqueue");
        // if (_n > 0) LOG_INFO(VALUE(_events[0].ident), VALUE(_events[0].filter), VALUE(_events[0].flags));
        // assert(_n == 0);
        if (_kq >= 0)
            close(_kq);
    }

    int do_kevent(const struct kevent *changelist, int nchanges,
                        struct kevent *eventlist,  int nevents,
                        const struct timespec *timeout, int ntry = 5) {
        int ret;
        for (int i = 1; i <= ntry; ++i) {
            ret = kevent(_kq, changelist, nchanges, eventlist, nevents, timeout);
            if (likely(ret >= 0)) return ret;
            LOG_ERROR("failed to call kevent(`, `)", VALUE(nchanges), VALUE(nevents), ERRNO());
            usleep(100 * 1000);
        }
        return ret;
    }

    int enqueue(int fd, short event, uint16_t action, uint32_t event_flags, void* udata, bool immediate = false) {
        assert(_n < LEN(_events));
        auto entry = &_events[_n++];
        EV_SET(entry, fd, event, action, event_flags, 0, udata);
        if (immediate || _n == LEN(_events)) {
            DEFER(_n = 0);
            return do_kevent(_events, _n, nullptr, 0, &tm00);
        }
        return 0;
    }

    int wait_for_fd(int fd, uint32_t interests, Timeout timeout) override {
        if (unlikely(interests == 0)) {
            errno = ENOSYS;
            return -1;
        }
        auto current = CURRENT;
        short ev = (interests == EVENT_READ) ? EVFILT_READ : EVFILT_WRITE;
        int ret = enqueue(fd, ev, EV_ADD | EV_ONESHOT, 0, current);
        if (ret < 0) return ret;
        SCOPED_PAUSE_WORK_STEALING;
        ret = thread_usleep(timeout);
        ERRNO err;
        if (likely(ret == -1 && err.no == EOK)) {
            return 0;  // event arrived
        }

        enqueue(fd, ev, EV_DELETE, 0, current, true);
        errno = (ret == 0) ? ETIMEDOUT : err.no;
        return -1;
    }

    ssize_t wait_and_fire_events(uint64_t timeout) override {
        ssize_t nev = 0;
        struct timespec tm;
        tm.tv_sec = timeout / 1000 / 1000;
        tm.tv_nsec = (timeout % (1000 * 1000)) * 1000;

    again:
        int ret = do_kevent(_events, _n, _events, LEN(_events), &tm);
        if (ret < 0) return ret;

        _n = 0;
        nev += ret;
        for (int i = 0; i < ret; ++i) {
            if (_events[i].filter == EVFILT_USER) continue;
            auto th = (thread*)_events[i].udata;
            if (th) thread_interrupt(th, EOK);
        }
        if (ret == (int)LEN(_events)) {  // there may be more events
            tm.tv_sec = tm.tv_nsec = 0;
            goto again;
        }
        return nev;
    }

    int cancel_wait() override {
        // cannot call `enqueue` directly since it will be called from another vCPU.
        // directly use kqueue to submit event, which is safe.
        // as same as `enqueue(_kq, EVFILT_USER, EV_ONESHOT, NOTE_TRIGGER, nullptr, true)`
        struct kevent entry;
        EV_SET(&entry, _kq, EVFILT_USER, EV_ONESHOT, NOTE_TRIGGER, 0, nullptr);
        return do_kevent(&entry, 1, nullptr, 0, &tm00);
    }

    // This vector is used to filter invalid add/rm_interest requests which may affect kevent's
    // functionality.
    std::vector<InFlightEvent> _inflight_events;
    int add_interest(Event e) override {
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
        entry.interests |= e.interests;
        if (e.interests & EVENT_READ) entry.reader_data = e.data;
        if (e.interests & EVENT_WRITE) entry.writer_data = e.data;
        if (e.interests & EVENT_ERROR) entry.error_data = e.data;
        auto events = evmap.translate_bitwisely(e.interests);
        return enqueue(e.fd, events, EV_ADD, 0, e.data, true);
    }

    int rm_interest(Event e) override {
        if (e.fd < 0 || (size_t)e.fd >= _inflight_events.size())
            LOG_ERROR_RETURN(EINVAL, -1, "invalid file descriptor ", e.fd);
        auto& entry = _inflight_events[e.fd];
        auto intersection = e.interests & entry.interests &
                            (EVENT_READ | EVENT_WRITE | EVENT_ERROR);
        if (intersection == 0) return 0;
        entry.interests ^= intersection;
        if (e.interests & EVENT_READ) entry.reader_data = nullptr;
        if (e.interests & EVENT_WRITE) entry.writer_data = nullptr;
        if (e.interests & EVENT_ERROR) entry.error_data = nullptr;
        auto events = evmap.translate_bitwisely(intersection);
        return enqueue(e.fd, events, EV_DELETE, 0, e.data, true);
    }

    ssize_t wait_for_events(void** data,
            size_t count, Timeout timeout) override {
        int ret = ::photon::wait_for_fd_readable(_kq, timeout);
        if (ret < 0) return errno == ETIMEDOUT ? 0 : -1;
        if (count > LEN(_events))
            count = LEN(_events);
        ret = do_kevent(_events, _n, _events, count, &tm00);
        if (ret < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to call kevent()");

        _n = 0;
        assert(ret <= (int) count);
        for (int i = 0; i < ret; ++i) {
            data[i] = _events[i].udata;
        }
        return ret;
    }
};

__attribute__((noinline))
static KQueue* new_kqueue_engine(bool is_master) {
    LOG_INFO("Init event engine: kqueue ", VALUE(is_master));
    return NewObj<KQueue>()->init();
}

MasterEventEngine* new_kqueue_master_engine() {
    return new_kqueue_engine(true);
}

CascadingEventEngine* new_kqueue_cascading_engine() {
    return new_kqueue_engine(false);
}


} // namespace photon
