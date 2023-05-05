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

#include "fstack_weak.h"

#include <cstdio>
#include <cerrno>

static const char* err_msg = "You are calling a weak implementation. Please link the fstack_dpdk library.\n";

#define RETURN_ERROR(code) do {         \
    fprintf(stderr, "%s", err_msg);   \
    errno = ENXIO;                      \
    return code;                        \
} while (0)

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((weak))
int ff_init(int argc, char* const argv[]) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
void ff_run(ff_run_func_t loop, void* arg) {
    RETURN_ERROR();
}

__attribute__((weak))
int ff_socket(int domain, int type, int protocol) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_connect(int s, const struct linux_sockaddr* name, socklen_t namelen) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_bind(int s, const struct linux_sockaddr* addr, socklen_t addrlen) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_listen(int s, int backlog) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_accept(int s, struct linux_sockaddr* addr, socklen_t* addrlen) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_shutdown(int s, int how) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_close(int fd) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
ssize_t ff_send(int s, const void* buf, size_t len, int flags) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
ssize_t ff_recv(int s, void* buf, size_t len, int flags) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
ssize_t ff_recvmsg(int s, struct msghdr* msg, int flags) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
ssize_t ff_sendmsg(int s, const struct msghdr* msg, int flags) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_ioctl(int fd, unsigned long request, ...) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_setsockopt(int s, int level, int optname, const void* optval, socklen_t optlen) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_getsockopt(int s, int level, int optname, void* optval, socklen_t* optlen) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_kqueue(void) {
    RETURN_ERROR(-1);
}

__attribute__((weak))
int ff_kevent(int kq, const struct kevent* changelist, int nchanges, struct kevent* eventlist, int nevents,
              const struct timespec* timeout) {
    RETURN_ERROR(-1);
}

#ifdef __cplusplus
}
#endif