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

namespace photon {
namespace net {
namespace http {

void URL::from_string(std::string_view url) {
    m_url = url.begin();
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
    p = url.find_first_of(":/");
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
        return;
    }
    if (url.front() == ':') {
        char* endp;
        auto port = strtol(url.begin() + 1, &endp, 10);
        auto port_len= endp - url.begin();
        if (endp != url.begin() + 1) {
            m_port = port;
            if (need_optional_port(*this)) m_host_port = rstring_view16(m_host.offset(), m_host.length() + port_len);
        }
        pos += port_len;
        url.remove_prefix(endp - url.begin());
    } else {
        m_port = m_secure ? 443 : 80;
    }
    if (url.empty()) {
        m_path = rstring_view16(pos, 0);
        m_query = rstring_view16(0, 0);
        m_target = m_path;
        return;
    }
    p = url.find_first_of("?");
    if (p == url.npos) {
        m_path = rstring_view16(pos, url.size());
        m_query = rstring_view16(0, 0);
        m_target = m_path;
        return;
    }
    m_path = rstring_view16(pos, p);
    m_target = rstring_view16(pos, url.size());
    pos += p+1;
    m_query = rstring_view16(pos, url.size() - (p + 1));
}

} // namespace http
} // namespace net
} // namespace photon