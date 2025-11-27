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

#include "url.h"
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/utility.h>

namespace photon {
namespace net {
namespace http {

bool URL::from_string(std::string_view url_) {
    estring_view url(url_); // [http[s]://]domain.or.ip[:port][/[path[?query]]]
    m_url = url.data();     // target := /path?query
    LOG_DEBUG(VALUE(url));  // target and path both start with '/

    free(m_tmp_target);
    m_tmp_target = nullptr;
    DEFER({ // fix the problem that path and target do not start with '/'
        estring_view t = m_url | m_target;
        if (t.size() == 0 || t.front() != '/') {
            m_tmp_target = (char*)malloc(t.size() + 2);
            m_tmp_target[0] = '/';
            memcpy(m_tmp_target+1, t.data(), t.size());
            m_tmp_target[t.size() + 1] = '\0';
            m_target = rstring_view16(0, t.size()+1);
            m_path = rstring_view16(0, m_path.size()+1);
        }
    });

    m_port = 80;
    m_secure = false;
    size_t p = url.find("://");
    if (p != url.npos) {
        auto protocol = url.substr(0, p);
        if (protocol.icmp("http") == 0) { }
        else if (protocol.icmp("https") == 0) { m_port = 443; m_secure = true; }
        else LOG_ERROR_RETURN(EINVAL, false, "invalid protocol: ", protocol);
    }

    url.remove_prefix(p + 3);
    // hashtag should be dropped, since it is pure client-side mark
    url = url.substr(0, url.find_first_of('#'));

    m_path = m_query = m_target = { };
    p = url.find_first_of(":/?");
    uint64_t offset = url.data() - m_url;
    if (p == url.npos) {
        m_host = m_host_port = {offset, url.size()};
        return true;
    }
    m_host = m_host_port = {offset, p};
    url.remove_prefix(p);
    if (url.empty()) return true;
    if (url.front() == ':') {
        url.remove_prefix(1); // Skip ':'
        uint64_t port_val = -1UL, port_len = url.to_uint64_check(&port_val);
        if (!port_len || port_val >= 65536)
            LOG_ERROR_RETURN(EINVAL, false, "invalid port ", url)
        m_port = static_cast<uint16_t>(port_val);
        if (need_optional_port(*this))
            m_host_port.length() += 1 + port_len;
        url.remove_prefix(port_len);
    }

    // Parse path and query
    if (url.empty()) return true;
    p = url.find_first_of("?");
    offset = url.data() - m_url;
    m_target = {offset, url.size()};
    if (p == url.npos) {
        m_path = m_target;
        return true;
    }
    m_path = {offset, p};
    auto query_len = sat_sub(url.size(), p + 1);
    m_query = {offset + p + 1, query_len};
    return true;
}

static bool isunreserved(char c) {
    if (c >= '0' && c <= '9') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c == '-' || c == '.' || c == '_' || c == '~') return true;
    return false;
}

std::string url_escape(std::string_view url, bool escape_slash) {
    static const char hex[] = "0123456789ABCDEF";
    std::string ret;
    ret.reserve(url.size() * 2);

    for (unsigned char c : url) {
        if (isunreserved(c)) {
            ret.push_back(c);
        } else if (!escape_slash && c == '/') {
            ret.push_back(c);
        } else {
            ret += '%';
            ret += hex[c >> 4];
            ret += hex[c & 0xf];
        }
    }
    return ret;
}

std::string url_unescape(std::string_view url) {
    std::string ret;
    ret.reserve(url.size());
    for (unsigned int i = 0; i < url.size(); i++) {
        if (url[i] == '%' && i + 2 < url.size()) {
            auto c = estring_view(url.substr(i + 1, 2)).hex_to_uint64();
            ret += static_cast<char>(c);
            i += 2;
        } else if (url[i] == '+')
            ret += ' ';
        else
            ret += url[i];
    }
    return ret;
}

} // namespace http
} // namespace net
} // namespace photon