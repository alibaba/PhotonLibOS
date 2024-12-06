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

#include "fstack-dpdk.h"

#include <unistd.h>
#include <sys/ioctl.h>
#include <vector>

#include <ff_api.h>

#include "fd-events.h"
#include "events_map.h"
#include "../thread/thread11.h"
#include "../common/alog.h"
#include "../net/basic_socket.h"
#include "reset_handle.h"

#ifndef EVFILT_EXCEPT
#define EVFILT_EXCEPT (-15)
#endif

namespace photon {

constexpr static EventsMap<EVUnderlay<EVFILT_READ, EVFILT_WRITE, EVFILT_EXCEPT>>
    evmap;

class FstackDpdkEngine : public MasterEventEngine, public CascadingEventEngine, public ResetHandle {
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
    struct timespec _tm = {0, 0};  // used for poll

    int init() {
        if (_kq >= 0)
            LOG_ERROR_RETURN(EALREADY, -1, "already init-ed");

        static char* argv[] = {
                (char*) "proc-name-not-used",
                (char*) "--conf=/etc/f-stack.conf",
                (char*) "--proc-type=primary",
                (char*) "--proc-id=0",
        };
        // f-stack will exit the program if init failed.
        // Unable to change the behavior unless it provides more flexible APIs ...
        if (ff_init(LEN(argv), argv))
            return -1;

        _kq = ff_kqueue();
        if (_kq < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to create kqueue()");

        if (enqueue(_kq, EVFILT_USER, EV_ADD | EV_CLEAR, 0, nullptr, true) < 0) {
            DEFER({ close(_kq); _kq = -1; });
            LOG_ERRNO_RETURN(0, -1, "failed to setup self-wakeup EVFILT_USER event by kevent()");
        }
        return 0;
    }

    int reset() override {
        assert(false);
    }

    ~FstackDpdkEngine() override {
        LOG_INFO("Finish f-stack dpdk engine");
        // if (_n > 0) LOG_INFO(VALUE(_events[0].ident), VALUE(_events[0].filter), VALUE(_events[0].flags));
        // assert(_n == 0);
        if (_kq >= 0)
            close(_kq);
    }

    int enqueue(int fd, short event, uint16_t action, uint32_t event_flags, void* udata, bool immediate = false) {
        // LOG_INFO("enqueue _kq: `, fd: `, event: `, action: `", _kq, fd, event, action);
        assert(_n < LEN(_events));
        auto entry = &_events[_n++];
        EV_SET(entry, fd, event, action, event_flags, 0, udata);
        if (immediate || _n == LEN(_events)) {
            int ret = ff_kevent(_kq, _events, _n, nullptr, 0, nullptr);
            if (ret < 0)
                LOG_ERRNO_RETURN(0, -1, "failed to submit events with kevent()");
            _n = 0;
        }
        return 0;
    }

    int wait_for_fd(int fd, uint32_t interests, uint64_t timeout) override {
        short ev = (interests == EVENT_READ) ? EVFILT_READ : EVFILT_WRITE;
        enqueue(fd, ev, EV_ADD | EV_ONESHOT, 0, CURRENT);
        int ret = thread_usleep(timeout);
        ERRNO err;
        if (ret == -1 && err.no == EOK) {
            return 0;  // event arrived
        }

        // enqueue(fd, ev, EV_DELETE, 0, CURRENT, true); // immediately
        errno = (ret == 0) ? ETIMEDOUT : err.no;
        return -1;
    }

    ssize_t wait_and_fire_events(uint64_t timeout = -1) override {
        ssize_t nev = 0;
        struct timespec tm;
        tm.tv_sec = timeout / 1000 / 1000;
        tm.tv_nsec = (timeout % (1000 * 1000)) * 1000;

    again:
        int ret = ff_kevent(_kq, _events, _n, _events, LEN(_events), &tm);
        if (ret < 0)
            LOG_ERRNO_RETURN(0, -1, "failed to call kevent()");

        _n = 0;
        nev += ret;
        for (int i = 0; i < ret; ++i) {
            if (_events[i].filter == EVFILT_USER) continue;
            auto th = (thread*) _events[i].udata;
            if (th) thread_interrupt(th, EOK);
        }
        if (ret == (int) LEN(_events)) {  // there may be more events
            tm.tv_sec = tm.tv_nsec = 0;
            goto again;
        }
        return nev;
    }

    int cancel_wait() override {
        enqueue(_kq, EVFILT_USER, EV_ONESHOT, NOTE_TRIGGER, nullptr, true);
        return 0;
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
            size_t count, uint64_t timeout = -1) override {
        int ret = get_vcpu()->master_event_engine->wait_for_fd_readable(_kq, timeout);
        if (ret < 0) return errno == ETIMEDOUT ? 0 : -1;
        if (count > LEN(_events))
            count = LEN(_events);
        ret = ff_kevent(_kq, _events, _n, _events, count, &_tm);
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

static thread_local MasterEventEngine* g_engine = nullptr;

static int poll_engine(void* arg) {
    assert(g_engine != nullptr);
    g_engine->wait_and_fire_events(0);
    thread_yield();
    return 0;
}

int fstack_dpdk_init() {
    LOG_INFO("Init f-stack dpdk engine");
    g_engine = NewObj<FstackDpdkEngine>()->init();
    if (g_engine == nullptr)
        return -1;
    photon::thread_create11(ff_run, poll_engine, nullptr);
    return 0;
}

int fstack_dpdk_fini() {
    return 0;   // TODO
}

int fstack_socket(int domain, int type, int protocol) {
    int fd = ff_socket(domain, type, protocol);
    if (fd < 0)
        return fd;
    int val = 1;
    int ret = ff_ioctl(fd, FIONBIO, &val);
    if (ret != 0)
        return -1;
    return fd;
}

// linux_sockaddr is required by f-stack api, and has the same layout to sockaddr
static_assert(sizeof(linux_sockaddr) == sizeof(sockaddr));

int fstack_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout) {
    int err = 0;
    while (true) {
        int ret = ff_connect(sockfd, (linux_sockaddr*) addr, addrlen);
        if (ret < 0) {
            auto e = errno;
            if (e == EINTR) {
                err = 1;
                continue;
            }
            if (e == EINPROGRESS || (e == EADDRINUSE && err == 1)) {
                ret = g_engine->wait_for_fd_writable(sockfd, timeout);
                if (ret < 0) return -1;
                socklen_t n = sizeof(err);
                ret = ff_getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &n);
                if (ret < 0) return -1;
                if (err) {
                    errno = err;
                    return -1;
                }
                return 0;
            }
        }
        return ret;
    }
}

int fstack_listen(int sockfd, int backlog) {
    return ff_listen(sockfd, backlog);
}

int fstack_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    return ff_bind(sockfd, (linux_sockaddr*) addr, addrlen);
}

int fstack_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen, uint64_t timeout) {
    return net::doio(LAMBDA(ff_accept(sockfd, (linux_sockaddr*) addr, addrlen)),
                     LAMBDA_TIMEOUT(g_engine->wait_for_fd_readable(sockfd, timeout)));
}

int fstack_close(int fd) {
    return ff_close(fd);
}

int fstack_shutdown(int sockfd, int how) {
    return ff_shutdown(sockfd, how);
}

ssize_t fstack_send(int sockfd, const void* buf, size_t count, int flags, uint64_t timeout) {
    return net::doio(LAMBDA(ff_send(sockfd, buf, count, flags)),
                     LAMBDA_TIMEOUT(g_engine->wait_for_fd_writable(sockfd, timeout)));
}

ssize_t fstack_sendmsg(int sockfd, const struct msghdr* message, int flags, uint64_t timeout) {
    return net::doio(LAMBDA(ff_sendmsg(sockfd, message, flags)),
                     LAMBDA_TIMEOUT(g_engine->wait_for_fd_writable(sockfd, timeout)));
}

ssize_t fstack_recv(int sockfd, void* buf, size_t count, int flags, uint64_t timeout) {
    return net::doio(LAMBDA(ff_recv(sockfd, buf, count, flags)),
                     LAMBDA_TIMEOUT(g_engine->wait_for_fd_readable(sockfd, timeout)));
}

ssize_t fstack_recvmsg(int sockfd, struct msghdr* message, int flags, uint64_t timeout) {
    return net::doio(LAMBDA(ff_recvmsg(sockfd, message, flags)),
                     LAMBDA_TIMEOUT(g_engine->wait_for_fd_readable(sockfd, timeout)));
}

int fstack_setsockopt(int socket, int level, int option_name, const void* option_value, socklen_t option_len) {
    return ff_setsockopt(socket, level, option_name, option_value, option_len);
}

int fstack_getsockopt(int socket, int level, int option_name, void* option_value, socklen_t* option_len) {
    return ff_getsockopt(socket, level, option_name, option_value, option_len);
}

} // namespace photon
