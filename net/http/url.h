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
#include <photon/common/string_view.h>
#include <photon/common/estring.h>

namespace photon {
namespace net {
namespace http {

constexpr char http_url_scheme[] = "http://";
constexpr char https_url_scheme[] = "https://";

inline std::string_view http_or_s(bool cond){
    return cond ?
        std::string_view(http_url_scheme) :
        std::string_view(https_url_scheme);
}

inline int what_protocol(estring_view url) {
    if (url.istarts_with(http_url_scheme)) return 1;
    if (url.istarts_with(https_url_scheme)) return 2;
    return 0;
}

class URL {
protected:
    const char* m_url = nullptr;
    rstring_view16 m_host;
    rstring_view16 m_host_port;
    rstring_view16 m_path;
    rstring_view16 m_query;
    rstring_view16 m_target;
    rstring_view16 m_fragment;
    uint16_t m_port = 0;
    bool m_secure;
    char *m_tmp_target = nullptr;
public:
    URL() = default;
    URL(std::string_view url) { from_string(url); }
    ~URL() {
        free((void*)m_tmp_target);
    }

    std::string to_string() {
        return m_url;
    }

    const char* c_str() {
        return m_url;
    }

    void from_string(std::string_view url);
    void fix_target();
    std::string_view query() const { return m_url | m_query; }

    std::string_view path() const {
        if (m_tmp_target != nullptr)
            return (m_tmp_target | m_path);
        return (m_url | m_path);
    }

    std::string_view target() const {
        if (m_tmp_target != nullptr)
            return (m_tmp_target | m_target);
        return (m_url | m_target);
    }

    std::string_view host() const { return m_url | m_host; }
    std::string_view host_no_port() const { return host(); }
    std::string_view host_port() const { return m_url | m_host_port;}
    uint16_t port() const { return m_port; }
    bool secure() const { return m_secure; }
    bool empty() const { return !m_url; }
};
class StoredURL : public URL {
public:
    StoredURL() = default;
    StoredURL(std::string_view url) { from_string(url); }
    void from_string(std::string_view url) {
        if (m_url) {
            free((void*)m_url);
        }
        auto u = strndup(url.data(), url.size());
        URL::from_string({u, url.size()});
    }
    ~StoredURL() {
        free((void*)m_url);
    }
};

inline bool need_optional_port(const URL& u) {
    if (u.secure() && u.port() != 443) return true;
    if (!u.secure() && u.port() != 80) return true;
    return false;
}

std::string url_escape(std::string_view url);
std::string url_unescape(std::string_view url);

} // namespace http
} // namespace net
} // namespace photon
