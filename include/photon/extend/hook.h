//
// Created by jun on 2024/7/25.
//

#ifndef PHOTON_EXTEND_HOOK_H
#define PHOTON_EXTEND_HOOK_H



#include <csignal>
#include <sys/socket.h>
#include <sys/types.h>
#include "comm-def.h"
#include "photon/thread/std-compat.h"
#include "photon/photon.h"


typedef int (*sleep_fun_ptr_t)(unsigned int seconds);

typedef int (*usleep_fun_ptr_t)(__useconds_t useconds);

typedef int (*accept_fun_ptr_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

typedef int (*connect_fun_ptr_t)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

typedef ssize_t (*read_fun_ptr_t)(int fd, void *buf, size_t count);

typedef ssize_t (*write_fun_ptr_t)(int fd, const void *buf, size_t count);

typedef ssize_t (*readv_fun_ptr_t)(int fd, const struct iovec *iov, int iovcnt);

typedef ssize_t (*writev_fun_ptr_t)(int fd, const struct iovec *iov, int iovcnt);

typedef ssize_t (*recv_fun_ptr_t) (int fd, void *buf, size_t n, int flags);

typedef ssize_t (*send_fun_ptr_t) (int fd, const void *buf, size_t n, int flags);

typedef ssize_t (*recvmsg_fun_ptr_t)(int fd, struct msghdr * msg, int flag);

typedef ssize_t (*sendmsg_fun_ptr_t) (int fd, const struct msghdr *msg,int flag);


namespace zyio{
    namespace hook{
        class HookFlag{
        private:
            CLASS_FAST_PROPERTY_GETTER(bool , g_hook, Ghook)
            CLASS_FAST_PROPERTY_GETTER(bool , g_hook_debug, GhookDebug)
            static HookFlag* INS;
        public:
            HookFlag() = delete;
            HookFlag(bool gHook,bool gHookDebug);
            static void init(bool hookFlag,bool isDebug);
        };

        bool isHook();
        bool isHookDebug();
    }
}

extern "C" {


unsigned int sleep(unsigned int seconds);

int usleep(__useconds_t useconds);

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

ssize_t read(int fd, void *buf, size_t count);

ssize_t write(int fd, const void *buf, size_t count);

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);

ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

ssize_t recv (int fd, void *buf, size_t n, int flags);

ssize_t send (int fd, const void *buf, size_t n, int flags);

ssize_t recvmsg(int fd, struct msghdr * msg, int flag);

ssize_t sendmsg (int fd, const struct msghdr *msg,int flag);

}

#endif //PHOTON_EXTEND_HOOK_H
