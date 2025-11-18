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

void URL::from_string(std::string_view url) {
    if (m_tmp_target) {
        free((void*)m_tmp_target);
        m_tmp_target = nullptr;
    }
    m_url = url.data();
    size_t pos = 0;
    LOG_DEBUG(VALUE(url));
    m_secure = ((estring_view&) (url)).starts_with(https_url_scheme);
    // hashtag should be dropped, since it is pure client-side mark
    url = url.substr(0, url.find_first_of('#'));
    auto p = url.find("://");
    if (p != url.npos) {
        pos += p + 3;
        url.remove_prefix(p + 3);
    }
    p = url.find_first_of(":/?");
    if (p == url.npos) p = url.size();
    m_host = rstring_view16(pos, p);
    m_host_port = m_host;
    pos += p;
    url.remove_prefix(p);
    if (url.empty()) {
        m_port = m_secure ? 443 : 80;
        m_path = rstring_view16(pos, 0);
        m_query = rstring_view16(0, 0);
        m_target = m_path;
    } else if (url.front() == ':') {
        url.remove_prefix(1); // Skip ':'
        // Parse port number, find the end of digit sequence
        size_t port_len = 0;
        while (port_len < url.size() && url[port_len] >= '0' && url[port_len] <= '9') {
            port_len++;
        }
        if (port_len > 0) {
            uint64_t port_val = 0;
            estring_view port_str(url.data(), port_len);
            if (port_str.to_uint64_check(&port_val)) {
                m_port = static_cast<uint16_t>(port_val);
            } else {
                m_port = m_secure ? 443 : 80;
            }
            if (need_optional_port(*this)) m_host_port = rstring_view16(m_host.offset(), m_host.length() + port_len + 1);
        } else {
            m_port = m_secure ? 443 : 80;
        }
        pos += port_len + 1; // +1 for ':'
        url.remove_prefix(port_len);
        
        if (url.empty()) {
            m_path = rstring_view16(pos, 0);
            m_query = rstring_view16(0, 0);
            m_target = m_path;
        } else {
            p = url.find_first_of("?");
            if (p == url.npos) {
                m_path = rstring_view16(pos, url.size());
                m_query = rstring_view16(0, 0);
                m_target = m_path;
            } else {
                m_path = rstring_view16(pos, p);
                m_target = rstring_view16(pos, url.size());
                pos += p+1;
                auto query_len = sat_sub(url.size(), p + 1);
                m_query = rstring_view16(pos, query_len);
            }
        }
    } else {
        m_port = m_secure ? 443 : 80;
        p = url.find_first_of("?");
        if (p == url.npos) {
            m_path = rstring_view16(pos, url.size());
            m_query = rstring_view16(0, 0);
            m_target = m_path;
        } else {
            m_path = rstring_view16(pos, p);
            m_target = rstring_view16(pos, url.size());
            pos += p+1;
            auto query_len = sat_sub(url.size(), p + 1);
            m_query = rstring_view16(pos, query_len);
        }
    }
    
    // Fix target if needed (was previously a separate fix_target() function)
    estring_view t = m_url | m_target;
    if (t.size() == 0 || t.front() != '/') {
        m_tmp_target = (char*)malloc(t.size() + 2);
        m_tmp_target[0] = '/';
        memcpy(m_tmp_target+1, t.data(), t.size());
        m_tmp_target[t.size() + 1] = '\0';
        m_target = rstring_view16(0, t.size()+1);
        m_path = rstring_view16(0, m_path.size()+1);
    }
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