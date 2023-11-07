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

#include "photon/common/string_view.h"
#include "message.h"

struct SV : public std::string_view {
	template<size_t N>
	constexpr SV(const char(&s)[N]) : SV(s, N) { }
	constexpr SV(const char* s, size_t n) : std::string_view(s, n) { }
};

constexpr static SV code_str[] = {
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

const static uint8_t LEN1xx = 4;
const static uint8_t LEN2xx = 9 + LEN1xx;
const static uint8_t LEN3xx = 9 + LEN2xx;
const static uint8_t LEN4xx = 32 + LEN3xx;
const static uint8_t LEN5xx = 12 + LEN4xx;
uint8_t entries[] = {0, LEN1xx, LEN2xx, LEN3xx, LEN4xx, LEN5xx};

std::string_view photon::net::http::obsolete_reason(int code) {
	uint8_t major = code / 100 - 1;
	uint8_t minor = code % 100;
	if (unlikely(major > 4)) return {};
	uint8_t max = entries[major+1] - entries[major];
	if (unlikely(minor >= max)) return {};
	return code_str[entries[major] + minor];

/*
	switch (code){

	case 100: return "Continue";
	case 101: return "Switching Protocols";
	case 102: return "Processing";
	case 103: return "Early Hints";

	case 200: return "OK";
	case 201: return "Created";
	case 202: return "Accepted";
	case 203: return "Non-Authoritative Information";
	case 204: return "No Content";
	case 205: return "Reset Content";
	case 206: return "Partial Content";
	case 207: return "Multi-Status";
	case 208: return "Already Reported";
	case 226: return "IM Used";

	case 300: return "Multiple Choices";
	case 301: return "Moved Permanently";
	case 302: return "Found";
	case 303: return "See Other";
	case 304: return "Not Modified";
	case 305: return "Use Proxy";
	case 307: return "Temporary Redirect";
	case 308: return "Permanent Redirect";

	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 402: return "Payment Required";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 406: return "Not Acceptable";
	case 407: return "Proxy Authentication Required";
	case 408: return "Request Timeout";
	case 409: return "Conflict";
	case 410: return "Gone";
	case 411: return "Length Required";
	case 412: return "Precondition Failed";
	case 413: return "Content Too Large";
	case 414: return "URI Too Long";
	case 415: return "Unsupported Media Type";
	case 416: return "Range Not Satisfiable";
	case 417: return "Expectation Failed";
	case 418: return "I'm a teapot";
	case 421: return "Misdirected Request";
	case 422: return "Unprocessable Content";
	case 423: return "Locked";
	case 424: return "Failed Dependency";
	case 425: return "Too Early";
	case 426: return "Upgrade Required";
	case 428: return "Precondition Required";
	case 429: return "Too Many Requests";
	case 431: return "Request Header Fields Too Large";
	case 451: return "Unavailable For Legal Reasons";

	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	case 504: return "Gateway Timeout";
	case 505: return "HTTP Version Not Supported";
	case 506: return "Variant Also Negotiates";
	case 507: return "Insufficient Storage";
	case 508: return "Loop Detected";
	case 510: return "Not Extended";
	case 511: return "Network Authentication Required";

	default: return { };
	}
*/
}
