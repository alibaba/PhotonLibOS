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
#include <photon/net/http/verb.h>
#include <photon/common/estring.h>

namespace photon {
namespace net {

class ISocketStream;

class Headers
{
public:
    struct iterator
    {
        const Headers* h;
        uint16_t i;
        iterator operator++() {return {h, i++};}
        iterator operator--() {return {h, i--};}
        iterator operator++(int) {return {h, ++i};}
        iterator operator--(int) {return {h, --i};}
        std::pair<std::string_view, std::string_view>
        operator*() const { return {first(), second()}; }
        std::string_view first() const
        {
            return h->kv(i).first | h->_buf;
        }
        std::string_view second() const
        {
            return h->kv(i).second | h->_buf;
        }
        bool operator==(const iterator& rhs) const { return h == rhs.h && i == rhs.i; }
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }
        bool operator<(const iterator& rhs) const { return i < rhs.i; }
    };
    iterator find(std::string_view key) const;
    iterator begin() const { return {this, 0};}
    iterator end() const   { return {this, _kv_size};}
    bool empty() const     { return _kv_size == 0; }
    std::string_view operator[](std::string_view key) const
    {
        auto it = find(key);
        return (it == end()) ? std::string_view() : it.second();
    }
    std::string_view operator[](size_t i) const
    {
        auto ret = _buf | kv(i).second;
        assert(ret.data() < _buf + _buf_size);
        assert(ret.end() <= _buf + _buf_size);
        return ret;
    }
    int merge_headers(const Headers &h, int allow_dup = 0) {
        for (auto kv : h)
            if (insert(kv.first, kv.second, allow_dup) == -ENOBUFS) return -ENOBUFS;
        return 0;
    }

    int insert(std::string_view key, std::string_view value, int allow_dup=0);
    bool value_append(std::string_view value);
    uint64_t content_length() const { return _content_length; }

    template<size_t BufCap = 64, typename...Ts>
    int insert_format(std::string_view key, const char* fmt, Ts...xs)
    {
        char buf[BufCap];
        size_t len = snprintf(buf, sizeof(buf), fmt, xs...);
        if (len >= sizeof(buf)) return -ENOBUFS;
        return insert(key, {buf, (size_t)len});
    }

    //use followed func to set content_length, instead of insert()
    int content_length(uint64_t cl) {
        _content_length = cl;
        return insert_format("Content-Length", "%lu", cl);
    }

    int insert_range(uint64_t from, uint64_t to) {
        return insert_format("Range", "bytes=%lu-%lu", from, to);
    }

    size_t space_remain() {
        return _buf_capacity - _buf_size - _kv_size * sizeof(KV);
    }

protected:
    char* _buf;
    uint16_t _buf_size = 0, _kv_size = 0, _buf_capacity = 0, _last_kv = 0;
    uint64_t _content_length = 0;
    using KV = std::pair<rstring_view16, rstring_view16>;
    KV* kv_end() const   { return (KV*)(_buf + _buf_capacity); }
    KV* kv_begin() const { return kv_end() - _kv_size; }
    KV* kv_add(KV kv);
    KV* kv_add_sort(KV kv);
    KV kv(uint16_t i) const
    {
        assert(i < _kv_size);
        return (i < _kv_size) ? kv_begin()[i] : KV{{0, 0}, {0, 0}};
    }
    Headers() = default;
    friend class HeaderAssistant;
};

class URL;

class RequestHeaders : public Headers
{
public:
    RequestHeaders(void* buf, uint16_t buf_capacity, Verb v,
                   std::string_view url, bool enable_proxy = false);
    RequestHeaders(void* buf, uint16_t buf_capacity) {
        _buf_capacity = buf_capacity;
        _buf = (char*)buf;
    }
    Verb verb() const { return (Verb)_verb;}
    std::string_view target() const { return _buf | _target; }
    std::string_view whole();
    bool secure() const { return _secure; }
    uint16_t port() const { return _port; }
    std::string_view host() const { return _buf | _host; }
    std::string_view abs_path() const { return _buf | _path; }
    std::string_view query() const { return _buf | _query; }
    using RemainSpace = std::pair<char*, size_t>;
    int reset(Verb v, std::string_view url, bool enable_proxy = false);
    RemainSpace get_remain_space() const {
        return RemainSpace(_buf + _buf_size,
                           _buf_capacity - _buf_size - _kv_size * sizeof(KV));
    }
    int redirect(Verb v, estring_view location, bool enable_proxy = false);

protected:
    void _make_prefix(Verb v, const URL& u, bool enable_proxy);
    rstring_view16 _target, _host, _path, _query;
    uint16_t _port;
    bool _secure;
    char _verb = (char)Verb::UNKNOWN;
};

class ResponseHeaders : public Headers
{
public:
    // assuming no data at present in the buffer
    // accquire buf_capacity at least 16K
    // if _space_owner = nullptr, meaning ResponseHeaders owned the mem space
    ResponseHeaders() = default;
    ResponseHeaders(char* buf, uint16_t buf_capacity) {
        _buf = buf;
        _buf_capacity = buf_capacity;
    }
    ResponseHeaders(const ResponseHeaders &rhs) = delete;
    ResponseHeaders(ResponseHeaders &&rhs) : Headers(rhs), _version(rhs._version),
                                            _status_message(rhs._status_message),
                                            _body(rhs._body), _status_code(rhs._status_code),
                                            _space_ownership(rhs._space_ownership){
        rhs._space_ownership = false;
    }
    virtual ~ResponseHeaders() {
        if (_space_ownership) {
            free(_buf);
        }
    }
    ResponseHeaders& operator=(const ResponseHeaders &rhs) = delete;
    ResponseHeaders& operator=(const ResponseHeaders &&rhs) = delete;
    //if space_ownership = true, buf should be a malloc() retv
    void reset(char* buf, uint16_t buf_capacity, bool space_ownership = false)
    {
        if (_space_ownership) {
            free(_buf);
        }
        _buf = buf;
        _buf_capacity = buf_capacity;
        _space_ownership = space_ownership;
        _buf_size = 0;
        _kv_size = 0;
        _status_code = 0;
    }
    static ssize_t constexpr _MAX_TRANSFER_BYTES = 4 * 1024;
    static ssize_t constexpr _RESERVED_INDEX_SIZE = 1024;

    // return 0 if "\r\n\r\n" is recvd
    // return 1 if "\r\n\r\n" is not recvd
    // return a negative number if an error occured
    int append_bytes(uint16_t size);
    int append_bytes(ISocketStream* s);

    std::string_view version() const
    {
        return _version | _buf;
    }
    std::string_view status_message() const
    {
        return _status_message | _buf;
    }
    std::string_view partial_body() const
    {
        return _body | _buf;
    }
    uint16_t status_code() const
    {
        return _status_code;
    }

    ssize_t resource_size() const;

protected:
    rstring_view16 _version, _status_message, _body;
    uint16_t _status_code = 0;  // HTTP response code
    bool _space_ownership = false;
};

template<uint16_t BUF_CAPACITY = 64*1024 - 1>
class CommonHeaders : public Headers
{
public:
    CommonHeaders() {
        _buf = _buffer;
        _buf_capacity = BUF_CAPACITY;
    }
protected:
    char _buffer[BUF_CAPACITY];
};

}
}
