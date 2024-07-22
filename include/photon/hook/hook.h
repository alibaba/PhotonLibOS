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



namespace ZyIo{

    namespace IoUring{

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
        CLASS_FAST_PROPERTY_GETTER(int , fd, Fd)
        CLASS_FAST_PROPERTY_GETTER(__s32*, res, Res)
        CLASS_FAST_PROPERTY_COMM2(size_t, currentDataLen, CurrentDataLen,0)
        CLASS_FAST_PROPERTY_GETTER2(void*, buf, Buf, nullptr)
        CLASS_FAST_PROPERTY_GETTER(size_t, bufLen, BufLen)

        public:
            DataCarrier() = delete;
            DataCarrier(DataFlag flag,photon::thread* tid,int fd,__s32* res,void *buf,size_t len);
            ~DataCarrier();

        };



        class IoUringImp final {
            CLASS_FAST_PROPERTY_GETTER(io_uring*, ring, Ring)

        public:
            static unsigned int DEFAULT_ENTITY_SIZE_S;
            IoUringImp();
            ~IoUringImp();

            void start();

            void startWithFb();

            void submitAccept(photon::thread* th,int sockfd,__s32* res, struct sockaddr *addr, socklen_t *addrlen);

            void submitConnect(photon::thread* th,int sockfd,__s32* res, const struct sockaddr *addr, socklen_t addrlen);

            void submitRead( photon::thread* th,int fd,__s32* res,void *buf, size_t count);

            void submitWrite( photon::thread* th,int fd,__s32* res, void *buf, size_t count);

            void submitRead(DataCarrier *dataCarrier);

            void submitWrite(DataCarrier *dataCarrier);

        private:
            io_uring_sqe* doTake();

            void doSubmit(io_uring_sqe* sqe, DataCarrier* carrier);


        };

        class EvImp{
        public:
            void start();

            void startWithFb();

            void submitRead( photon::thread* th,int fd,__s32* res,void *buf, size_t count);

            void submitWrite( photon::thread* th,int fd,__s32* res, void *buf, size_t count);

            void submitRead(DataCarrier *dataCarrier);

            void submitWrite(DataCarrier *dataCarrier);
        };
    }


    namespace Hook
    {


        class HookFlag{

        public:
            static bool G_HOOK;
            static bool G_HOOK_IS_DEBUG;
            static IoUring::IoUringImp* G_HOOK_IOURING_INS;
            static void init(bool isDebug);


        public:
            HookFlag() = delete;
            ~HookFlag() = default;
        };





        unsigned int sleep_hook( photon::thread* th,unsigned int seconds);

        int accept_hook( photon::thread* th,int sockfd, struct sockaddr *addr, socklen_t *addrlen);

        int connect_hook( photon::thread* th,int sockfd, const struct sockaddr *addr, socklen_t addrlen);

        ssize_t read_hook( photon::thread* th,int fd, void *buf, size_t count);

        ssize_t write_hook( photon::thread* th,int fd, void *buf, size_t count);

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

#endif //URING_SOCKET_V3_HOOK_H