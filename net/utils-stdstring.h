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

#include <string>
#include <photon/net/socket.h>
#include <photon/common/estring.h>

namespace photon {
namespace net {

inline void __to_string(const IPAddr& addr, char* text) {
    if (addr.is_ipv4()) {
        in_addr ip4;
        ip4.s_addr = addr.to_nl();
        inet_ntop(AF_INET, &ip4, text, INET_ADDRSTRLEN);
    } else {
        inet_ntop(AF_INET6, &addr, text, INET6_ADDRSTRLEN);
    }
}

inline std::string to_string(const IPAddr& addr) {
    char ip4or6[INET6_ADDRSTRLEN];
    __to_string(addr, ip4or6);
    return ip4or6;
}

inline estring to_string(const photon::net::EndPoint& ep) {
    char ip4or6[INET6_ADDRSTRLEN];
    __to_string(ep.addr, ip4or6);
    return ep.is_ipv4() ? estring().appends(ip4or6, ':', ep.port):
                          estring().appends('[', ip4or6, "]:", ep.port);
}

}
}