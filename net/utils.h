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
#include <thread>
#include <type_traits>
#include <vector>

#include <photon/common/callback.h>
#include <photon/thread/thread11.h>
#include <photon/common/string_view.h>
#include <photon/net/socket.h>

namespace photon {
namespace net {

// user must make sure host or domain is a NULL TERMINATED c-string

/**
 * Get self-IPAddr by trying to access remote address
 * resolve procedure will block current thread.
 * Even if there is multiple network interfaces
 * it will get the one should be choosen to connect
 * to remote address.
 *
 * @param name target hostname when detecting
 * @return `IPAddr` of this host
 */
IPAddr gethostbypeer(IPAddr remote);

/**
 * Get self-IPAddr by trying to access remote address
 * resolve procedure will block current thread.
 * Even if there is multiple network interfaces
 * it will get the one should be choosen to connect
 * to remote address.
 * Take remote address as string, and will resolve
 * it then trying to detect.
 *
 * @param name target hostname when detecting
 * @return `IPAddr` of this host
 */
IPAddr gethostbypeer(const char* domain);

// Callback returns -1 means break

int _gethostbyname(const char* name, Callback<IPAddr> append_op);

// inline implemention for compatible

/**
 * Resolves hostname, get `IPAddr` results
 * returns resolve result.
 * resolve procedure will block current thread.
 *
 * @param name Host name to resolve
 * @return first resolved address.
 */
 inline IPAddr gethostbyname(const char* name) {
    IPAddr ret;
    auto cb = [&](IPAddr addr) {
        ret = addr;
        return -1;
    };
    _gethostbyname(name, cb);
    return ret;
}

/**
 * Resolves hostname, get `IPAddr` results
 * (Or single IPAddr ptr with bufsize=1 as default)
 * resolve procedure will block current thread.
 *
 *
 * @param name Host name to resolve
 * @param buf IPAddr buffer pointer
 * @param bufsize size of `buf`, takes `sizeof(IPAddr)` as unit
 * @return sum of resolved address number. result will be filled into `buf`
 */
inline int gethostbyname(const char* name, IPAddr* buf, int bufsize = 1) {
    int i = 0;
    auto cb = [&](IPAddr addr) {
        if (i < bufsize) buf[i++] = addr;
        return 0;
    };
    return _gethostbyname(name, cb);
}

/**
 * Resolves hostname, get `IPAddr` results
 * and fill result into vector.
 * resolve procedure will block current thread.
 *
 *
 * @param name Host name to resolve
 * @param ret `std::vector<IPAddr>` reference to get results
 * @return sum of resolved address number.
 */
inline int gethostbyname(const char* name, std::vector<IPAddr>& ret) {
    ret.clear();
    auto cb = [&](IPAddr addr) {
        ret.push_back(addr);
        return 0;
    };
    return _gethostbyname(name, cb);
}

/**
 * Resolves hostname, get `IPAddr` results
 * and fill result into vector
 * resolve procedure will running in another std::thread,
 * current photon thread will be blocked, but other photon threads will
 * still working.
 *
 * @param name Host name to resolve
 * @param ret `std::vector<IPAddr>` reference to get results
 * @return sum of resolved address number.
 */
inline int gethostbyname_nb(const char* name, std::vector<IPAddr>& ret) {
    photon::semaphore sem(0);
    int r = 0;
    ret.clear();
    std::thread([&] {
        r = gethostbyname(name, ret);
        sem.signal(1);
    }).detach();
    sem.wait(1);
    return r;
}

void Base64Encode(std::string_view in, std::string &out);
bool Base64Decode(std::string_view in, std::string &out);

/* Check if kernel version satisfies and thus zerocopy feature should be enabled */
bool zerocopy_available();

}  // namespace net
}
