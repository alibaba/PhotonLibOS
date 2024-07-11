//
// Created by jun on 2024/7/4.
//

#ifndef URING_SOCKET_V3_HOOK_H
#define URING_SOCKET_V3_HOOK_H


#include <csignal>
#include <sys/socket.h>
#include <sys/types.h>
#include "CommDef.h"
#include "photon/thread/std-compat.h"
#include "photon/photon.h"
#include <liburing.h>

typedef int (*sleep_fun_ptr_t)(unsigned int seconds);

typedef int (*accept_fun_ptr_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

typedef int (*connect_fun_ptr_t)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

typedef ssize_t (*read_fun_ptr_t)(int fd, void *buf, size_t count);

typedef ssize_t (*write_fun_ptr_t)(int fd, const void *buf, size_t count);






namespace ZyIo{

    namespace Socket{

        /**
        * data flag
        */
        enum DataFlag
        {
            ACCEPT, CONNETC, READ, WRITE
        };
        class DataCarrier final {
        CLASS_FAST_PROPERTY_GETTER(DataFlag, flag, Flag)
        CLASS_FAST_PROPERTY_GETTER(photon::thread* , tid, Tid)
        CLASS_FAST_PROPERTY_GETTER(__s32*, res, Res)

        public:
            DataCarrier() = delete;
            DataCarrier(DataFlag flag,photon::thread* tid,__s32* res);
            ~DataCarrier();

        };



        class ZySokect final {
            CLASS_FAST_PROPERTY_GETTER(io_uring*, ring, Ring)

        public:
            static unsigned int DEFAULT_ENTITY_SIZE_S;
            ZySokect();
            ~ZySokect();

            void start();

            void startWithFb();

            void submitAccept( photon::thread* th,__s32* res,int sockfd, struct sockaddr *addr, socklen_t *addrlen);

            void submitConnect( photon::thread* th,__s32* res,int sockfd, const struct sockaddr *addr, socklen_t addrlen);

            void submitRead( photon::thread* th,__s32* res,int fd, void *buf, size_t count);

            void submitWrite( photon::thread* th,__s32* res,int fd, const void *buf, size_t count);





        protected:
            io_uring_sqe* doTake();

            void doSubmit(io_uring_sqe* sqe, DataCarrier* carrier);
        };
    }


    namespace Hook
    {
        void initHook(bool isDebug);

        unsigned int sleep_hook( photon::thread* th,unsigned int seconds);

        int accept_hook( photon::thread* th,int sockfd, struct sockaddr *addr, socklen_t *addrlen);

        int connect_hook( photon::thread* th,int sockfd, const struct sockaddr *addr, socklen_t addrlen);

        ssize_t read_hook( photon::thread* th,int fd, void *buf, size_t count);

        ssize_t write_hook( photon::thread* th,int fd, const void *buf, size_t count);
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