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
#include <photon/common/alog.h>
#include <photon/common/timeout.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <vector>

#include "reset_handle.h"

namespace photon {
#ifndef EPOLLRDHUP
#define EPOLLRDHUP 0
#endif

class EventEngineEPollNG : public MasterEventEngine,
                           public CascadingEventEngine,
                           public ResetHandle {
public:
    static int if_close_fd(int& fd) {
        if (fd < 0) return 0;
        DEFER(fd = -1);
        return close(fd);
    }

    struct Poller {
        epoll_event events[16];
        int epfd = -1;
        int remains = 0;

        int init() {
            if (epfd >= 0) return -EEXIST;
            epfd = epoll_create1(EPOLL_CLOEXEC);
            if (epfd < 0) LOG_ERRNO_RETURN(0, -1, "failed to epoll_create1");
            return 0;
        }

        void fini() { if_close_fd(epfd); }

        int ctl(int fd, int op, uint32_t events, epoll_data_t data) {
            struct epoll_event ev;
            ev.events = events;  // EPOLLERR | EPOLLHUP always included
            ev.data = data;
            int ret = epoll_ctl(epfd, op, fd, &ev);
            if (ret < 0) {
                ERRNO err;
                auto events = HEX(ev.events);
                auto data = ev.data.ptr;
                LOG_WARN("failed to call epoll_ctl(`, `, `, {`, `})",
                         VALUE(epfd), VALUE(op), VALUE(fd), VALUE(events),
                         VALUE(data), err);
                return -err.no;
            }
            return ret;
        }

        int add(int fd, uint32_t events, epoll_data_t data) {
            return ctl(fd, EPOLL_CTL_ADD, events, data);
        }

        int rm(int fd, uint32_t events, epoll_data_t data) {
            return ctl(fd, EPOLL_CTL_DEL, events, data);
        }

        template <typename DataCB, typename FDCB>
        int notify_one(const DataCB& datacb, const FDCB& fdcb) {
            if (remains && fdcb()) {
                datacb(events[--remains].data);
                return 1;
            }
            return 0;
        }

        template <typename DataCB, typename FDCB>
        int notify_all(const DataCB& datacb, const FDCB& fdcb) {
            int fired = 0;
            while (remains && fdcb()) {
                fired += notify_one(datacb, fdcb);
            }
            return fired;
        }

        void reap(uint64_t timeout) {
            uint8_t cool_down_ms = 1;
            // since timeout may less than 1ms
            // in such condition, timeout_ms should be at least 1
            // or it may call epoll_wait without any idle
            timeout = (timeout && timeout < 1024) ? 1 : timeout / 1024;
            while (epfd > 0) {
                int ret = epoll_wait(epfd, events, LEN(events), timeout);
                if (ret < 0) {
                    ERRNO err;
                    if (err.no == EINTR) continue;
                    usleep(1024L * cool_down_ms);
                    timeout = sat_sub(timeout, cool_down_ms);
                    if (cool_down_ms < 16) {
                        cool_down_ms *= 2;
                        continue;
                    }
                    LOG_ERROR_RETURN(err.no, , "epoll_wait() failed ", err);
                }
                remains += ret;
                return;
            }
        }
    };

    enum class POLLERTYPE : int {
        ENGINE = 0,
        READER = 1,
        WRITER = 2,
        ERROR = 3,
        EVENT = 4,
    };

    Poller pl[4];

#define engine (pl[(int)POLLERTYPE::ENGINE])
#define rpoller (pl[(int)POLLERTYPE::READER])
#define wpoller (pl[(int)POLLERTYPE::WRITER])
#define epoller (pl[(int)POLLERTYPE::ERROR])
#define poller(x) (pl[(int)x])

    int evfd = -1;

    int init() {
        for (int i = 0; i < 4; i++) {
            if (poller(i).init() < 0) {
                LOG_ERROR("Failed to create sub poller `, because `", i,
                          ERRNO());
                goto errout;
            }
        }
        evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (evfd < 0) goto errout;
        for (int i = 1; i < 4; i++) {
            if (engine.add(poller(i).epfd, EPOLLIN, {.u64 = (uint64_t)i}) < 0)
                goto errout;
        }
        if (engine.add(evfd, EPOLLIN, {.u64 = (uint64_t)POLLERTYPE::EVENT}) < 0)
            goto errout;
        return 0;

    errout:
        for (int i = 3; i >= 0; i--) poller(i).fini();
        if_close_fd(evfd);
        return -1;
    }
    int fini() {
        LOG_INFO("Finish event engine: epoll-ng");
        for (int i = 3; i >= 0; i--) poller(i).fini();
        if_close_fd(evfd);
        return 0;
    }
    int reset() override {
        fini();
        return init();
    }
    virtual ~EventEngineEPollNG() override {
        fini();
    }

    virtual int add_interest(Event e) override {
        if (e.fd < 0)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid file descriptor ", e.fd);
        int ret = 0;
        int mod = (e.interests & ONE_SHOT) ? EPOLLONESHOT : 0;
        if (e.interests & EVENT_READ) {
            ret =
                rpoller.add(e.fd, mod | EPOLLIN | EPOLLRDHUP, {.ptr = e.data});
            if (ret < 0) return ret;
        }
        DEFER(if (ret < 0) rpoller.rm(e.fd, 0, {}));
        if (e.interests & EVENT_WRITE) {
            ret = wpoller.add(e.fd, mod | EPOLLOUT, {.ptr = e.data});
            if (ret < 0) return ret;
        }
        DEFER(if (ret < 0) wpoller.rm(e.fd, 0, {}));
        if (e.interests & EVENT_ERROR) {
            ret = epoller.add(e.fd, mod | EPOLLERR, {.ptr = e.data});
            if (ret < 0) return ret;
        }
        return ret;
    }
    virtual int rm_interest(Event e) override {
        if (e.fd < 0)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid file descriptor ", e.fd);
        int ret = 0;
        if (e.interests & EVENT_READ) {
            ret |= rpoller.rm(e.fd, 0, {});
        }
        if (e.interests & EVENT_WRITE) {
            ret |= wpoller.rm(e.fd, 0, {});
        }
        if (e.interests & EVENT_ERROR) {
            ret |= epoller.rm(e.fd, 0, {});
        }
        return ret;
    }

    template <typename DataCB, typename FDCB>
    void wait_for_events(uint64_t timeout, const DataCB& datacb,
                         const FDCB& fdcb) {
        int fired = 0;
        int turn;
        do {
            turn = rpoller.notify_one(datacb, fdcb) +
                   wpoller.notify_one(datacb, fdcb) +
                   epoller.notify_one(datacb, fdcb);
            fired += turn;
        } while (turn);
        if (!fired) {
            // no events ready
            eventfd_t value;
            engine.reap(timeout);
            engine.notify_all(
                [&](epoll_data_t data) __INLINE__ {
                    switch (data.u64) {
                        case (uint64_t)POLLERTYPE::READER:
                        case (uint64_t)POLLERTYPE::WRITER:
                        case (uint64_t)POLLERTYPE::ERROR:
                            poller(data.u64).reap(0);
                            return;
                        case (uint64_t)POLLERTYPE::EVENT:
                            eventfd_read(evfd, &value);
                            return;
                        default:
                            LOG_ERROR_RETURN(EINVAL, ,
                                             "Catch unknown event by engine ",
                                             data.u64);
                    }
                },
                [&]() __INLINE__ { return true; });
        }
    }
    virtual ssize_t wait_for_events(void** data, size_t count,
                                    uint64_t timeout = -1) override {
        int ret = get_vcpu()->master_event_engine->wait_for_fd_readable(
            engine.epfd, timeout);
        if (ret < 0) {
            return errno == ETIMEDOUT ? 0 : -1;
        }
        auto ptr = data;
        auto end = data + count;
        wait_for_events(
            0, [&](epoll_data_t data) __INLINE__ { *ptr++ = data.ptr; },
            [&]()
                __INLINE__ {  // make sure each fd receives all possible events
                    return end > ptr;
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
            [&](epoll_data_t data) __INLINE__ {
                assert(data.ptr);
                auto waiter = (Event*)data.ptr;
                rm_interest(*waiter);
                thread_interrupt((thread*)waiter->data, EOK);
                n++;
            },
            [&]() __INLINE__ { return true; });
        return n;
    }
    virtual int cancel_wait() override { return eventfd_write(evfd, 1); }

    int wait_for_fd(int fd, uint32_t interests, uint64_t timeout) override {
        Event waiter{fd, interests | ONE_SHOT, CURRENT};
        Event event{fd, interests | ONE_SHOT, &waiter};
        int ret = add_interest(event);
        if (ret < 0) LOG_ERROR_RETURN(0, -1, "failed to add event interest");
        ret = thread_usleep(timeout);
        ERRNO err;
        if (ret == -1 && err.no == EOK) {
            return 0;  // Event arrived
        }
        rm_interest(event);
        if (ret == 0) {
            errno = ETIMEDOUT;
            return -1;
        } else {
            errno = err.no;
            return -1;
        }
    }
};

__attribute__((noinline)) static EventEngineEPollNG* new_epoll_ng_engine() {
    LOG_INFO("Init event engine: epoll-ng");
    return NewObj<EventEngineEPollNG>()->init();
}

MasterEventEngine* new_epoll_ng_master_engine() {
    return new_epoll_ng_engine();
}

CascadingEventEngine* new_epoll_ng_cascading_engine() {
    return new_epoll_ng_engine();
}

}  // namespace photon
