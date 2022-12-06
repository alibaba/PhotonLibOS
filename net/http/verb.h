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
#include <photon/common/conststr.h>
namespace photon {
namespace net {
namespace http {
DEFINE_ENUM_STR(Verb, verbstr, UNKNOWN, DELETE, GET, HEAD, POST, PUT, CONNECT,
                OPTIONS, TRACE, COPY, LOCK, MKCOL, MOV, PROPFIND, PROPPATCH,
                SEARCH, UNLOCK, BIND, REBIND, UNBIND, ACL, REPORT, MKACTIVITY,
                CHECKOUT, MERGE, MSEARCH, NOTIFY, SUBSCRIBE, UNSUBSCRIBE, PATCH,
                PURGE, MKCALENDAR, LINK, UNLINK);
Verb string_to_verb(std::string_view v);
} // namespace http
} // namespace net
} // namespace photon
