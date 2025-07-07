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

#include <photon/common/conststr.h>
#include <photon/common/string_view.h>
#include <photon/common/utility.h>

namespace photon {
namespace net {
namespace http {

const static auto code_str = ConstString::make_compact_str_array<uint16_t>(
    /*100*/ TSTRING("Continue"),
    /*101*/ TSTRING("Switching Protocols"),
    /*102*/ TSTRING("Processing"),
    /*103*/ TSTRING("Early Hints"),

    /*200*/ TSTRING("OK"),
    /*201*/ TSTRING("Created"),
    /*202*/ TSTRING("Accepted"),
    /*203*/ TSTRING("Non-Authoritative Information"),
    /*204*/ TSTRING("No Content"),
    /*205*/ TSTRING("Reset Content"),
    /*206*/ TSTRING("Partial Content"),
    /*207*/ TSTRING("Multi-Status"),
    /*208*/ TSTRING("Already Reported"),
    //	/*226*/ TSTRING("IM Used"),

    /*300*/ TSTRING("Multiple Choices"),
    /*301*/ TSTRING("Moved Permanently"),
    /*302*/ TSTRING("Found"),
    /*303*/ TSTRING("See Other"),
    /*304*/ TSTRING("Not Modified"),
    /*305*/ TSTRING("Use Proxy"),
    /*306*/ TSTRING(""),
    /*307*/ TSTRING("Temporary Redirect"),
    /*308*/ TSTRING("Permanent Redirect"),

    /*400*/ TSTRING("Bad Request"),
    /*401*/ TSTRING("Unauthorized"),
    /*402*/ TSTRING("Payment Required"),
    /*403*/ TSTRING("Forbidden"),
    /*404*/ TSTRING("Not Found"),
    /*405*/ TSTRING("Method Not Allowed"),
    /*406*/ TSTRING("Not Acceptable"),
    /*407*/ TSTRING("Proxy Authentication Required"),
    /*408*/ TSTRING("Request Timeout"),
    /*409*/ TSTRING("Conflict"),
    /*410*/ TSTRING("Gone"),
    /*411*/ TSTRING("Length Required"),
    /*412*/ TSTRING("Precondition Failed"),
    /*413*/ TSTRING("Content Too Large"),
    /*414*/ TSTRING("URI Too Long"),
    /*415*/ TSTRING("Unsupported Media Type"),
    /*416*/ TSTRING("Range Not Satisfiable"),
    /*417*/ TSTRING("Expectation Failed"),
    /*418*/ TSTRING("I'm a teapot"),
    /*419*/ TSTRING(""),
    /*420*/ TSTRING(""),
    /*421*/ TSTRING("Misdirected Request"),
    /*422*/ TSTRING("Unprocessable Content"),
    /*423*/ TSTRING("Locked"),
    /*424*/ TSTRING("Failed Dependency"),
    /*425*/ TSTRING("Too Early"),
    /*426*/ TSTRING("Upgrade Required"),
    /*427*/ TSTRING(""),
    /*428*/ TSTRING("Precondition Required"),
    /*429*/ TSTRING("Too Many Requests"),
    /*430*/ TSTRING(""),
    /*431*/ TSTRING("Request Header Fields Too Large"),
    //	/*451*/ TSTRING("Unavailable For Legal Reasons"),

    /*500*/ TSTRING("Internal Server Error"),
    /*501*/ TSTRING("Not Implemented"),
    /*502*/ TSTRING("Bad Gateway"),
    /*503*/ TSTRING("Service Unavailable"),
    /*504*/ TSTRING("Gateway Timeout"),
    /*505*/ TSTRING("HTTP Version Not Supported"),
    /*506*/ TSTRING("Variant Also Negotiates"),
    /*507*/ TSTRING("Insufficient Storage"),
    /*508*/ TSTRING("Loop Detected"),
    /*509*/ TSTRING(""),
    /*510*/ TSTRING("Not Extended"),
    /*511*/ TSTRING("Network Authentication Required"));

std::string_view obsolete_reason(int code) {
    uint8_t major = code / 100 - 1;
    uint8_t minor = code % 100;
    constexpr static auto entries = ConstString::accumulate_helper(
        ConstString::TList<uint8_t, 4, 9, 9, 32, 12>());
    if (unlikely(major >= entries.size())) return {};
    auto i = entries.arr[major] + minor;
    if (unlikely(i >= entries.arr[major + 1])) return {};
    return code_str[i];
}

}  // namespace http
}  // namespace net
}  // namespace photon
