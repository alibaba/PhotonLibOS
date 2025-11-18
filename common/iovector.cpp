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

#include <sstream>
#define protected public
#include "iovector.h"
#include "utility.h"
#include "alog.h"

size_t iovector_view::sum() const
{
    size_t s = 0;
    for (int i = 0; i < iovcnt; ++i)
        s += iov[i].iov_len;
    return s;
}

size_t iovector_view::shrink_to(size_t size)
{
    if (size == 0)
        return iovcnt = 0;

    auto size0 = size;
    for (int i = 0; i < iovcnt; ++i)
    {
        if (size <= iov[i].iov_len)
        {
            iov[i].iov_len = size;
            iovcnt = i + 1;
            return size0;
        }
        else size -= iov[i].iov_len;
    }
    return size0 - size;
}

size_t iovector_view::shrink_less_than(size_t size)
{
    if (size == 0)
    {
        if (iovcnt)
        {
            iovcnt = 0;
            return iov[0].iov_len;
        }
        return 0;
    }

    for (int i = 0; i < iovcnt; ++i)
    {
        if (size <= iov[i].iov_len)
        {
            iovcnt = i + 1;
            return iov[i].iov_len - size;
        }
        else size -= iov[i].iov_len;
    }
    return 0;
}

ssize_t iovector_view::slice(size_t count, off_t offset, iovector_view* /*OUT*/ iov) const
{
    if (iov == nullptr || iov->iovcnt == 0) {
        // empty iov, takes as no useable nested iovec to store the result
        return -1;
    }
    int cnt = 0;
    off_t pos = 0;
    auto ptr = iov->iov;
    ssize_t ret = 0;
    for (auto &p : *this) {
        if (count && (pos + (off_t)p.iov_len > offset)) {
            if (offset > pos) {
                ptr[cnt].iov_base = (char*)p.iov_base + (offset - pos);
                ptr[cnt].iov_len = p.iov_len - (offset - pos);
            } else {
                ptr[cnt].iov_base = p.iov_base;
                ptr[cnt].iov_len = p.iov_len;
            }
            if (count < ptr[cnt].iov_len)
                ptr[cnt].iov_len = count;
            pos += p.iov_len;
            ret += ptr[cnt].iov_len;
            count -= ptr[cnt].iov_len;
            cnt ++;
            if (cnt == iov->iovcnt)
                break;
        }
    }
    iov->iovcnt = cnt;
    return ret;
}

struct ioview : public iovector_view
{
    template<typename CB> __INLINE__
    ssize_t do_extract_front(size_t bytes, const CB& cb)
    {
        if (bytes == 0)
            return 0;

        auto bytes0 = bytes;
        while(!empty() > 0)
        {
            auto& v = front();
            if (bytes <= v.iov_len)
            {
                if (cb(v.iov_base, bytes) < 0)
                    return -1;
                v.iov_len -= bytes;
                if (v.iov_len == 0) {
                    pop_front();
                } else {
                    (char*&)v.iov_base += bytes;
                }
                bytes = 0;
                break;
            }
            else
            {
                if (cb(v.iov_base, v.iov_len) < 0)
                    return -1;
                bytes -= v.iov_len;
                pop_front();
            }
        }
        return bytes0 - bytes;
    }

    template<typename CB> __INLINE__
    ssize_t do_extract_back(size_t bytes, const CB& cb)
    {
        if (bytes == 0)
            return 0;

        auto bytes0 = bytes;
        while(!empty() > 0)
        {
            auto& v = back();
            if (bytes <= v.iov_len)
            {
                auto ptr = (char*)v.iov_base + v.iov_len - bytes;
                if (cb(ptr, bytes) < 0)
                    return -1;
                v.iov_len -= bytes;
                if (v.iov_len == 0)
                    pop_back();
//                else (char*&)v.iov_base += bytes;
                bytes = 0;
                break;
            }
            else
            {
                if (cb(v.iov_base, v.iov_len) < 0)
                    return -1;
                bytes -= v.iov_len;
                pop_back();
            }
        }
        return bytes0 - bytes;
    }
};

#define _this static_cast<ioview*>(this)
size_t iovector_view::extract_front(size_t bytes)
{
    return _this->do_extract_front(bytes, [](void*, size_t) __INLINE__ { return 0; });
}

size_t iovector_view::extract_front(size_t bytes, void* buf)
{
    return _this->do_extract_front(bytes, [&](void* ptr, size_t size) __INLINE__
    {
        memcpy(buf, ptr, size);
        (char*&)buf += size;
        return 0;
    });
}

ssize_t iovector_view::extract_front(size_t bytes, iovector_view* iov)
{
    auto N = iov->iovcnt;
    iov->iovcnt = 0;
    return _this->do_extract_front(bytes, [&](void* ptr, size_t size) __INLINE__
    {
//        LOG_DEBUG(VALUE(ptr), VALUE(size));
        if (iov->iovcnt == N)
            return -1;
        iov->iov[iov->iovcnt++] = {ptr, size};
        return 0;
    });
}

size_t iovector_view::extract_back(size_t bytes)
{
    return _this->do_extract_back(bytes, [](void*, size_t) __INLINE__ { return 0; });
}

size_t iovector_view::extract_back(size_t bytes, void* buf)
{
    (char*&)buf += bytes;
    return _this->do_extract_back(bytes, [&](void* ptr, size_t size) __INLINE__
    {
        (char*&)buf -= size;
        memcpy(buf, ptr, size);
        return 0;
    });
}

ssize_t iovector_view::extract_back(size_t bytes, iovector_view* iov)
{
    auto N = iov->iovcnt;
    auto begin = iov->iov + N;
    auto end = begin;
    iov->iovcnt = 0;
    auto ret = _this->do_extract_back(bytes, [&](void* ptr, size_t size) __INLINE__
    {
       if (begin == iov->iov)
           return -1;
        *--begin = {ptr, size};
        return 0;
    });

    iov->iov = begin;
    iov->iovcnt = (int)(end - begin);
    return ret;
}
#undef _this

inline void operator+=(iovec& v, size_t nbytes) {
    (char*&)v.iov_base += nbytes;
    v.iov_len -= nbytes;
}

class iov_iterator {
    const iovec* _iov;
    iovec _v;
    int _iovcnt;
public:
    iov_iterator(iov_iterator&&) = default;
    iov_iterator(const iov_iterator&) = default;
    iov_iterator(iovector_view v) : _iov(v.iov), _v(v.iov[0]), _iovcnt(v.iovcnt) { }
    bool empty() const { return _iovcnt == 0; }
    iovec front() const { return _v; }
    iov_iterator& operator += (size_t n) {
        assert(_iovcnt);
        assert(n <= _v.iov_len);
        if (n < _v.iov_len) { _v += n; }
        else { 
            _v = *++_iov; 
            _iovcnt--; 
            assert(_iovcnt == 0 || _v.iov_base);
        }
        return *this;
    }
};

inline size_t min(size_t a, size_t b, size_t c) {
    return std::min(a, std::min(b, c));
}

template<typename T, typename P> inline
size_t _copy_pipe_iov(T&& dest, P&& src, size_t size) {
    auto size0 = size;
    while (size && !dest.empty() && !src.empty()) {
        auto df = dest.front(), sf = src.front();
        size_t stepsize = min(size, df.iov_len, sf.iov_len);
        // LOG_DEBUG("memcpy(", df.iov_base, ", ", sf.iov_base, ", ", stepsize, ")");
        memcpy(df.iov_base, sf.iov_base, stepsize);
        size -= stepsize;
        dest += stepsize;
        src += stepsize;
    }
    return size0 - size;
}

size_t iovector_view::memcpy_iov(iovector_view d, iovector_view s, size_t size) {
    return _copy_pipe_iov(iov_iterator(d), iov_iterator(s), size);
}

template<typename T>
struct src_extractor : public T {
    void operator+=(size_t n) {
        assert(!this->empty());
        auto& v = this->front();
        assert(n <= v.iov_len);
        if (n < v.iov_len) { v += n; }
        else { this->pop_front(); }
    }
};

size_t iovector_view::pipe_iov(iovector_view d, iovector_view& src, size_t size) {
    return _copy_pipe_iov(iov_iterator(d), (src_extractor<iovector_view>&&)src, size);
}

size_t iovector::pipe_iov(iovector_view d, iovector& s, size_t size) {
    return _copy_pipe_iov(iov_iterator(d), (src_extractor<iovector>&&)s, size);
}

size_t iovector::push_front_more(size_t bytes)
{
    auto bytes0 = bytes;
    while(bytes)
    {
        if (iov_begin == 0) {
            LOG_ERROR_RETURN(ENOBUFS, bytes0 - bytes, "no more preserved space iovs[]");
            break;
        }
        auto v = new_iovec(bytes);
        if (v.iov_len == 0)
            break;
        bytes -= v.iov_len;
        push_front(v);
    }
    return bytes0 - bytes;
}

size_t iovector::push_back_more(size_t bytes)
{
    auto bytes0 = bytes;
    while(bytes)
    {
        if (iov_end >= capacity) {
            LOG_ERROR_RETURN(ENOBUFS, bytes0 - bytes,
                    "no more buffer space in iovs[] (capacity: `)", capacity);
        }
        auto v = new_iovec(bytes);
        if (v.iov_len == 0) {
            break;
        }
        bytes -= v.iov_len;
        push_back(v);
    }
    return bytes0 - bytes;
}

void iovector::debug_print() {
    std::stringstream ss;
    ss << "iov sum: " << sum() << ", ";
    for (auto each : *this) {
        ss << "{addr: " << each.iov_base << ", len: " << each.iov_len << "}, ";
    }
    LOG_DEBUG(ss.str().c_str());
}

