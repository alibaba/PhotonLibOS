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

namespace photon {
namespace net {

inline std::string to_string(const photon::net::IPAddr& addr) {
    std::string str;
    char text[INET6_ADDRSTRLEN];
    if (addr.is_ipv4()) {
        in_addr ip4;
        ip4.s_addr = addr.to_nl();
        inet_ntop(AF_INET, &ip4, text, INET_ADDRSTRLEN);
    } else {
        inet_ntop(AF_INET6, &addr, text, INET6_ADDRSTRLEN);
    }
    str.assign(text, strlen(text));
    return str;
}

inline std::string to_string(const photon::net::EndPoint& ep) {
    if (ep.is_ipv4()) {
        return to_string(ep.addr) + ":" + std::to_string(ep.port);
    } else {
        return "[" + to_string(ep.addr) + "]:" + std::to_string(ep.port);
    }
}

}
}