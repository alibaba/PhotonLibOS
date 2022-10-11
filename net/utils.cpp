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

#include "utils.h"

#include <inttypes.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <thread>
#include <vector>

#include <photon/common/alog.h>
#include <photon/io/fd-events.h>
#include <photon/io/signal.h>
#include <photon/thread/thread11.h>
#include <photon/common/utility.h>
#include "socket.h"

namespace photon {
namespace net {

IPAddr gethostbypeer(IPAddr remote) {
    // detect ip for itself,
    // by trying to "connect" remote udp socket
    // this will not connect or send out any datagram
    // but let os select the interface to connect,
    // then get its ip
    constexpr uint16_t UDP_IP_DETECTE_PORT = 8080;

    int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) LOG_ERRNO_RETURN(0, IPAddr(), "Cannot create udp socket");
    DEFER(::close(sockfd));
    struct sockaddr_in addr_in =
        EndPoint{remote, UDP_IP_DETECTE_PORT}.to_sockaddr_in();
    auto ret =
        ::connect(sockfd, (sockaddr *)&addr_in, sizeof(struct sockaddr_in));
    if (ret < 0) LOG_ERRNO_RETURN(0, IPAddr(), "Cannot connect remote");
    struct sockaddr_in addr_local;
    socklen_t len = sizeof(struct sockaddr_in);
    ::getsockname(sockfd, (sockaddr *)&addr_local, &len);
    IPAddr result;
    result.from_nl(addr_local.sin_addr.s_addr);
    return result;
}

IPAddr gethostbypeer(const char *domain) {
    // get self ip by remote domain instead of ip
    IPAddr remote;
    auto ret = gethostbyname(domain, &remote);
    if (ret < 0) return IPAddr();
    LOG_DEBUG("Resolved remote host ip ", VALUE(remote));
    return gethostbypeer(remote);
}

int _gethostbyname(const char *name, Delegate<int, IPAddr> append_op) {
    struct hostent *ent = nullptr;
#ifdef __APPLE__
    ent = ::gethostbyname(name);
#else
    int err;
    struct hostent hbuf;
    char buf[32 * 1024];

    // only retval 0 means successful
    if (::gethostbyname_r(name, &hbuf, buf, sizeof(buf), &ent, &err) != 0)
        LOG_ERRNO_RETURN(0, -1, "Failed to gethostbyname", VALUE(err));
#endif
    int idx = 0;
    if (ent && ent->h_addrtype == AF_INET) {
        // can only support IPv4
        auto addrlist = (struct in_addr **)ent->h_addr_list;
        while (*addrlist != nullptr) {
            if (append_op(IPAddr((*addrlist)->s_addr)) < 0) break;
            idx++;
            addrlist++;
        }
    }
    return idx;
}

inline __attribute__((always_inline)) void base64_translate_3to4(const char *in, char *out)  {
    struct xlator {
        unsigned char _;
        unsigned char a : 6;
        unsigned char b : 6;
        unsigned char c : 6;
        unsigned char d : 6;
    } __attribute__((packed));
    static_assert(sizeof(xlator) == 4, "...");
    static const unsigned char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto v = htonl(*(uint32_t *)in);
    auto x = *(xlator *)(&v);
    *(uint32_t *)out = ((tbl[x.a] << 24) + (tbl[x.b] << 16) +
                        (tbl[x.c] << 8) + (tbl[x.d] << 0));
}
void Base64Encode(std::string_view in, std::string &out) {
    auto main = in.size() / 3;
    auto remain = in.size() % 3;
    if (0 == remain) {
        remain = 3;
        main--;
    }
    auto out_size = (main + 1) * 4;
    out.resize(out_size);
    auto _in = &in[0];
    auto _out = &out[0];
    auto end = _in + main * 3;

    for (; _in + 3 * 4 < end; _in += 3 * 4, _out += 4 * 4) {
        base64_translate_3to4(_in, _out);
        base64_translate_3to4(_in + 3, _out + 4);
        base64_translate_3to4(_in + 6, _out + 8);
        base64_translate_3to4(_in + 9, _out + 12);
    }


    for (; _in < end; _in += 3, _out += 4) {
        base64_translate_3to4(_in, _out);
    }

    char itail[4];
    itail[3] = 0;
    itail[0] = _in[0];
    if (remain == 2) {
        itail[1] = _in[1];
    } else if (remain == 3) {
        *(short *)&itail[1] = *(short *)&_in[1];
    }
    base64_translate_3to4(itail, _out);
    for (size_t i = 0; i < (3 - remain); ++i) out[out_size - i - 1] = '=';
}

static bool do_zerocopy_available() {
    utsname buf = {};
    uname(&buf);
    std::string kernel_release(buf.release);
    std::string kernel_version = kernel_release.substr(0, kernel_release.find('-'));
    int result = 0;
    int ret = Utility::version_compare(kernel_version, "4.15", result);
    if (ret != 0) {
        LOG_ERROR_RETURN(0, false, "Unable to detect kernel version, not using zero-copy");
    }
    return result >= 0;
}

bool zerocopy_available() {
    static bool r = do_zerocopy_available();
    return r;
}

}  // namespace net
}
