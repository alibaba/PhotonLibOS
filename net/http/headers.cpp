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

#include "headers.h"
#include <algorithm>
#include <locale>
#include "parser.h"
#include <photon/common/utility.h>
#include "url.h"
#include <photon/common/alog-stdstring.h>
#include <photon/net/socket.h>

using namespace std;

namespace photon {
namespace net {

class HeaderAssistant
{
public:
    const Headers* _h;
    HeaderAssistant(const Headers* h) : _h(h) { }
    //TODO: Case-insensitive string comparison
    bool equal_to(rstring_view16 _k, std::string_view key) const
    {
        return (_k | _h->_buf) == key;
    }
    bool less(rstring_view16 _k1, rstring_view16 _k2) const
    {
        return (_k1 | _h->_buf) < (_k2 | _h->_buf);
    }
    bool less(rstring_view16 _k, std::string_view key) const
    {
        return (_k | _h->_buf) < key;
    }
    bool less(std::string_view key, rstring_view16 _k) const
    {
        return key < (_k | _h->_buf);
    }
};

using HA = HeaderAssistant;
Headers::iterator Headers::find(std::string_view key) const
{
    struct Comp
    {
        const Headers* _Headers;
        Comp(const Headers* h) : _Headers(h) { }
        bool operator()(KV a, std::string_view b) const { return HA(_Headers).less(a.first, b); }
        bool operator()(std::string_view a, KV b) const { return HA(_Headers).less(a, b.first); }
    };
    auto it = std::lower_bound(kv_begin(), kv_end(), key, Comp(this));
    if (it == kv_end() || !HA(this).equal_to(it->first, key)) return end();
    return {this, (uint16_t)(it - kv_begin())};
}

inline void buf_append(char*& ptr, std::string_view sv)
{
    memcpy(ptr, sv.data(), sv.size());
    ptr += sv.size();
}

inline void buf_append(char*& ptr, uint64_t x)
{
    auto begin = ptr;
    do
    {
        *ptr = '0' + x % 10;
        ptr++;
        x /= 10;
    } while(x);
    std::reverse(begin, ptr);
}

int Headers::insert(std::string_view key, std::string_view value, int allow_dup)
{
    if (!allow_dup) {
        auto it = find(key);
        if (it != end()) return -EEXIST;
    }
    uint64_t vbegin = _buf_size + key.size() + 2;
    uint64_t new_size = vbegin + value.size() + 2;
    if (new_size + (_kv_size + 1) * sizeof(KV) > _buf_capacity)
        return -ENOBUFS;
    auto ptr = _buf + _buf_size;
    buf_append(ptr, key);
    buf_append(ptr, ": ");
    buf_append(ptr, value);
    buf_append(ptr, "\r\n");
    _last_kv = kv_add_sort({{_buf_size, (uint16_t)key.size()},
            {(uint16_t)vbegin, (uint16_t)value.size()}}) - kv_begin();
    _buf_size = (uint16_t)new_size;
    return 0;
}

bool Headers::value_append(std::string_view value) {
    if (_last_kv >= _kv_size) return false;
    auto append_size =  value.size();
    uint64_t new_size = _buf_size + append_size;
    if (new_size + _kv_size * sizeof(KV) > _buf_capacity)
        return false;
    auto ptr = _buf + _buf_size - 2;
    buf_append(ptr, value);
    buf_append(ptr, "\r\n");
    _buf_size = (uint16_t)new_size;
    kv_begin()[_last_kv].second.size() += append_size;
    return true;
}

Headers::KV* Headers::kv_add(KV kv)
{
    _kv_size++;
    auto _kv = kv_begin();
    if ((char*)_kv <= _buf + _buf_size)
        return nullptr;
    _kv[0] = kv;
    return _kv;
}

Headers::KV* Headers::kv_add_sort(KV kv)
{
    auto begin = kv_begin();
    if ((char*)(begin - 1) <= _buf + _buf_size)
        return nullptr;
    auto it = std::lower_bound(begin, kv_end(), kv,
                               [this](const KV& lh, const KV& rh) {
                                   return HA(this).less(lh.first, rh.first);
                               });
    memmove(begin - 1,  begin, sizeof(KV) * (it - begin));
    _kv_size++;
    *(it - 1) = kv;
    return it - 1;
}
Verb string_to_verb(std::string_view v) {
    for (auto i = Verb::UNKNOWN; i <= Verb::UNLINK; i = Verb((int)i + 1)) {
        if (verbstr[i] == v) return i;
    }
    return Verb::UNKNOWN;
}
inline size_t full_url_size(const URL& u) {
    return u.target().size() + u.host_port().size() +
           (u.secure() ? sizeof(https_url_scheme) : sizeof(http_url_scheme)) - 1;
}
void RequestHeaders::_make_prefix(Verb v, const URL& u, bool enable_proxy) {
    _secure = u.secure();
    _port = u.port();
    _verb = (char)v;
    char* buf = _buf;
    buf_append(buf, verbstr[v]);
    buf_append(buf, " ");
    uint16_t target_disp = buf - _buf;
    _target = {uint16_t(buf - _buf), u.target().size()};
    if (enable_proxy) {
        _target = {uint16_t(buf - _buf), full_url_size(u)};
        buf_append(buf, u.secure() ? https_url_scheme : http_url_scheme);
        buf_append(buf, u.host_port());
    }
    buf_append(buf, u.target());
    uint16_t path_offset = (uint16_t)(u.path().data() - u.target().data()) + target_disp;
    _path = {path_offset, u.path().size()};
    uint16_t query_offset = (uint16_t)(u.query().data() - u.target().data()) + target_disp;
    _query = {query_offset, u.query().size()};
    buf_append(buf, " HTTP/1.1\r\n");
    _buf_size = buf - _buf;
}

RequestHeaders::RequestHeaders(void* buf, uint16_t buf_capacity, Verb v,
                               std::string_view url, bool enable_proxy)
{
    _buf_capacity = buf_capacity;
    _buf = (char*)buf;
    auto ret = reset(v, url, enable_proxy);
    if (ret != 0) assert(false);
}

int RequestHeaders::reset(Verb v, std::string_view url, bool enable_proxy) {
    URL u(url);
    if ((size_t)_buf_capacity <= u.target().size() + 21 + verbstr[v].size()) return -1;
    _kv_size = 0;
    _make_prefix(v, u, enable_proxy);
    _host = {uint16_t(_buf_size + 6), u.host().size()};
    insert("Host", u.host_port());
    return 0;
}
std::string_view RequestHeaders::whole() {
    if (_buf_size + sizeof(KV) * _kv_size + 2 > _buf_capacity)
        LOG_ERRNO_RETURN(0, {}, "ran out of buffer");
    memcpy(_buf + _buf_size, "\r\n", 2);
    return {_buf, _buf_size + 2UL};
}
int RequestHeaders::redirect(Verb v, estring_view location, bool enable_proxy) {
    estring full_location;
    if (!location.starts_with(http_url_scheme) && (!location.starts_with(https_url_scheme))) {
        full_location.appends(secure() ? https_url_scheme : http_url_scheme,
                             host(), location);
        location = full_location;
    }
    StoredURL u(location);
    //Header Host按约定应当是第一组kv，找到Host_kv
    KV* host_kv = nullptr;
    int min = _buf_capacity;
    for (auto kv = kv_begin(); kv != kv_end(); kv++)
        if (kv->first.offset() < min) {
            host_kv = kv;
            min = kv->first.offset();
        }
    assert(host_kv != nullptr);
    //计算headers中不变部分的位移量，做memmove并维护kv索引
    auto new_prefix_size = verbstr[v].size() + sizeof(" HTTP/1.1\r\n")
                           + u.host_port().size() + 8;
    if (enable_proxy)
        new_prefix_size += full_url_size(u);
    else
        new_prefix_size += u.target().size();
    auto old_prefix_size = host_kv->second.offset() + host_kv->second.size() + 2;
    auto delta = new_prefix_size - old_prefix_size;
    auto final_size = _buf_size + delta;
    if (final_size + sizeof(KV) * _kv_size >= _buf_capacity)
        LOG_ERROR_RETURN(ENOBUFS, -1, "need more buffer");
    memmove(_buf + new_prefix_size, _buf + old_prefix_size, _buf_size - old_prefix_size);
    for (auto kv = kv_begin(); kv != kv_end(); kv++) {
        kv->first += delta;
        kv->second += delta;
    }

    //构造请求行以及第一组kv(即Host)
    _make_prefix(v, u, enable_proxy);
    _host = {uint16_t(_buf_size + 6), u.host_port().size()};
    *host_kv = {{_buf_size, 4}, _host};
    auto ptr = _buf + _buf_size;
    buf_append(ptr, "Host: ");
    buf_append(ptr, u.host_port());
    buf_append(ptr, "\r\n");
    _buf_size = final_size;
    return 0;
}

int ResponseHeaders::append_bytes(ISocketStream* s) {
    if (_status_code != 0) return 0;
    if (_buf_capacity - _buf_size <= _MAX_TRANSFER_BYTES + _RESERVED_INDEX_SIZE) {
        errno = ENOBUFS;
        return -1;
    }
    auto transfer_bytes = s->recv(_buf + _buf_size, _MAX_TRANSFER_BYTES);
    auto ret = (transfer_bytes < 0) ? transfer_bytes
                                    : append_bytes((uint16_t)transfer_bytes);
    if (ret != 0 && transfer_bytes == 0) 
        LOG_ERROR_RETURN(0, -1, "Peer closed before ")
    return ret;
}
int ResponseHeaders::append_bytes(uint16_t size)
{
    if (_status_code != 0) LOG_ERROR_RETURN(0, -1, "double parse");
    if(_buf_size + size >= (uint64_t)_buf_capacity)
        LOG_ERROR_RETURN(0, -1, "need more buffer");
    std::string_view sv(_buf + _buf_size, size);
    auto income = _buf + _buf_size;
    auto left = income - 3;
    if (left < _buf) left = _buf;
    _buf_size += size;
    std::string_view whole(left, _buf + _buf_size - left);
    auto pos = whole.find("\r\n\r\n");
    if (pos == whole.npos) return 1;

    _status_code = -1;
    pos += 4 - (income - left);
    auto body_begin = sv.begin() + pos - _buf;
    _body = {(uint16_t)body_begin, sv.size() - pos};
    Parser p({_buf, _buf_size});
    p.skip_string("HTTP/");
    _version = p.extract_until_char(' ');
    if (_version.size() >= 6) LOG_ERROR_RETURN(0, -1, "invalid scheme");
    p.skip_chars(' ');
    auto code = p.extract_integer();
    if (0 >= code || code >= 1000) LOG_ERROR_RETURN(0, -1, "invalid status code");
    _status_code = (uint16_t)code;
    p.skip_chars(' ');
    _status_message = p.extract_until_char('\r');
    p.skip_string("\r\n");

    while(p[0] != '\r')
    {
        auto k = p.extract_until_char(':');
        p.skip_string(": ");
        auto v = p.extract_until_char('\r');
        p.skip_string("\r\n");
        if (kv_add({k, v}) == nullptr) LOG_ERROR_RETURN(0, -1, "add kv failed");
    }
    std::sort(kv_begin(), kv_end(), [&](const KV& a, const KV& b){
        return (_buf | a.first) < (b.first | _buf);
    });
    return 0;
}

ssize_t ResponseHeaders::resource_size() const{
    std::string_view ret;
    auto content_range = (*this)["Content-Range"];
    if (content_range.empty()) {
        ret = (*this)["Content-Length"];
        if (ret.empty()) return -1;
    } else {
        auto lenstr = content_range.find_first_of('/');
        if (lenstr == std::string_view::npos) return -1;
        ret = content_range.substr(lenstr + 1);
        if (ret.find_first_of('*') != std::string_view::npos) return -1;
    }
    return estring_view(ret).to_uint64();
}

}
}
