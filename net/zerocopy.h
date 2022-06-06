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

#include <cstdint>
#include <photon/common/ring.h>
#include <photon/thread/thread.h>

namespace photon {
namespace net {

class ZerocopyEventEntry {
public:
    explicit ZerocopyEventEntry(int fd);

    ~ZerocopyEventEntry();

    // counter is a scalar value that equals to (number of sendmsg calls - 1),
    // it wraps after UINT_MAX.
    int zerocopy_wait(uint32_t counter, uint64_t timeout);

    // Read counter from epoll error msg, and wake up the corresponding threads
    void handle_events();

    int get_fd() const {
        return m_fd;
    }

protected:
    struct Entry {
        uint32_t counter;
        photon::thread* th;
    };
    int m_fd;
    RingQueue<Entry> m_queue;
};

int zerocopy_init();

int zerocopy_fini();

/* Check if kernel version satisfies and thus zerocopy feature should be enabled */
bool zerocopy_available();

}
}
