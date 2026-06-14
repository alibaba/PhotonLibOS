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
#ifdef _WIN32
// poll.h stub for Windows (use select/WSAWaitForMultipleEvents instead)
#include <winsock2.h>
#ifndef POLLIN
struct pollfd {
    int fd;
    short events;
    short revents;
};
#define POLLIN  0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#endif
#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif
inline int poll(struct pollfd* fds, unsigned long nfds, int timeout) {
    (void)fds; (void)nfds; (void)timeout;
    return 0;
}
#else
#include_next <poll.h>
#endif
