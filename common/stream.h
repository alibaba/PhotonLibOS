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
#include <sys/types.h>
#include <memory>
#include <photon/common/object.h>

struct iovec;

enum class ShutdownHow : int { Read = 0, Write = 1, ReadWrite = 2 };

class IStream : public Object
{
public:
    virtual int close() = 0;

    virtual int shutdown(ShutdownHow how) { return 0; }

    // should keep read/readv write/writev as photon-safe atomic operation
    virtual ssize_t read(void *buf, size_t count) = 0;
    virtual ssize_t readv(const struct iovec *iov, int iovcnt) = 0;
    virtual ssize_t readv_mutable(struct iovec *iov, int iovcnt)
    {   // there might be a faster implementaion in derived class
        return readv(iov, iovcnt);
    }

    virtual ssize_t write(const void *buf, size_t count) = 0;
    virtual ssize_t writev(const struct iovec *iov, int iovcnt) = 0;
    virtual ssize_t writev_mutable(struct iovec *iov, int iovcnt)
    {   // there might be a faster implementaion in derived class
        return writev(iov, iovcnt);
    }

    struct ReadAll {
        struct FreeDeleter {
            void operator()(void* ptr) {
                ::free(ptr);
            }
        };
        std::unique_ptr<void, FreeDeleter> ptr;
        ssize_t size;   // <= 0 if error occured; |size| is always the # of bytes read
    };

    // read until EOF
    ReadAll readall(size_t max_buf = 1024 * 1024 * 1024, size_t min_buf = 1024);

    // member function pointer to either read() or write()
    typedef ssize_t (IStream::*FuncIO) (void *buf, size_t count);
    FuncIO _and_read()  { return &IStream::read; }
    FuncIO _and_write() { return (FuncIO)&IStream::write; }
    bool is_readf(FuncIO f) { return f == _and_read(); }
    bool is_writef(FuncIO f) { return f == _and_write(); }

    // member function pointer to either readv() or writev(), the non-const iovec* edition
    typedef ssize_t (IStream::*FuncIOV_mutable) (struct iovec *iov, int iovcnt);
    FuncIOV_mutable _and_readv_mutable()  { return &IStream::readv_mutable; }
    FuncIOV_mutable _and_writev_mutable() { return &IStream::writev_mutable; }
    bool is_readf(FuncIOV_mutable f) { return f == _and_readv_mutable(); }
    bool is_writef(FuncIOV_mutable f) { return f == _and_writev_mutable(); }

    // member function pointer to either readv() or writev(), the const iovec* edition
    typedef ssize_t (IStream::*FuncIOCV) (const struct iovec *iov, int iovcnt);
    FuncIOCV _and_readcv()  { return &IStream::readv; }
    FuncIOCV _and_writecv() { return &IStream::writev; }
    bool is_readf(FuncIOCV f) { return f == _and_readcv(); }
    bool is_writef(FuncIOCV f) { return f == _and_writecv(); }
};

