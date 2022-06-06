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

/***
Internal header provides abstract socket base class
***/

#include <vector>

#include <photon/common/alog.h>
#include <photon/thread/thread11.h>
#include <photon/net/socket.h>

#define UNIMPLEMENTED(method)                                   \
    virtual method override {                                   \
        LOG_ERROR_RETURN(ENOSYS, -1, #method " unimplemented"); \
    }

#define UNIMPLEMENTED_PTR(method)                                    \
    virtual method override {                                        \
        LOG_ERROR_RETURN(ENOSYS, nullptr, #method " unimplemented"); \
    }

#define UNIMPLEMENTED_VOID(method)                            \
    virtual method override {                                 \
        LOG_ERROR_RETURN(ENOSYS, , #method " unimplemented"); \
    }

namespace photon {
namespace net {
/// Abstract base class combines all socket interfaces
class SocketBase : public ISocketStream,
                   public ISocketClient,
                   public ISocketServer {
public:
    UNIMPLEMENTED(int setsockopt(int level, int option_name,
                                 const void* option_value,
                                 socklen_t option_len))
    UNIMPLEMENTED(int getsockopt(int level, int option_name, void* option_value,
                                 socklen_t* option_len))
    UNIMPLEMENTED(int getsockname(EndPoint& addr))
    UNIMPLEMENTED(int getpeername(EndPoint& addr))
    UNIMPLEMENTED(int getsockname(char* path, size_t count))
    UNIMPLEMENTED(int getpeername(char* path, size_t count))
    UNIMPLEMENTED(uint64_t timeout())
    UNIMPLEMENTED_VOID(void timeout(uint64_t tm))
    UNIMPLEMENTED(int close())
    UNIMPLEMENTED(ssize_t read(void* buf, size_t count))
    UNIMPLEMENTED(ssize_t readv(const struct iovec* iov, int iovcnt))
    UNIMPLEMENTED(ssize_t write(const void* buf, size_t count))
    UNIMPLEMENTED(ssize_t writev(const struct iovec* iov, int iovcnt))
    UNIMPLEMENTED(ssize_t recv(void* buf, size_t count))
    UNIMPLEMENTED(ssize_t recv(const struct iovec* iov, int iovcnt))
    UNIMPLEMENTED(ssize_t send(const void* buf, size_t count))
    UNIMPLEMENTED(ssize_t send(const struct iovec* iov, int iovcnt))
    UNIMPLEMENTED_PTR(ISocketStream* connect(const EndPoint& ep))
    UNIMPLEMENTED_PTR(ISocketStream* connect(const char* path, size_t count))
    UNIMPLEMENTED(int bind(uint16_t port, IPAddr addr))
    UNIMPLEMENTED(int bind(const char* path, size_t count))
    UNIMPLEMENTED(int listen(int backlog = 1024))
    UNIMPLEMENTED_PTR(ISocketStream* accept())
    UNIMPLEMENTED_PTR(ISocketStream* accept(EndPoint* remote_endpoint))
    UNIMPLEMENTED_PTR(ISocketServer* set_handler(Handler handler))
    UNIMPLEMENTED(int start_loop(bool block))
    UNIMPLEMENTED_VOID(void terminate())
    UNIMPLEMENTED(ssize_t send2(const void *buf, size_t count, int flag))
    UNIMPLEMENTED(ssize_t send2(const struct iovec *iov, int iovcnt, int flag))
    UNIMPLEMENTED(ssize_t sendfile(int in_fd, off_t offset, size_t count))
};

struct SocketOpt {
    int level;
    int opt_name;
    void* opt_val;
    socklen_t opt_len;
};
class SockOptBuffer : public std::vector<SocketOpt> {
protected:
    static constexpr uint64_t BUFFERSIZE = 8192;
    char buffer[BUFFERSIZE];
    char* ptr = buffer;

public:
    int put_opt(int level, int name, const void* val, socklen_t len) {
        if (ptr + len >= buffer + BUFFERSIZE) {
            return -1;
        }
        memcpy(ptr, val, len);
        push_back(SocketOpt{level, name, ptr, len});
        ptr += len;
        return 0;
    }
    int get_opt(int level, int name, void* val, socklen_t* len)
    {
        for (auto& x: *this)
            if (level == x.level && name == x.opt_name && *len >= x.opt_len)
                return memcpy(val, x.opt_val, *len = x.opt_len), 0;
        return -1;
    }
};

}  // namespace net
}
