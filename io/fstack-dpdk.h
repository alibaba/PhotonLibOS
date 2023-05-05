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

#include <sys/socket.h>
#include <cstdio>
#include <cstdint>

namespace photon {

int fstack_dpdk_init();

int fstack_dpdk_fini();

int fstack_socket(int domain, int type, int protocol);

int fstack_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout);

int fstack_listen(int sockfd, int backlog);

int fstack_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);

int fstack_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen, uint64_t timeout);

int fstack_close(int fd);

int fstack_shutdown(int sockfd, int how);

ssize_t fstack_send(int sockfd, const void* buf, size_t count, int flags, uint64_t timeout);

ssize_t fstack_sendmsg(int sockfd, const struct msghdr* message, int flags, uint64_t timeout);

ssize_t fstack_recv(int sockfd, void* buf, size_t count, int flags, uint64_t timeout);

ssize_t fstack_recvmsg(int sockfd, struct msghdr* message, int flags, uint64_t timeout);

int fstack_setsockopt(int socket, int level, int option_name, const void* option_value, socklen_t option_len);

int fstack_getsockopt(int socket, int level, int option_name, void* option_value, socklen_t* option_len);

}