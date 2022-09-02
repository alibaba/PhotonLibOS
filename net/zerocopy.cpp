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

#include "zerocopy.h"
#include <linux/errqueue.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <photon/common/event-loop.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/common/alog.h>
#include "socket.h"

#ifndef SO_EE_ORIGIN_ZEROCOPY
#define SO_EE_ORIGIN_ZEROCOPY		5
#endif

#ifndef SO_EE_CODE_ZEROCOPY_COPIED
#define SO_EE_CODE_ZEROCOPY_COPIED	1
#endif

using namespace std;

namespace photon {
namespace net {

class ErrQueueEventLoop {
public:
    static constexpr size_t MAX_POLL_SIZE = 16;
    static constexpr size_t MAX_WAIT_TIME = 10UL * 1000 * 1000;
    EventLoop * loop;
    photon::CascadingEventEngine* poller;
    ErrQueueEventLoop()
        : loop(new_event_loop({this, &ErrQueueEventLoop::wait_event},
                              {this, &ErrQueueEventLoop::on_event})),
          poller(photon::new_epoll_cascading_engine()) {
        loop->async_run();
    }

    ~ErrQueueEventLoop() {
        loop->stop();
        delete loop;
        delete poller;
    }

    void* event_entries[MAX_POLL_SIZE];
    int n;

    int wait_event(EventLoop*) {
        n = poller->wait_for_events(event_entries, MAX_POLL_SIZE, MAX_WAIT_TIME);
        return n;
    }

    int on_event(EventLoop*) {
        for (int i = 0; i < n; i++) {
            auto entry = (ZerocopyEventEntry*) event_entries[i];
            entry->handle_events();
        }
        return 0;
    }

    int register_entry(ZerocopyEventEntry* entry) {
        return poller->add_interest({.fd = entry->get_fd(), .interests = photon::EVENT_ERROR, .data = entry});
    }

    int deregister_entry(int fd) {
        return poller->rm_interest({.fd = fd, .interests = photon::EVENT_ERROR, .data = nullptr});
    }

};

thread_local static ErrQueueEventLoop * g_eqloop;

int zerocopy_init() {
    if (!zerocopy_available()) {
        return -1;
    }
    g_eqloop = new ErrQueueEventLoop();
    if (!g_eqloop)
        LOG_ERROR_RETURN(0, -1, "Failed to create poller");
    return 0;
}

int zerocopy_fini() {
    delete g_eqloop;
    g_eqloop = nullptr;
    return 0;
}

inline bool is_counter_wrapped(uint32_t old_value, uint32_t new_value) {
    const uint32_t mid = UINT32_MAX / 2;
    return new_value - old_value > mid && old_value > mid;
}

static int receive_counter(int fd, uint32_t& counter) {
    uint32_t last_counter = 0;
    while (true) {
        char control[128] = {};
        msghdr msg = {};
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        int ret = recvmsg(fd, &msg, MSG_ERRQUEUE);
        if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (ret == -1) {
            LOG_ERRNO_RETURN(0, -1, "Fail to receive from MSG_ERRQUEUE")
        }
        cmsghdr* cm = CMSG_FIRSTHDR(&msg);
        if (cm == nullptr) {
            LOG_ERROR_RETURN(0, -1, "Fail to get control message header");
        }
        sock_extended_err* serr = (sock_extended_err*) CMSG_DATA(cm);
        if (serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
            LOG_ERROR_RETURN(0, -1, "wrong origin");
        }
        if (serr->ee_errno != 0) {
            LOG_ERROR_RETURN(0, -1, "wrong error code ", serr->ee_errno);
        }
        if ((serr->ee_code & SO_EE_CODE_ZEROCOPY_COPIED)) {
            LOG_DEBUG("deferred copy occurs, might harm performance");
        }

        uint32_t new_counter = serr->ee_data;
        if (new_counter > counter || is_counter_wrapped(last_counter, new_counter)) {
            counter = new_counter;
        }
        last_counter = new_counter;
    }
    return 0;
}

static bool do_zerocopy_available() {
    utsname buf = {};
    uname(&buf);
    string kernel_release(buf.release);
    string kernel_version = kernel_release.substr(0, kernel_release.find('-'));
    int result = 0;
    int ret = Utility::version_compare(kernel_version, "4.15", result);
    if (ret != 0) {
        LOG_ERROR("Unable to detect kernel version, not using zero-copy");
        return false;
    }
    return result >= 0;
}

bool zerocopy_available() {
    static bool r = do_zerocopy_available();
    return r;
}

ZerocopyEventEntry::ZerocopyEventEntry(int fd) : m_fd(fd), m_queue(1 << 16) {
    g_eqloop->register_entry(this);
}

ZerocopyEventEntry::~ZerocopyEventEntry() {
    g_eqloop->deregister_entry(m_fd);
}

int ZerocopyEventEntry::zerocopy_wait(uint32_t counter, uint64_t timeout) {
    m_queue.push_back(Entry{counter, photon::CURRENT});

    int ret = photon::thread_usleep(timeout);
    if (ret == 0) {
        // Timed out, wake up all sleeping threads
        while (!m_queue.empty()) {
            photon::thread_interrupt(m_queue.front().th);
            m_queue.pop_front();
        }
    }
    return ret;
}

void ZerocopyEventEntry::handle_events() {
    uint32_t counter = 0;
    int ret = receive_counter(m_fd, counter);
    if (ret != 0) {
        // Handle zerocopy errmsg failed, or it's probably another type of error
        return;
    }

    auto fire_front = [&]() {
        photon::thread* target_th = m_queue.front().th;
        m_queue.pop_front();
        photon::thread_interrupt(target_th);
    };

    if (!m_queue.empty()) {
        uint32_t back_counter = m_queue.back().counter;
        if (is_counter_wrapped(back_counter, counter)) {
            // The first wrapped counter can wake up all the other counters in queue
            while (!m_queue.empty()) {
                fire_front();
            }
            return;
        }
    }

    while (!m_queue.empty()) {
        uint32_t front_counter = m_queue.front().counter;
        if (front_counter <= counter) {
            // Wake up one by one, in ascending order
            fire_front();
        } else {
            // Terminate if not less
            break;
        }
    }
}

}
}
