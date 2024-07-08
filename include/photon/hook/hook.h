//
// Created by jun on 2024/7/4.
//

#ifndef URING_SOCKET_V3_HOOK_H
#define URING_SOCKET_V3_HOOK_H


#include <csignal>
#include <bits/socket.h>
#include <sys/types.h>

typedef int (*sleep_fun_ptr_t)(unsigned int seconds);

typedef int (*accept_fun_ptr_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

typedef int (*connect_fun_ptr_t)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

typedef ssize_t (*read_fun_ptr_t)(int fd, void *buf, size_t count);

typedef ssize_t (*write_fun_ptr_t)(int fd, const void *buf, size_t count);






namespace ZyIo{

    namespace Hook
    {
        void initHook();

        unsigned int sleep_hook(unsigned int seconds);

        int accept_hook(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

        int connect_hook(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

        ssize_t read_hook(int fd, void *buf, size_t count);

        ssize_t write_hook(int fd, const void *buf, size_t count);
    }






}

extern "C" {


unsigned int sleep(unsigned int seconds);

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

ssize_t read(int fd, void *buf, size_t count);

ssize_t write(int fd, const void *buf, size_t count);



}

#endif //URING_SOCKET_V3_HOOK_H