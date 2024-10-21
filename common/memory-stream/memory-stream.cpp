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

#include "memory-stream.h"
#include <photon/common/ring.h>
#include <photon/common/utility.h>

#include <fcntl.h>

using namespace std;
using namespace photon;

class SimplexMemoryStream final : public IStream
{
public:
    RingBuffer m_ringbuf;
    bool closed = false;
    SimplexMemoryStream(uint32_t capacity) : m_ringbuf(capacity) { }
    virtual int close() override
    {
        closed = true;
        return 0;
    }
    virtual ssize_t read(void *buf, size_t count) override
    {
        if (closed) return -1;
        return m_ringbuf.read(buf, count);
    }
    virtual ssize_t readv(const struct iovec *iov, int iovcnt) override
    {
        if (closed) return -1;
        return m_ringbuf.readv(iov, iovcnt);
    }
    virtual ssize_t write(const void *buf, size_t count) override
    {
        if (closed) return -1;
        return m_ringbuf.write(buf, count);
    }
    virtual ssize_t writev(const struct iovec *iov, int iovcnt) override
    {
        if (closed) return -1;
        return m_ringbuf.writev(iov, iovcnt);
    }
};

class DuplexMemoryStreamImpl : public DuplexMemoryStream
{
public:
    class EndPoint : public IStream
    {
    public:
        IStream* s1;
        IStream* s2;
        bool closed = false;
        EndPoint(IStream* s1, IStream* s2) : s1(s1), s2(s2) { }
        virtual int close() override
        {
            closed = true;
            return 0;
        }
        virtual ssize_t read(void *buf, size_t count) override
        {
            if (closed) return -1;
            return s1->read(buf, count);
        }
        virtual ssize_t readv(const struct iovec *iov, int iovcnt) override
        {
            if (closed) return -1;
            return s1->readv(iov, iovcnt);
        }
        virtual ssize_t write(const void *buf, size_t count) override
        {
            if (closed) return -1;
            return s2->write(buf, count);
        }
        virtual ssize_t writev(const struct iovec *iov, int iovcnt) override
        {
            if (closed) return -1;
            return s2->writev(iov, iovcnt);
        }
    };

    EndPoint epa, epb;
    SimplexMemoryStream s1, s2;
    DuplexMemoryStreamImpl(uint32_t capacity) :
        epa(&s1, &s2), epb(&s2, &s1), s1(capacity), s2(capacity)
    {
        endpoint_a = &epa;
        endpoint_b = &epb;
    }
    virtual int close() override
    {
        epa.close();
        epb.close();
        return 0;
    }
};

IStream* new_simplex_memory_stream(uint32_t capacity)
{
    return new SimplexMemoryStream(capacity);
}

DuplexMemoryStream* new_duplex_memory_stream(uint32_t capacity)
{
    return new DuplexMemoryStreamImpl(capacity);
}

class FaultStream : public IStream
{
public:
    IStream* m_stream;
    int m_flags;
    bool m_ownership;

    virtual ~FaultStream() {
        if (m_ownership)
            delete m_stream;
    }
    FaultStream(IStream* stream, int flags, bool ownership): m_stream(stream), m_flags(flags), m_ownership(ownership) {}

    static inline bool gen()
    {
        if (rand() % 100 < 1)
        {
            static const uint16_t e[] = {
                   1,    2,   83,  111,  112,
                 113,  114,  115,  116,  117,
                 118,  119,  120,  121,  122,
                 123,  124,  125,  126,  127,
                 128,  129,  130,  131,  132,
                 133,  134,  135,  136,  137,
                 138,  139,  140,  141,  142,
                 143,  144,  145,  146,  147,
                 148,  149,  150,  151,  152,
                 153,  156,  157,  158,  159,
                 160,  162,  163,  164,  165,
                 166,  167,  168,  169,  183,
                 200,  201, 1000, 1001, 1002,
                1003, 1004, 1005, 1006, 1007,
                1008, 1009, 1100, 1101, 1102,
                1103, 1104, 1105, 1106, 1107,
                1108, 1109, 1110, 1111, 1112,
                1113, 1114, 1115, 1116, 1117,
                1118, 1119, 1120, 1121, 1122,
                1123, 1124, 1125, 1126, 1127,
                1128, 1129, 1130, 1131, 1132,
                1133, 1134, 1135, 1136, 1137,
                1138, 1139, 1140, 1141, 1142,
                1143, 1144, 1145, 1146, 1147,
                1148, 1149, 1150, 1151, 1152,
            };
            errno = e[rand() % LEN(e)];
            return true;
        }
        return false;
    }

    #define IF_GEN_FAULT_RETURN(x) if (gen()) return (x);

    virtual int close() override { return m_stream->close(); }
    virtual ssize_t read(void* buf, size_t count) override {
        if (m_flags & 1)
            IF_GEN_FAULT_RETURN(-1);
        return m_stream->read(buf, count);
    }

    virtual ssize_t readv(const struct iovec* iov, int iovcnt) override {
        if (m_flags & 1)
            IF_GEN_FAULT_RETURN(-1);
        return m_stream->readv(iov, iovcnt);
    }

    virtual ssize_t write(const void* buf, size_t count) override {
        if (m_flags & 2)
            IF_GEN_FAULT_RETURN(-1);
        return m_stream->write(buf, count);
    }

    virtual ssize_t writev(const struct iovec* iov, int iovcnt) override {
        if (m_flags & 2)
            IF_GEN_FAULT_RETURN(-1);
        return m_stream->writev(iov, iovcnt);
    }

};

using namespace photon::net;
class StringSocketStreamImpl : public StringSocketStream {
public:
    bool _closed = false;
    #define ERROR_RETURN(code, retv) {errno = code; return retv; }
    virtual ssize_t recv(void *buf, size_t count, int flags = 0) override {
        if (_closed) ERROR_RETURN(ECANCELED, -1);
        size_t n = rand() % 8 + 1;
        if (n > _inv.size()) n = _inv.size();
        if (n > count) n = count;
        memcpy(buf, _inv.data(), n);
        _inv = _inv.substr(n);
        return n;
    }
    virtual ssize_t recv(const struct iovec *iov, int iovcnt, int flags = 0) override {
        if (_closed) ERROR_RETURN(ECANCELED, -1);
        while(!iov->iov_base || !iov->iov_len)
            if (iovcnt) { ++iov, --iovcnt; }
            else return -1;
        return recv(iov->iov_base, iov->iov_len);
    }
    virtual ssize_t send(const void *buf, size_t count, int flags = 0) override {
        if (_closed) ERROR_RETURN(ECANCELED, -1);
        size_t n = rand() % 8 + 1;
        if (n > count) n = count;
        auto p = (const char*)buf;
        _out.append(p, p + n);
        return n;
    }
    virtual ssize_t send(const struct iovec *iov, int iovcnt, int flags = 0) override {
        if (_closed) ERROR_RETURN(ECANCELED, -1);
        while(!iov->iov_base || !iov->iov_len)
            if (iovcnt) { ++iov, --iovcnt; }
            else return -1;
        return send(iov->iov_base, iov->iov_len);
    }
    virtual int close() override {
        if (_closed) ERROR_RETURN(ECANCELED, -1);
        _closed = true;
        _inv = {};
        _in.clear();
        _out.clear();
        return 0;
    }
    virtual ssize_t read(void *buf, size_t count) override {
        if (_closed) ERROR_RETURN(ECANCELED, -1);
        if (count > _inv.size()) count = _inv.size();
        memcpy(buf, _inv.data(), count);
        return count;
    }
    virtual ssize_t readv(const struct iovec *iov, int iovcnt) override {
        ssize_t s = 0;
        for (int i = 0; i < iovcnt; ++i) {
            ssize_t ret = read(iov[i].iov_base, iov[i].iov_len);
            if (ret < 0) return ret;
            s += ret;
            if (ret < (ssize_t)iov[i].iov_len) break;
        }
        return s;
    }
    virtual ssize_t write(const void *buf, size_t count) override {
        if (_closed) ERROR_RETURN(ECANCELED, -1);
        auto p = (const char*)buf;
        _out.append(p, p + count);
        return count;
    }
    virtual ssize_t writev(const struct iovec *iov, int iovcnt) override {
        ssize_t s = 0;
        for (int i = 0; i < iovcnt; ++i) {
            ssize_t ret = write(iov[i].iov_base, iov[i].iov_len);
            if (ret < 0) return ret;
            s += ret;
            if (ret < (ssize_t)iov[i].iov_len) break;
        }
        return s;
    }
    virtual Object* get_underlay_object(uint64_t recursion = 0) override { return 0; }
    virtual ssize_t sendfile(int in_fd, off_t offset, size_t count) override { ERROR_RETURN(ENOSYS, -1); }
    virtual int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override { ERROR_RETURN(ENOSYS, -1); }
    virtual int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override { ERROR_RETURN(ENOSYS, -1); }
    virtual int getsockname(EndPoint& addr) override { ERROR_RETURN(ENOSYS, -1); }
    virtual int getpeername(EndPoint& addr) override { ERROR_RETURN(ENOSYS, -1); }
    virtual int getsockname(char* path, size_t count) override { ERROR_RETURN(ENOSYS, -1); }
    virtual int getpeername(char* path, size_t count) override { ERROR_RETURN(ENOSYS, -1); }
};

IStream* new_fault_stream(IStream* stream, int flag, bool ownership) {
    return new FaultStream(stream, flag, ownership);
}

StringSocketStream* new_string_socket_stream() {
    return new StringSocketStreamImpl;
}
