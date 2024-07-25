//
// Created by jun on 2024/7/25.
//
#include "photon/extend//hook.h"
#include <dlfcn.h>
#include <iostream>
#include "photon/io/iouring-wrapper.h"


#define HOOK_SYS_FUNC(name) name##_fun_ptr_t g_sys_##name##_fun = (name##_fun_ptr_t)dlsym(RTLD_NEXT, #name);
HOOK_SYS_FUNC(sleep);
HOOK_SYS_FUNC(usleep);
HOOK_SYS_FUNC(connect);
HOOK_SYS_FUNC(accept);
HOOK_SYS_FUNC(read);
HOOK_SYS_FUNC(write);
HOOK_SYS_FUNC(readv);
HOOK_SYS_FUNC(writev);
HOOK_SYS_FUNC(recv);
HOOK_SYS_FUNC(send);
HOOK_SYS_FUNC(recvmsg);
HOOK_SYS_FUNC(sendmsg);

namespace zyio{
    namespace hook{
        HookFlag* HookFlag::INS = nullptr;
        HookFlag::HookFlag(bool gHook, bool gHookDebug) : g_hook(gHook),g_hook_debug(gHookDebug){

        }
        void HookFlag::init(bool hookFlag, bool isDebug) {
            #ifdef __linux__
            auto th = photon_std::this_thread::get_id();
            if (th == nullptr) {
                fprintf(stderr, "please init hook in fiber\n");
                return;
            }
            HookFlag::INS = new HookFlag(hookFlag,isDebug);
            printf("enable block socket api hook\n");
            #else
            fprintf(stderr, "not support hook for this system\n");
            #endif
        }

        bool isHook(){
            if(HookFlag::INS == nullptr){
                return false;
            }
            return HookFlag::INS->getGhook();
        }

        bool isHookDebug(){
            if(HookFlag::INS == nullptr){
                return false;
            }
            return HookFlag::INS->getGhookDebug();
        }

        bool canHook(){
            auto th = photon_std::this_thread::get_id();
            return isHook() && th != nullptr;
        }
    }
}




extern "C" {

unsigned int sleep(unsigned int seconds) {
    if (zyio::hook::canHook()) {
        if (zyio::hook::isHookDebug()) {
            printf("call hook sleep api,sleep:%d s\n", seconds);
        }
        return photon::thread_sleep(seconds);
    }
    if (zyio::hook::isHookDebug()) {
        printf("call lib c sleep api,sleep:%d s\n", seconds);
    }
    return g_sys_sleep_fun(seconds);
}

int usleep(__useconds_t useconds){
    if (zyio::hook::canHook()) {
        if (zyio::hook::isHookDebug()) {
            printf("call hook usleep api,sleep:%d s\n", useconds);
        }
        return photon::thread_usleep(useconds);
    }
    if (zyio::hook::isHookDebug()) {
        printf("call lib c usleep api,sleep:%d s\n", useconds);
    }
    return g_sys_usleep_fun(useconds);
}


int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (zyio::hook::canHook()) {
        if (zyio::hook::isHookDebug()) {
            printf("call hook accept api\n");
        }
        return photon::iouring_accept(sockfd, addr, addrlen);
    }
    if (zyio::hook::isHookDebug()) {
        printf("call lib c accept api\n");
    }
    return g_sys_accept_fun(sockfd, addr, addrlen);
}


int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (zyio::hook::canHook()) {
//        // 设置socket为非阻塞
//        int flags = fcntl(sockfd, F_GETFL, 0);
//        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
        if (zyio::hook::isHookDebug()) {
            printf("call hook connect api\n");
        }
        return photon::iouring_connect(sockfd, addr, addrlen);
    }

    if (zyio::hook::isHookDebug()) {
        printf("call lib c connect api\n");
    }
    return g_sys_connect_fun(sockfd, addr, addrlen);
}

ssize_t read(int fd, void *buf, size_t count) {
    if (zyio::hook::canHook()) {
        if (zyio::hook::isHookDebug()) {
            printf("call hook read api,data len:%zu\n", count);
            size_t len = photon::iouring_pread(fd, buf, count, 0);
            printf("expect data len:%zu,success read data len:%zu\n",count, len);
            return len;
        }
        return photon::iouring_pread(fd, buf, count, 0);
    }

    if (zyio::hook::isHookDebug()) {
            printf("call lib c read api\n");
            size_t len = g_sys_read_fun(fd, buf, count);
            printf("expect data len:%zu,success read data len:%zu\n", count, len);
            return len;
    }
    return g_sys_read_fun(fd, buf, count);
}


ssize_t write(int fd, const void *buf, size_t count) {
    if (zyio::hook::canHook()) {
        if (zyio::hook::isHookDebug()) {
            printf("call hook write api,data len:%zu\n", count);
            size_t res = photon::iouring_pwrite(fd, buf, count, 0);
            printf("call hook write read api,write data len:%zu,write res:%zu\n", count, res);
            return res;
        }
        return photon::iouring_pwrite(fd, buf, count, 0);
    }
    if (zyio::hook::isHookDebug()) {
        printf("call lib c write api,write data len:%zu\n", count);
    }
    return g_sys_write_fun(fd, buf, count);
}


ssize_t readv(int fd, const struct iovec *iov, int iovcnt){
    if (zyio::hook::canHook()) {
        return photon::iouring_preadv(fd, iov, iovcnt,0);
    }
    return g_sys_readv_fun(fd, iov, iovcnt);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt){
    if (zyio::hook::canHook()) {
        return photon::iouring_pwritev(fd, iov, iovcnt,0);
    }
    return g_sys_writev_fun(fd, iov, iovcnt);
}

ssize_t recv (int fd, void *buf, size_t n, int flags){
    if (zyio::hook::canHook()) {
        return photon::iouring_recv(fd, buf, n,flags);
    }
    return g_sys_recv_fun(fd, buf, n,flags);
}

ssize_t send (int fd, const void *buf, size_t n, int flags){
    if (zyio::hook::canHook()) {
        return photon::iouring_send(fd, buf, n,flags);
    }
    return g_sys_send_fun(fd, buf, n,flags);
}

ssize_t recvmsg(int fd, struct msghdr * msg, int flag){
    if (zyio::hook::canHook()) {
        return photon::iouring_recvmsg(fd, msg, flag);
    }
    return g_sys_recvmsg_fun(fd, msg, flag);
}

ssize_t sendmsg (int fd, const struct msghdr *msg,int flag){
    if (zyio::hook::canHook()) {
        return photon::iouring_sendmsg(fd, msg, flag);
    }
    return g_sys_sendmsg_fun(fd, msg, flag);
}

}