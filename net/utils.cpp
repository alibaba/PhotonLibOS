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

#include <chrono>
#include <list>
#include <thread>
#include <string>

#include <photon/common/alog.h>
#include <photon/thread/thread11.h>
#include <photon/common/utility.h>
#include <photon/common/expirecontainer.h>
#include "socket.h"
#include "base_socket.h"

namespace photon {
namespace net {

IPAddr gethostbypeer(IPAddr remote) {
    // detect ip for itself,
    // by trying to "connect" remote udp socket
    // this will not connect or send out any datagram
    // but let os select the interface to connect,
    // then get its ip
    constexpr uint16_t UDP_IP_DETECTE_PORT = 8080;
    int sock_family = remote.is_ipv4() ? AF_INET : AF_INET6;

    int sockfd = ::socket(sock_family, SOCK_DGRAM, 0);
    if (sockfd < 0) LOG_ERRNO_RETURN(0, IPAddr(), "Cannot create udp socket");
    DEFER(::close(sockfd));

    EndPoint ep_remote(remote, UDP_IP_DETECTE_PORT);
    sockaddr_storage s_remote(ep_remote);

    auto ret = ::connect(sockfd, s_remote.get_sockaddr(), s_remote.get_socklen());
    if (ret < 0) LOG_ERRNO_RETURN(0, IPAddr(), "Cannot connect remote");

    sockaddr_storage s_local;
    socklen_t len = s_local.get_max_socklen();
    ::getsockname(sockfd, s_local.get_sockaddr(), &len);

    return s_local.to_endpoint().addr;
}

IPAddr gethostbypeer(const char *domain) {
    // get self ip by remote domain instead of ip
    IPAddr remote;
    auto ret = gethostbyname(domain, &remote);
    if (ret < 0) return IPAddr();
    LOG_DEBUG("Resolved remote host ip ", VALUE(remote));
    return gethostbypeer(remote);
}

int _gethostbyname(const char* name, Delegate<int, IPAddr> append_op) {
    assert(name);
    int idx = 0;
    addrinfo* result = nullptr;
    addrinfo hints = {};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    int ret = getaddrinfo(name, nullptr, &hints, &result);
    if (ret != 0) {
        LOG_ERROR_RETURN(0, -1, "Fail to getaddrinfo: `", gai_strerror(ret));
    }
    assert(result);
    for (auto* cur = result; cur != nullptr; cur = cur->ai_next) {
        if (cur->ai_family == AF_INET6) {
            auto sock_addr = (sockaddr_in6*) cur->ai_addr;
            if (append_op(IPAddr(sock_addr->sin6_addr)) < 0) {
                break;
            }
            idx++;
        } else if (cur->ai_family == AF_INET) {
            auto sock_addr = (sockaddr_in*) cur->ai_addr;
            if (append_op(IPAddr(sock_addr->sin_addr)) < 0) {
                break;
            }
            idx++;
        }
    }
    freeaddrinfo(result);
    return idx;
}

struct xlator {
    unsigned char _;
    unsigned char a : 6;
    unsigned char b : 6;
    unsigned char c : 6;
    unsigned char d : 6;
} __attribute__((packed));
static_assert(sizeof(xlator) == 4, "...");

inline __attribute__((always_inline))
void base64_translate_3to4(const char *in, char *out)  {
    static const unsigned char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto v = htonl(*(uint32_t *)in);
    auto x = (xlator*) &v;
    *(uint32_t *)out = ((tbl[x->a] << 24) + (tbl[x->b] << 16) +
                        (tbl[x->c] << 8) + (tbl[x->d] << 0));
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

    char itail[4] = {0};
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
    int result = 0;
    int ret = kernel_version_compare("4.15", result);
    if (ret != 0) return false;
    return result >= 0;
}

bool zerocopy_available() {
    static bool r = do_zerocopy_available();
    return r;
}


static const char base64_index_min = '+';
static const char base64_index_max = 'z';
#define EI 255
static unsigned char base64_index_map[]= {
    //'+', ',', '-', '.', '/', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?', '@',
       62,  EI,  EI,  EI,  63,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  EI,  EI,  EI,  EI,  EI,  EI,  EI,
    //'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
        0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    //'[', '\', ']', '^', '_', '`',
        EI, EI,  EI,  EI,  EI,  EI,
    //abcdefghijklmnopqrstuvwxyz
        26, 27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37, 38, 39, 40, 41, 42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52 };

static unsigned char get_index_of(char val, bool &ok) {
    ok = true;
    if (val < base64_index_min || val > base64_index_max) {
        ok = false;
        return 0;
    }
     unsigned char ret = base64_index_map[val - base64_index_min];
     ok = (ret != EI);
     return ret;
}
 #undef EI

inline
bool base64_translate_4to3(const char *in, char *out)  {
    xlator v;
    bool f1, f2, f3, f4;
    v.a = get_index_of(*(in+3), f1);
    v.b = get_index_of(*(in+2), f2);
    v.c = get_index_of(*(in+1), f3);
    v.d = get_index_of(*(in),   f4);

    *(uint32_t *)out = ntohl(*(uint32_t *)&v);
    return (f1 && f2 && f3 && f4);
}

bool Base64Decode(std::string_view in, std::string &out) {
#define GSIZE 4 //Size of each group
    auto in_size = in.size();
    if (in_size == 0 || in_size % GSIZE != 0) {
        return false;
    }

    char in_tail[GSIZE];
    int pad = 0;
    if (in[in_size - 1] == '=') {
        memcpy(in_tail, &(in[in_size - GSIZE]), GSIZE);
        in_tail[GSIZE-1] = 'A';
        pad = 1;

        if (in[in_size - 2] == '='){
            in_tail[GSIZE-2] = 'A';
            pad = 2;
        }
    }
    auto out_size = (in_size/GSIZE ) * 3;
    out.resize(out_size);

    auto _in = &in[0];
    auto _out = &out[0];
    auto end = _in + (in_size - pad);
    for (; _in + GSIZE <= end; _in += GSIZE, _out += 3 ) {
        if (!base64_translate_4to3(_in, _out)) {
            return false;
        }
    }

    if (!pad) {
        return true;
    }
    if (!base64_translate_4to3(in_tail, _out)) {
        return false;
    }
    out.resize(out_size - pad);
    return true;
#undef BUNIT
}

class DefaultResolver : public Resolver {
protected:
    struct IPAddrNode : public intrusive_list_node<IPAddrNode> {
        IPAddr addr;
        IPAddrNode(IPAddr addr) : addr(addr) {}
    };
    using IPAddrList = intrusive_list<IPAddrNode>;
public:
    DefaultResolver(uint64_t cache_ttl, uint64_t resolve_timeout)
        : dnscache_(cache_ttl), resolve_timeout_(resolve_timeout) {}
    ~DefaultResolver() {
        for (auto it : dnscache_) {
            ((IPAddrList*)it->_obj)->delete_all();
        }
        dnscache_.clear();
    }

    IPAddr resolve(const char *host) override {
        auto ctr = [&]() -> IPAddrList* {
            auto addrs = new IPAddrList();
            photon::semaphore sem;
            std::thread([&]() {
                auto now = std::chrono::steady_clock::now();
                IPAddrList ret;
                auto cb = [&](IPAddr addr) {
                    ret.push_back(new IPAddrNode(addr));
                    return 0;
                };
                _gethostbyname(host, cb);
                auto time_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - now).count();
                if ((uint64_t)time_elapsed <= resolve_timeout_) {
                    addrs->push_back(std::move(ret));
                    sem.signal(1);
                }
            }).detach();
            sem.wait(1, resolve_timeout_);
            return addrs;
        };
        auto ips = dnscache_.borrow(host, ctr);
        if (ips->empty()) LOG_ERRNO_RETURN(0, IPAddr(), "Domain resolution for ` failed", host);
        auto ret = ips->front();
        ips->node = ret->next();  // access in round robin order
        return ret->addr;
    }

    void resolve(const char *host, Delegate<void, IPAddr> func) override { func(resolve(host)); }

    void discard_cache(const char *host, IPAddr ip) override {
        auto ipaddr = dnscache_.borrow(host);
        if (ip.undefined() || ipaddr->empty()) ipaddr.recycle(true);
        else {
            for (auto itr = ipaddr->rbegin(); itr != ipaddr->rend(); itr++) {
                if ((*itr)->addr == ip) {
                    ipaddr->erase(*itr);
                    break;
                }
            }
        }
    }

private:
    ObjectCache<std::string, IPAddrList*> dnscache_;
    uint64_t resolve_timeout_;
};

Resolver* new_default_resolver(uint64_t cache_ttl, uint64_t resolve_timeout) {
    return new DefaultResolver(cache_ttl, resolve_timeout);
}

}  // namespace net
}
