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
#include <cinttypes>
#include <vector>
#include <photon/common/object.h>
#include <photon/common/callback.h>
#include <photon/common/string_view.h>
#include <photon/net/socket.h>
#include <photon/net/http/message.h>

namespace photon {
namespace fs {
    class IFileSystem;
}

namespace net {
namespace http {

enum class Protocol {
    HTTP = 0,
    HTTPS
};

using DelegateHTTPHandler = Delegate<int, Request&, Response&, std::string_view>;

class HTTPHandler : public Object {
public:
    virtual int handle_request(Request&, Response&, std::string_view) = 0;
};

class HTTPServer : public Object {
public:
    virtual int handle_connection(ISocketStream* stream) = 0;
    ISocketServer::Handler get_connection_handler() {
        return {this, &HTTPServer::handle_connection};
    }

    // should matching pattern in add_handler calling order, currently only prefix matching is supported
    // empty pattern for default handler, which is used when matching patterns failed
    // if no handler was set, return 404
    virtual void add_handler(DelegateHTTPHandler handler, std::string_view pattern = "") = 0;
    virtual void add_handler(HTTPHandler *handler, bool ownership = false, std::string_view pattern = "") = 0;
};

class Client;

// modify body is not allowed
using Director = Delegate<int, Request&, Request&>;
using Modifier = Delegate<int, Response&, Response&>;

// handler will ignore @ignore_prefix in target prefix
HTTPHandler* new_fs_handler(fs::IFileSystem* fs);

HTTPHandler* new_proxy_handler(Director cb_Director, Modifier cb_Modifier,
                               Client* client = nullptr, bool client_ownership = false);

HTTPHandler* new_default_forward_proxy_handler(uint64_t timeout = -1);

HTTPServer* new_http_server();

} // namespace http
} // namespace net
} // namespace photon
