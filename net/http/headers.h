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
#include <cassert>
#include <cerrno>
#include <memory>
#include <algorithm>
#include <sys/uio.h>
#include <photon/net/http/verb.h>
#include <photon/common/estring.h>
#include <photon/common/stream.h>
#include <photon/common/callback.h>

namespace photon {
namespace net {
namespace http {

void buf_append(char*& ptr, uint64_t x);

inline void buf_append(char*& ptr, std::string_view sv) {
    memcpy(ptr, sv.data(), sv.size());
    ptr += sv.size();
}

class HeadersBase {
public:
    HeadersBase() = default;
    struct iterator
    {
        const HeadersBase* h;
        uint16_t i;
        iterator operator++() {return {h, i++};}
        iterator operator--() {return {h, i--};}
        iterator operator++(int) {return {h, ++i};}
        iterator operator--(int) {return {h, --i};}
        std::pair<std::string_view, std::string_view>
        operator*() const { return {first(), second()}; }
        std::string_view first() const {
            return h->kv(i).first | h->m_buf;
        }
        std::string_view second() const {
            return h->kv(i).second | h->m_buf;
        }
        bool operator==(const iterator& rhs) const { return h == rhs.h && i == rhs.i; }
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }
        bool operator<(const iterator& rhs) const { return i < rhs.i; }
    };
    iterator find(std::string_view key) const;
    iterator begin() const { return {this, 0};}
    iterator end() const   { return {this, m_kv_size};}
    bool empty() const     { return m_kv_size == 0; }
    void reset() {
        m_buf_size = m_kv_size = m_last_kv = 0;
    }
    int reset(void* buf, uint16_t buf_capacity, uint16_t buf_size = 0) {
        m_buf = (char*)buf;
        m_buf_capacity = buf_capacity;
        reset();
        if (buf_size) {
            m_buf_size = buf_size;
            return parse();
        }
        return 0;
    }
    std::string_view get_value(std::string_view key) const {
        auto it = find(key);
        return (it == end()) ? std::string_view() : it.second();
    }
    std::string_view operator[](std::string_view key) const {
        return get_value(key);
    }
    std::string_view operator[](size_t i) const {
        auto ret = std::string_view{m_buf, m_buf_size} | kv(i).second;
        assert(ret.data() < m_buf + m_buf_size);
        assert(ret.end() <= m_buf + m_buf_size);
        return ret;
    }
    int merge(const HeadersBase &h, int allow_dup = 0) {
        for (auto kv : h)
            if (insert(kv.first, kv.second, allow_dup) < 0)
                return -1;
        return 0;
    }

    int insert(std::string_view key, std::string_view value, int allow_dup=0);
    bool value_append(std::string_view value);

    template<size_t BufCap = 64, typename...Ts>
    int insert_format(std::string_view key, const char* fmt, Ts...xs) {
        char buf[BufCap];
        size_t len = snprintf(buf, sizeof(buf), fmt, xs...);
        if (len >= sizeof(buf)) return -ENOBUFS;
        return insert(key, {buf, (size_t)len});
    }

    uint16_t kv_size() const { return m_kv_size * sizeof(KV); }
    uint16_t size() const { return m_buf_size; }
    size_t space_remain() const {
        return m_buf_capacity - size() - kv_size();
    }
    int reset_host(int delta, std::string_view host);

protected:
    char* m_buf;
    uint16_t m_buf_size = 0, m_kv_size = 0, m_buf_capacity = 0, m_last_kv = 0;
    friend class HeaderAssistant;

    using KV = std::pair<rstring_view16, rstring_view16>;
    KV kv(uint16_t i) const {
        assert(i < m_kv_size);
        return (i < m_kv_size) ? kv_begin()[i] : KV{{0, 0}, {0, 0}};
    }

    KV* kv_add_sort(KV kv);
    KV* kv_add(KV kv);
    KV* kv_end() const   { return (KV*)(m_buf + m_buf_capacity); }
    KV* kv_begin() const { return kv_end() - m_kv_size; }

    int parse();
};

class Headers: public HeadersBase {
public:
    std::pair<ssize_t, ssize_t> range() const;
    int range(uint64_t from, uint64_t to) {
        return insert_format("Range", "bytes=%lu-%lu", from, to);
    }
    int content_range(size_t start, size_t end, ssize_t size = -1) {
        auto s = (size == -1 ? "*" : std::to_string(size));
        return insert_format("Content-Range", "bytes %lu-%lu/%s", start, end, s.c_str());
    }
    bool chunked() const {
        return get_value("Transfer-Encoding") == "chunked";
    }

    uint64_t content_length() const {
        return estring_view(get_value("Content-Length")).to_uint64();
    }

    int content_length(uint64_t cl) {
        return insert_format("Content-Length", "%lu", cl);
    }
};


template<uint16_t BUF_CAPACITY = 64*1024 - 1>
class CommonHeaders : public Headers {
public:
    CommonHeaders() {
        m_buf = m_buffer;
        m_buf_capacity = BUF_CAPACITY;
    }
protected:
    char m_buffer[BUF_CAPACITY];
};

} // namespace http
} // namespace net
} // namespace photon
