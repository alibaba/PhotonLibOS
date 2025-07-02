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

#include <array>
#include "photon/common/utility.h"
#include "../../common/static-strings.h"
#include "photon/common/string_view.h"
#include "message.h"

namespace photon {
namespace net {
namespace http {

constexpr static std::string_view code_str[] = {
    /*100*/ "Continue",
    /*101*/ "Switching Protocols",
    /*102*/ "Processing",
    /*103*/ "Early Hints",

    /*200*/ "OK",
    /*201*/ "Created",
    /*202*/ "Accepted",
    /*203*/ "Non-Authoritative Information",
    /*204*/ "No Content",
    /*205*/ "Reset Content",
    /*206*/ "Partial Content",
    /*207*/ "Multi-Status",
    /*208*/ "Already Reported",
//	/*226*/ "IM Used",

    /*300*/ "Multiple Choices",
    /*301*/ "Moved Permanently",
    /*302*/ "Found",
    /*303*/ "See Other",
    /*304*/ "Not Modified",
    /*305*/ "Use Proxy",
    /*306*/ {0, 0},
    /*307*/ "Temporary Redirect",
    /*308*/ "Permanent Redirect",

    /*400*/ "Bad Request",
    /*401*/ "Unauthorized",
    /*402*/ "Payment Required",
    /*403*/ "Forbidden",
    /*404*/ "Not Found",
    /*405*/ "Method Not Allowed",
    /*406*/ "Not Acceptable",
    /*407*/ "Proxy Authentication Required",
    /*408*/ "Request Timeout",
    /*409*/ "Conflict",
    /*410*/ "Gone",
    /*411*/ "Length Required",
    /*412*/ "Precondition Failed",
    /*413*/ "Content Too Large",
    /*414*/ "URI Too Long",
    /*415*/ "Unsupported Media Type",
    /*416*/ "Range Not Satisfiable",
    /*417*/ "Expectation Failed",
    /*418*/ "I'm a teapot",
    /*419*/ {0, 0},
    /*420*/ {0, 0},
    /*421*/ "Misdirected Request",
    /*422*/ "Unprocessable Content",
    /*423*/ "Locked",
    /*424*/ "Failed Dependency",
    /*425*/ "Too Early",
    /*426*/ "Upgrade Required",
    /*427*/ {0, 0},
    /*428*/ "Precondition Required",
    /*429*/ "Too Many Requests",
    /*430*/ {0, 0},
    /*431*/ "Request Header Fields Too Large",
//	/*451*/ "Unavailable For Legal Reasons",

    /*500*/ "Internal Server Error",
    /*501*/ "Not Implemented",
    /*502*/ "Bad Gateway",
    /*503*/ "Service Unavailable",
    /*504*/ "Gateway Timeout",
    /*505*/ "HTTP Version Not Supported",
    /*506*/ "Variant Also Negotiates",
    /*507*/ "Insufficient Storage",
    /*508*/ "Loop Detected",
    /*509*/ {0, 0},
    /*510*/ "Not Extended",
    /*511*/ "Network Authentication Required",
};

template<typename P, typename...Ts> inline
constexpr auto static_accumulate(Ts...xs) {
    std::array<P, sizeof...(xs) + 1> a;
    a[0] = 0;
    auto acc = [i = 0, &a](auto const& x) mutable {
        a[i+1] = a[i] + x;
        i++;
    };
    (acc(xs), ...);
    return a;
}

std::string_view obsolete_reason(int code) {
    uint8_t major = code / 100 - 1;
    uint8_t minor = code % 100;
    const static auto entries = static_accumulate<uint8_t>(4, 9, 9, 32, 12);
    if (unlikely(major >= entries.size())) return {};
    auto i = entries[major] + minor;
    if (unlikely(i >= entries[major+1])) return {};
    const static auto sss = SSS_CONSTRUCT(code_str);
    return sss[i];
}

}
}
}
