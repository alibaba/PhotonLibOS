//
// Created by jun on 2024/7/4.
//

#include "photon/hook/hook.h"
#include <dlfcn.h>
#include <cstring>


#define HOOK_SYS_FUNC(name) name##_fun_ptr_t g_sys_##name##_fun = (name##_fun_ptr_t)dlsym(RTLD_NEXT, #name);
HOOK_SYS_FUNC(sleep);
HOOK_SYS_FUNC(connect);
HOOK_SYS_FUNC(accept);
HOOK_SYS_FUNC(read);
HOOK_SYS_FUNC(write);


namespace ZyIo{
    namespace IoUring{


        DataCarrier::DataCarrier(DataFlag flag, photon::thread* tid,int fd, __s32 *res,void *buf,size_t len)
                :flag(flag),tid(tid),fd(fd),res(res),buf(buf),bufLen(len) {

        }
        DataCarrier::~DataCarrier()= default;


        unsigned int IoUringImp::DEFAULT_ENTITY_SIZE_S = 128;
        IoUringImp::IoUringImp() {
            auto pUring = new io_uring;
            auto const ret = io_uring_queue_init(DEFAULT_ENTITY_SIZE_S, pUring, 0); // 初始化
            if (ret < 0)
            {
                fprintf(stderr, "io_uring_queue_init error: %d\n",ret);
                exit(EXIT_FAILURE);
            }
            this->ring = pUring;
        }
        IoUringImp::~IoUringImp() {
            delete ring;
        }

        void IoUringImp::startWithFb() {
            auto thread = std::thread(&IoUringImp::start, this);
            thread.detach();
        }

        void processCqe(io_uring* ring,io_uring_cqe* cqe){
            auto dataCarrier = (DataCarrier*)cqe->user_data;
            //1.check read or write
            if(dataCarrier->getFlag() == DataFlag::READ || dataCarrier->getFlag() == DataFlag::WRITE){
                size_t res = cqe->res;
                dataCarrier->setCurrentDataLen(dataCarrier->getCurrentDataLen() + res);
                if(dataCarrier->getCurrentDataLen() == dataCarrier->getBufLen()){
                    //return res
                    *(dataCarrier->getRes()) = dataCarrier->getBufLen();
                    //resume thead
                    photon::thread_interrupt(dataCarrier->getTid());
                    delete dataCarrier;
                    io_uring_cqe_seen(ring, cqe);
                    return;
                }
                //keep read
                io_uring_cqe_seen(ring, cqe);
                if(dataCarrier->getFlag() == DataFlag::READ){
                    Hook::HookFlag::G_HOOK_IOURING_INS->submitRead(dataCarrier);
                }else{
                    Hook::HookFlag::G_HOOK_IOURING_INS->submitWrite(dataCarrier);
                }
                return;
            }
            //2.connect accept ...
            //return res
            *(dataCarrier->getRes()) = cqe->res;
            //resume thead
            photon::thread_interrupt(dataCarrier->getTid());
            delete dataCarrier;
            io_uring_cqe_seen(ring, cqe);
        }

        void IoUringImp::start() {
            while (true)
            {

//                struct io_uring_cqe *cqe;
//                int ret =io_uring_wait_cqe(ring,&cqe);
//                if(ret <0){
//                    continue;
//                }
//                processCqe(ring,cqe);
//                批量获取
                usleep(500);
                io_uring_cqe* cqes[64] = {};
                auto cqeCount = io_uring_peek_batch_cqe(ring, cqes, 64);
                if (cqeCount <= 0)
                {
                    continue;
                }
                for (unsigned int i = 0; i < cqeCount; i++)
                {
                    processCqe(ring,cqes[i]);
                }
            }
        }



        io_uring_sqe *IoUringImp::doTake() {
            return io_uring_get_sqe(ring);
        }

        void IoUringImp::doSubmit(io_uring_sqe *sqe, DataCarrier *carrier) {
            io_uring_sqe_set_data(sqe, carrier);
            io_uring_submit(ring);
        }

        void IoUringImp::submitConnect(photon::thread *th, int sockfd, __s32 *res,  const struct sockaddr *addr,
                                     socklen_t addrlen) {
            auto carrier = new DataCarrier(CONNETC, th,sockfd,res, nullptr,0);
            auto sqe = doTake();
            io_uring_prep_connect(sqe, sockfd, addr, addrlen);
            doSubmit(sqe, carrier);
        }

        void IoUringImp::submitAccept(photon::thread *th, int sockfd,__s32 *res,  struct sockaddr *addr, socklen_t *addrlen) {
            auto carrier = new DataCarrier(ACCEPT, th,sockfd,res, nullptr,0);
            auto sqe = doTake();
            io_uring_prep_accept(sqe, sockfd, addr, addrlen, 0);
            doSubmit(sqe, carrier);
        }

        void IoUringImp::submitRead(photon::thread *th, int fd,__s32 *res,  void *buf, size_t count) {
            auto carrier = new DataCarrier(READ, th,fd,res,buf,count);
            auto sqe = doTake();
            io_uring_prep_read(sqe, fd, carrier->getBuf(), carrier->getBufLen(), 0);
            doSubmit(sqe, carrier);
        }

        void IoUringImp::submitWrite(photon::thread *th, int fd,__s32 *res,void *buf, size_t count) {
            auto carrier = new DataCarrier(WRITE, th,fd,res,buf,count);
            auto sqe = doTake();
            io_uring_prep_write(sqe, fd, carrier->getBuf(), carrier->getBufLen(), 0);
            doSubmit(sqe, carrier);
        }

        void IoUringImp::submitRead(ZyIo::IoUring::DataCarrier *dataCarrier) {
            auto sqe = doTake();
            io_uring_prep_read(sqe, dataCarrier->getFd(),dataCarrier->getBuf() , dataCarrier->getBufLen() - dataCarrier->getCurrentDataLen(), dataCarrier->getCurrentDataLen());
            doSubmit(sqe, dataCarrier);
        }

        void IoUringImp::submitWrite(ZyIo::IoUring::DataCarrier *dataCarrier) {
            auto sqe = doTake();
            io_uring_prep_write(sqe, dataCarrier->getFd(),dataCarrier->getBuf() , dataCarrier->getBufLen() - dataCarrier->getCurrentDataLen(), dataCarrier->getCurrentDataLen());
            doSubmit(sqe, dataCarrier);
        }



    }


    namespace Hook{
        bool HookFlag::G_HOOK = false;
        bool HookFlag::G_HOOK_IS_DEBUG = false;
        IoUring::IoUringImp* HookFlag::G_HOOK_IOURING_INS = nullptr;
        void HookFlag::init(bool isDebug) {
            #ifdef __linux__
            auto th = photon_std::this_thread::get_id();
            if(th == nullptr){
                fprintf(stderr, "please init hook in fiber\n");
                return;
            }
            HookFlag::G_HOOK_IOURING_INS = new IoUring::IoUringImp();
            HookFlag::G_HOOK_IOURING_INS->startWithFb();
            HookFlag::G_HOOK_IS_DEBUG = isDebug;
            HookFlag::G_HOOK = true;
            printf("enable block socket api hook\n");
            #else
            fprintf(stderr, "not support hook for this system\n");
            #endif
        }



        unsigned int sleep_hook(photon::thread *th, unsigned int seconds) {
            return photon::thread_sleep(seconds);
        }

        int accept_hook(photon::thread *th, int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
            //submit accept
            __s32 r=-100;
            __s32* res =&r;
            HookFlag::G_HOOK_IOURING_INS->submitAccept(th,sockfd,res,addr,addrlen);
            //yield()
            photon::thread_sleep(-1U);
            //wait notify
            return *res;
        }


        int connect_hook(photon::thread* th,int sockfd, const struct sockaddr *addr, socklen_t addrlen){
            //submit connect
            __s32 r=-100;
            __s32* res =&r;
            HookFlag::G_HOOK_IOURING_INS->submitConnect(th,sockfd,res,addr,addrlen);
            //yield()
            photon::thread_sleep(-1U);
            //wait notify
            return *res;
        }

        ssize_t read_hook(photon::thread* th,int fd, void *buf, size_t count){
            //submit read
            __s32 r=-100;
            __s32* res =&r;
            HookFlag::G_HOOK_IOURING_INS->submitRead(th,fd,res,buf,count);
            //yield()
            photon::thread_sleep(-1U);
            //wait notify
            return *res;
        }

        ssize_t write_hook(photon::thread* th,int fd, void *buf, size_t count){
            //submit write
            __s32 r=-100;
            __s32* res =&r;
            HookFlag::G_HOOK_IOURING_INS->submitWrite(th,fd,res,buf,count);
            //yield()
            photon::thread_sleep(-1U);
            //wait notify
            return *res;
        }
    }

}




extern "C" {

unsigned int sleep(unsigned int seconds){
    auto th = photon_std::this_thread::get_id();
    if(ZyIo::Hook::HookFlag::G_HOOK && th != nullptr){
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            printf("call hook sleep api,sleep:%d s\n",seconds);
        }
        return ZyIo::Hook::sleep_hook(th,seconds);
    }else{
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            printf("call lib c sleep api,sleep:%d s\n",seconds);
        }
        return g_sys_sleep_fun(seconds);
    }
}


int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    auto th = photon_std::this_thread::get_id();
    if (ZyIo::Hook::HookFlag::G_HOOK && th != nullptr) {
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            printf("call hook accept api\n");
        }
        return ZyIo::Hook::accept_hook(th,sockfd, addr, addrlen);
    } else {
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            printf("call lib c accept api\n");
        }
        return g_sys_accept_fun(sockfd, addr, addrlen);
    }
}


int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    auto th = photon_std::this_thread::get_id();
    if (ZyIo::Hook::HookFlag::G_HOOK && th != nullptr) {
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            printf("call hook connect api\n");
        }
        return ZyIo::Hook::connect_hook(th,sockfd, addr, addrlen);
    } else {
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            printf("call lib c connect api\n");
        }
        return g_sys_connect_fun(sockfd, addr, addrlen);
    }
}

ssize_t read(int fd, void *buf, size_t count) {
    auto th = photon_std::this_thread::get_id();
    if (ZyIo::Hook::HookFlag::G_HOOK && th != nullptr) {
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            printf("call hook read api\n");
            size_t len = ZyIo::Hook::read_hook(th,fd, buf, count);
            printf("expect data len:%zu,success read data len:%zu\n",count,len);
            return len;
        }
        return ZyIo::Hook::read_hook(th,fd, buf, count);
    } else {
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            printf("call lib c read api\n");
            size_t len = g_sys_read_fun(fd, buf, count);
            printf("expect data len:%zu,success read data len:%zu\n",count,len);
            return len;
        }
        return g_sys_read_fun(fd, buf, count);
    }
}

ssize_t write(int fd, const void *buf, size_t count) {
    auto th = photon_std::this_thread::get_id();
    if (ZyIo::Hook::HookFlag::G_HOOK && th != nullptr) {
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            size_t res = ZyIo::Hook::write_hook(th, fd, const_cast<void *>(buf), count);
            printf("call hook write read api,write data len:%zu,write res:%zu\n",count,res);
            return res;
        }
        return ZyIo::Hook::write_hook(th, fd, const_cast<void *>(buf), count);
    } else {
        if(ZyIo::Hook::HookFlag::G_HOOK_IS_DEBUG){
            printf("call lib c write api,write data len:%zu\n",count);
        }
        return g_sys_write_fun(fd, buf, count);
    }
}






}