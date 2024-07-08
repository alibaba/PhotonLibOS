//
// Created by jun on 2024/7/4.
//

#include "photon/hook/hook.h"
#include "photon/hook/ZySokect.h"
#include <iostream>
#include <dlfcn.h>
#include <photon/photon.h>
#include <photon/net/basic_socket.h>

#include "photon/thread/std-compat.h"


#define HOOK_SYS_FUNC(name) name##_fun_ptr_t g_sys_##name##_fun = (name##_fun_ptr_t)dlsym(RTLD_NEXT, #name);
HOOK_SYS_FUNC(sleep);
HOOK_SYS_FUNC(connect);
HOOK_SYS_FUNC(accept);
HOOK_SYS_FUNC(read);
HOOK_SYS_FUNC(write);





namespace ZyIo::Hook{
    static bool G_HOOK = false;
    static ZyIo::ZySokectHook* G_HOOK_SOCKET_INS = nullptr;
    void initHook() {
        G_HOOK = true;
        G_HOOK_SOCKET_INS = new ZyIo::ZySokectHook();
        G_HOOK_SOCKET_INS->start();
    }

    unsigned int sleep_hook(unsigned int seconds){
        std::cout << "sleep_hook share lib! " << std::endl;
        return g_sys_sleep_fun(seconds);
    }

    int accept_hook(int sockfd, struct sockaddr *addr, socklen_t *addrlen){
        //submit accept
        __s32 r=-100;
        __s32* res =&r;

        photon_std::thread::id threadId = photon_std::this_thread::get_id();
        photon::thread_interrupt(threadId);

        G_HOOK_SOCKET_INS->submitAccept(0,res,sockfd,addr,addrlen);
        //yield()

        //wait notify
        return *res;
    }

    int connect_hook(int sockfd, const struct sockaddr *addr, socklen_t addrlen){
        //submit connect
        __s32 r=-100;
        __s32* res =&r;
        G_HOOK_SOCKET_INS->submitConnect(0,res,sockfd,addr,addrlen);
        //yield()

        //wait notify
        return *res;
    }

    ssize_t read_hook(int fd, void *buf, size_t count){
        //submit read
        __s32 r=-100;
        __s32* res =&r;
        G_HOOK_SOCKET_INS->submitRead(0,res,fd,buf,count);
        //yield()

        //wait notify
        return *res;
    }

    ssize_t write_hook(int fd, const void *buf, size_t count){
        //submit write
        __s32 r=-100;
        __s32* res =&r;
        G_HOOK_SOCKET_INS->submitWrite(0,res,fd,buf,count);
        //yield()

        //wait notify
        return *res;
    }
}




extern "C" {

unsigned int sleep(unsigned int seconds){
    if (!ZyIo::Hook::G_HOOK) {
        return g_sys_sleep_fun(seconds);
    } else {
        return ZyIo::Hook::sleep_hook(seconds);
    }
}


int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (!ZyIo::Hook::G_HOOK) {
        return g_sys_accept_fun(sockfd, addr, addrlen);
    } else {
        return ZyIo::Hook::accept_hook(sockfd, addr, addrlen);
    }
}


int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!ZyIo::Hook::G_HOOK) {
        return g_sys_connect_fun(sockfd, addr, addrlen);
    } else {
        return ZyIo::Hook::connect_hook(sockfd, addr, addrlen);
    }
}

ssize_t read(int fd, void *buf, size_t count) {
    if (!ZyIo::Hook::G_HOOK) {
        return g_sys_read_fun(fd, buf, count);
    } else {
        return ZyIo::Hook::read_hook(fd, buf, count);
    }
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (!ZyIo::Hook::G_HOOK) {
        return g_sys_write_fun(fd, buf, count);
    } else {
        return ZyIo::Hook::write_hook(fd, buf, count);
    }
}






}