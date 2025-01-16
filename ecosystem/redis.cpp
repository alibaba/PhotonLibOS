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

#include <photon/ecosystem/redis.h>
// #include <inttypes.h>
// #include <memory>
#include <photon/net/socket.h>
#include <photon/common/alog.h>
// #include <photon/thread/thread11.h>

namespace photon {
using namespace net;
namespace redis {

any _RedisClient::parse_response_item() {
    switch (auto mark = this->get_char()) {
    case simple_string::mark():
        return get_simple_string();
    case error_message::mark():
        return get_error_message();
    case integer::mark():
        return get_integer();
    case bulk_string::mark():
        return get_bulk_string();
    case array_header::mark(): {
        auto x = get_integer();
        return array_header{x};}
    default:
        LOG_ERROR("unrecognized mark: ", mark);
        return error_message("unrecognized mark");
    }
}

ssize_t _RedisClient::__refill(size_t atleast) {
    size_t room = _bufsize - _j;
    if (!room || room < atleast) { if (_refcnt > 0) {
        LOG_ERROR_RETURN(0, -1 , "no enough buffer space");
    } else {
        size_t available = _j - _i;
        memmove(ibuf(), ibuf() + _i, available);
        _i = 0; _j = available;
        room = _bufsize - _j;
    } }
    ssize_t ret = _s->recv_at_least(ibuf() + _j, room, atleast);
    if (ret < (ssize_t)atleast)
        LOG_ERRNO_RETURN(0, -1, "failed to recv at least ` bytes", atleast);
    _j += ret;
    return ret;
}

std::string_view _RedisClient::__getline() {
    size_t pos;
    assert(_j >= _i);
    estring_view sv(ibuf() + _i, _j - _i);
    while ((pos = sv.find('\n')) == sv.npos) {
        size_t j = _j;
        if (__refill(0) < 0) {
            return {};
        }
        assert(_j > j);
        sv = {ibuf() + j, (uint32_t)(_j - j)};
    }

    assert(sv.begin() >= ibuf());
    auto begin = ibuf() + _i;   // not sv.begin()!
    auto end = &sv[pos];
    assert(*end == '\n');
    _i = end - ibuf() + 1;
    assert(_i <= _j);
    if (likely(end[-1] == '\r')) {
        *(uint16_t*)--end = 0;
    } else {
        *(char*)end = 0;
    }
    return {begin, size_t(end - begin)};
}

std::string_view _RedisClient::__getstring(size_t length) {
    assert(_i <= _j);
    size_t available = _j - _i;
    if (available < length + 2 && __refill(length + 2 - available) < 0) {
        return {};
    }
    auto begin = ibuf() + _i;
    _i += length;
    assert(_i + 2 <= _j);
    auto ptr = ibuf() + _i;
    if (likely(ptr[0] == '\r')) {
        ptr[0] = '\0';
        _i += 1 + (ptr[1]=='\n');
    }
    return {begin, length};
}

ssize_t _RedisClient::flush(const void* extra_buffer, size_t size) {
    iovec iov[2];
    iov[0] = {obuf(), _o};
    int iovcnt = 1;
    ssize_t sum = _o;
    if (extra_buffer && size) {
        iov[iovcnt++] = {(void*)extra_buffer, size};
        sum += size;
    }
    ssize_t ret = _s->writev_mutable(iov, iovcnt);
    if (ret < sum)
        LOG_ERRNO_RETURN(0, -1, "failed to write to socket stream");
    _o = 0;
    return ret;
}

}
}
