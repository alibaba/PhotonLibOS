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
#include <photon/common/utility.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>
#include "url.h"
#include "parser.h"

using namespace std;

namespace photon {
namespace net {
namespace http {

class HeaderAssistant {
public:
    const HeadersBase* _h;
    HeaderAssistant(const HeadersBase* h) : _h(h) {}

    int icmp(estring_view k1, estring_view k2) const {
        int r = (int)(k1.size() - k2.size());
        return r ? r : strncasecmp(k1.data(), k2.data(), k1.size());
    }

    bool equal_to(rstring_view16 _k, std::string_view key) const {
        return icmp(_k | _h->m_buf, key) == 0;
    }
    bool less(rstring_view16 _k1, rstring_view16 _k2) const {
        return icmp(_k1 | _h->m_buf, _k2 | _h->m_buf) < 0;
    }
    bool less(rstring_view16 _k, std::string_view key) const {
        return icmp(_k | _h->m_buf, key) < 0;
    }
    bool less(std::string_view key, rstring_view16 _k) const {
        return icmp(key, _k | _h->m_buf) < 0;
    }

    bool operator()(HeadersBase::KV a, std::string_view b) const { return less(a.first, b); }
    bool operator()(std::string_view a, HeadersBase::KV b) const { return less(a, b.first); }
    bool operator()(HeadersBase::KV a, HeadersBase::KV b) const { return less(a.first, b.first); }
};

using HA = HeaderAssistant;

HeadersBase::iterator HeadersBase::find(std::string_view key) const {
    auto it = std::lower_bound(kv_begin(), kv_end(), key, HA(this));
    if (it == kv_end() || !HA(this).equal_to(it->first, key)) return end();
    return {this, (uint16_t)(it - kv_begin())};
}

void buf_append(char*& ptr, std::string_view sv) {
    memcpy(ptr, sv.data(), sv.size());
    ptr += sv.size();
}

void buf_append(char*& ptr, uint64_t x) {
    auto begin = ptr;
    do {
        *ptr = '0' + x % 10;
        ptr++;
        x /= 10;
    } while(x);
    std::reverse(begin, ptr);
}

int HeadersBase::insert(std::string_view key, std::string_view value, int allow_dup) {
    if (!allow_dup) {
        auto it = find(key);
        if (it != end()) return -EEXIST;
    }
    uint64_t vbegin = m_buf_size + key.size() + 2;
    uint64_t new_size = vbegin + value.size() + 2;
    if (new_size + (m_kv_size + 1) * sizeof(KV) > m_buf_capacity)
        LOG_ERROR_RETURN(ENOBUFS, -1, "no buffer");
    auto ptr = m_buf + m_buf_size;
    buf_append(ptr, key);
    buf_append(ptr, ": ");
    buf_append(ptr, value);
    buf_append(ptr, "\r\n");
    m_last_kv = kv_add_sort({{m_buf_size, (uint16_t)key.size()},
            {(uint16_t)vbegin, (uint16_t)value.size()}}) - kv_begin();
    m_buf_size = (uint16_t)new_size;
    return 0;
}

bool HeadersBase::value_append(std::string_view value) {
    if (m_last_kv >= m_kv_size) return false;
    auto append_size =  value.size();
    uint64_t new_size = m_buf_size + append_size;
    if (new_size + m_kv_size * sizeof(KV) > m_buf_capacity)
        LOG_ERROR_RETURN(ENOBUFS, false, "no buffer");
    auto ptr = m_buf + m_buf_size - 2;
    buf_append(ptr, value);
    buf_append(ptr, "\r\n");
    m_buf_size = (uint16_t)new_size;
    kv_begin()[m_last_kv].second.size() += append_size;
    return true;
}

int HeadersBase::reset_host(int delta, std::string_view host) {
    KV* host_kv = nullptr;
    for (auto kv = kv_begin(); kv != kv_end(); kv++)
        if (kv->first.offset() == 0) {
            host_kv = kv;
            break;
        }
    assert(host_kv != nullptr);

    int inner_delta = host.size() - host_kv->second.size();
    if ((ssize_t)space_remain() < delta + inner_delta)
        LOG_ERROR_RETURN(ENOBUFS, -1, "no buffer");

    memmove(m_buf + delta + host.size() + 8,
            m_buf + host_kv->second.size() + 8,
            size() - host_kv->second.size() - 8);

    for (auto kv = kv_begin(); kv != kv_end(); kv++) {
        kv->first += inner_delta;
        kv->second += inner_delta;
    }
    *host_kv = {{0, 4}, {6, host.size()}};

    m_buf += delta;
    m_buf_capacity -= delta;
    m_buf_size += inner_delta;
    auto ptr = m_buf;
    buf_append(ptr, "Host: ");
    buf_append(ptr, host);
    buf_append(ptr, "\r\n");
    return 0;
}

HeadersBase::KV* HeadersBase::kv_add_sort(KV kv) {
    auto begin = kv_begin();
    if ((char*)(begin - 1) <= m_buf + m_buf_size)
        LOG_ERROR_RETURN(ENOBUFS, nullptr, "no buffer");
    auto it = std::lower_bound(begin, kv_end(), kv, HA(this));
    memmove(begin - 1, begin, sizeof(KV) * (it - begin));
    m_kv_size++;
    *(it - 1) = kv;
    return it - 1;
}

HeadersBase::KV* HeadersBase::kv_add(KV kv) {
    auto begin = kv_begin();
    if ((char*)(begin - 1) <= m_buf + m_buf_size)
        LOG_ERROR_RETURN(ENOBUFS, nullptr, "no buffer");
    m_kv_size++;
    *(begin - 1) = kv;
    return begin -1;
}

int HeadersBase::parse() {
    Parser p({m_buf, m_buf_size});
    while(p[0] != '\r') {
        auto k = p.extract_until_char(':');
        p.skip_string(": ");
        auto v = p.extract_until_char('\r');
        p.skip_string("\r\n");
        if (kv_add({k, v}) == nullptr)
            LOG_ERROR_RETURN(0, -1, "add kv failed");
    }
    std::sort(kv_begin(), kv_end(), HA(this));
    return 0;
}

std::pair<ssize_t, ssize_t> Headers::range() const {
    auto found = find("Range");
    if (found == end()) return {-1, -1};
    auto range = found.second();
    auto eq_pos = range.find("=");
    auto dash_pos = range.find("-");
    auto start_sv = estring_view(range.substr(eq_pos + 1, dash_pos - eq_pos + 1));
    auto end_sv = estring_view(range.substr(dash_pos + 1));
    auto start = start_sv.to_uint64();
    auto end = end_sv.to_uint64();
    if (start_sv.empty()) return {-1, end};
    if (end_sv.empty()) return {start, -1};
    return {start, end};
}

} // namespace http
} // namespace net
} // namespace photon
