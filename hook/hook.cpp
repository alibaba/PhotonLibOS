//
// Created by jun on 2024/7/4.
//

#include "photon/hook/hook.h"
#include "photon/common/alog.h"
#include <dlfcn.h>


#define HOOK_SYS_FUNC(name) name##_fun_ptr_t g_sys_##name##_fun = (name##_fun_ptr_t)dlsym(RTLD_NEXT, #name);
HOOK_SYS_FUNC(sleep);
HOOK_SYS_FUNC(connect);
HOOK_SYS_FUNC(accept);
HOOK_SYS_FUNC(read);
HOOK_SYS_FUNC(write);




namespace ZyIo{
    namespace Socket{

        DataCarrier::DataCarrier(ZyIo::Socket::DataFlag flag, photon::thread* tid, __s32 *res)
        :flag(flag),tid(tid),res(res) {

        }
        DataCarrier::~DataCarrier()= default;


        unsigned int ZySokect::DEFAULT_ENTITY_SIZE_S = 16;
        ZySokect::ZySokect() {
            auto pUring = new io_uring;
            auto const ret = io_uring_queue_init(DEFAULT_ENTITY_SIZE_S, pUring, 0); // 初始化
            if (ret < 0)
            {
                LOG_ERROR("io_uring_queue_init error: %d",ret);
                exit(EXIT_FAILURE);
            }
            this->ring = pUring;
        }
        ZySokect::~ZySokect() {
            delete ring;
        }

        void ZySokect::startWithFb() {
            new photon_std::thread(&ZyIo::Socket::ZySokect::start, this);
        }

        void ZySokect::start() {
            while (true)
            {
                //批量获取
                io_uring_cqe* cqes[64] = {};
                auto cqeCount = io_uring_peek_batch_cqe(ring, cqes, 64);
                if (cqeCount <= 0)
                {
                    continue;
                }
                for (unsigned int i = 0; i < cqeCount; i++)
                {
                    auto cqe = cqes[i];
                    auto dataCarrier = (DataCarrier*)cqe->user_data;
                    //return res
                    *(dataCarrier->getRes()) = cqe->res;
                    //resume thead
                    photon::thread_interrupt(dataCarrier->getTid());
                    delete dataCarrier;
                    io_uring_cqe_seen(ring, cqe);
                }
            }
        }

        io_uring_sqe *ZySokect::doTake() {
            return io_uring_get_sqe(ring);
        }

        void ZySokect::doSubmit(io_uring_sqe *sqe, ZyIo::Socket::DataCarrier *carrier) {
            io_uring_sqe_set_data(sqe, carrier);
            io_uring_submit(ring);
        }

        void ZySokect::submitConnect(photon::thread *th, __s32 *res, int sockfd, const struct sockaddr *addr,
                                     socklen_t addrlen) {
            auto carrier = new DataCarrier(CONNETC, th,res);
            auto sqe = doTake();
            io_uring_prep_connect(sqe, sockfd, addr, addrlen);
            doSubmit(sqe, carrier);
        }

        void ZySokect::submitAccept(photon::thread *th, __s32 *res, int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
            auto carrier = new DataCarrier(ACCEPT, th,res);
            auto sqe = doTake();
            io_uring_prep_accept(sqe, sockfd, addr, addrlen, 0);
            doSubmit(sqe, carrier);
        }

        void ZySokect::submitRead(photon::thread *th, __s32 *res, int fd, void *buf, size_t count) {
            auto carrier = new DataCarrier(READ, th,res);
            auto sqe = doTake();
            io_uring_prep_read(sqe, fd, buf, count, 0);
            doSubmit(sqe, carrier);
        }

        void ZySokect::submitWrite(photon::thread *th, __s32 *res, int fd, const void *buf, size_t count) {
            auto carrier = new DataCarrier(WRITE, th,res);
            auto sqe = doTake();
            io_uring_prep_write(sqe, fd, buf, count, 0);
            doSubmit(sqe, carrier);
        }



    }
    namespace Hook{
        static bool G_HOOK = false;
        static bool G_HOOK_IS_DEBUG = false;
        static ZyIo::Socket::ZySokect* G_HOOK_SOCKET_INS = nullptr;
        void initHook(bool isDebug) {
        #ifdef __linux__
            auto th = photon_std::this_thread::get_id();
            if(th == nullptr){
                LOG_WARN("please init hook in fiber");
                return;
            }
            G_HOOK = true;
            G_HOOK_IS_DEBUG = isDebug;
            G_HOOK_SOCKET_INS = new ZyIo::Socket::ZySokect();
            G_HOOK_SOCKET_INS->start();
            LOG_INFO("enable block socket api hook");
        #else
            LOG_WARN("not support hook for this system");
        #endif
        }

        unsigned int sleep_hook(photon::thread* th,unsigned int seconds){
            return photon::thread_usleep(seconds);
        }

        int accept_hook(photon::thread* th,int sockfd, struct sockaddr *addr, socklen_t *addrlen){
            //submit accept
            __s32 r=-100;
            __s32* res =&r;
            G_HOOK_SOCKET_INS->submitAccept(th,res,sockfd,addr,addrlen);
            //yield()
            photon::thread_usleep(-1UL);
            //wait notify
            return *res;
        }

        int connect_hook(photon::thread* th,int sockfd, const struct sockaddr *addr, socklen_t addrlen){
            //submit connect
            __s32 r=-100;
            __s32* res =&r;
            G_HOOK_SOCKET_INS->submitConnect(th,res,sockfd,addr,addrlen);
            //yield()
            photon::thread_usleep(-1UL);
            //wait notify
            return *res;
        }

        ssize_t read_hook(photon::thread* th,int fd, void *buf, size_t count){
            //submit read
            __s32 r=-100;
            __s32* res =&r;
            G_HOOK_SOCKET_INS->submitRead(th,res,fd,buf,count);
            //yield()
            photon::thread_usleep(-1UL);
            //wait notify
            return *res;
        }

        ssize_t write_hook(photon::thread* th,int fd, const void *buf, size_t count){
            //submit write
            __s32 r=-100;
            __s32* res =&r;
            G_HOOK_SOCKET_INS->submitWrite(th,res,fd,buf,count);
            //yield()
            photon::thread_usleep(-1UL);
            //wait notify
            return *res;
        }
    }

}




extern "C" {

unsigned int sleep(unsigned int seconds){
    auto th = photon_std::this_thread::get_id();
    if(ZyIo::Hook::G_HOOK && th != nullptr){
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call hook sleep api");
        }
        return ZyIo::Hook::sleep_hook(th,seconds);
    }else{
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call lib c sleep api");
        }
        return g_sys_sleep_fun(seconds);
    }
}


int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    auto th = photon_std::this_thread::get_id();
    if (ZyIo::Hook::G_HOOK && th != nullptr) {
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call hook accept api");
        }
        return ZyIo::Hook::accept_hook(th,sockfd, addr, addrlen);
    } else {
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call lib c accept api");
        }
        return g_sys_accept_fun(sockfd, addr, addrlen);
    }
}


int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    auto th = photon_std::this_thread::get_id();
    if (ZyIo::Hook::G_HOOK && th != nullptr) {
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call hook connect api");
        }
        return ZyIo::Hook::connect_hook(th,sockfd, addr, addrlen);
    } else {
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call lib c connect api");
        }
        return g_sys_connect_fun(sockfd, addr, addrlen);
    }
}

ssize_t read(int fd, void *buf, size_t count) {
    auto th = photon_std::this_thread::get_id();
    if (ZyIo::Hook::G_HOOK && th != nullptr) {
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call hook read api");
        }
        return ZyIo::Hook::read_hook(th,fd, buf, count);
    } else {
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call lib c read api");
        }
        return g_sys_read_fun(fd, buf, count);
    }
}

ssize_t write(int fd, const void *buf, size_t count) {
    auto th = photon_std::this_thread::get_id();
    if (ZyIo::Hook::G_HOOK && th != nullptr) {
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call write read api");
        }
        return ZyIo::Hook::write_hook(th,fd, buf, count);
    } else {
        if(ZyIo::Hook::G_HOOK_IS_DEBUG){
            LOG_INFO("call lib c write api");
        }
        return g_sys_write_fun(fd, buf, count);
    }
}






}