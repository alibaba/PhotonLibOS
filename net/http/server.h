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
#include <photon/common/iovector.h>
#include <photon/net/http/verb.h>
#include <photon/net/socket.h>

namespace photon {
namespace fs {
    class IFileSystem;
}
}

namespace photon {
namespace net {

using HeaderLists = std::vector<std::pair<std::string_view, std::string_view>>;
enum class Protocol {
    HTTP = 0,
    HTTPS
};

class HTTPServerRequest {
public:
    virtual void SetProtocol(Protocol p) = 0;
    virtual Protocol GetProtocol() const = 0;
    virtual Verb GetMethod() const = 0;
    virtual void SetMethod(Verb v) = 0;
    virtual std::string_view GetOriginHost() const = 0;
    virtual std::string_view GetTarget() const = 0;
    virtual void SetTarget(std::string_view target) = 0;
    virtual std::string_view Find(std::string_view key) const = 0;
    virtual int Insert(std::string_view key, std::string_view value) = 0;
    virtual int Erase(std::string_view key) = 0;
    virtual HeaderLists GetKVs() const = 0;
    virtual std::string_view Body() const = 0;
    virtual size_t ContentLength() const = 0;
    virtual std::pair<ssize_t, ssize_t> Range() const = 0;
    virtual void KeepAlive(bool alive) = 0;
    virtual bool KeepAlive() const = 0;
    virtual void Version(int version) = 0;
    virtual int Version() const = 0;
};
enum class RetType {
    failed = 0,
    success = 1,
    connection_close = 2
};
class HTTPServerResponse {
public:
    virtual RetType HeaderDone() = 0;
    virtual RetType Done() = 0;
    virtual void SetResult(int status_code) = 0;
    virtual int GetResult() const = 0;
    virtual std::string_view Find(std::string_view key) const = 0;
    virtual int Insert(std::string_view key, std::string_view value) = 0;
    virtual int Erase(std::string_view key) = 0;
    virtual HeaderLists GetKVs() const = 0;
    virtual ssize_t Write(void* buf, size_t count) = 0;
    virtual ssize_t Writev(const struct iovec *iov, int iovcnt) = 0;
    virtual void ContentLength(size_t len) = 0;
    virtual void KeepAlive(bool alive) = 0;
    virtual bool KeepAlive() const = 0;
    virtual void Version(int version) = 0;
    virtual int Version() const = 0;
    virtual int ContentRange(size_t start, size_t end, ssize_t size = -1) = 0;
    virtual HTTPServerRequest* GetRequest() const = 0;

};

using HTTPServerHandler = Delegate<RetType, HTTPServerRequest&, HTTPServerResponse&>;

class HTTPServer : public Object {
public:
    virtual void SetHTTPHandler(HTTPServerHandler handler) = 0;
    virtual int handle_connection(ISocketStream* stream) = 0;
    ISocketServer::Handler GetConnectionHandler() {
        return {this, &HTTPServer::handle_connection};
    }
};

class HTTPHandler : public Object {
public:
    virtual HTTPServerHandler GetHandler() = 0;
};

class MuxHandler : public HTTPHandler {
public:
    //should matching prefix in add_handler calling order
    virtual void AddHandler(std::string_view prefix, HTTPHandler *handler) = 0;
    /* use default_handler when matching prefix failed
       if no default_handler was set, return 404*/
    virtual void SetDefaultHandler(HTTPHandler *default_handler) = 0;
};
class Client;
//modify body is not allowed
using Director = Delegate<RetType, HTTPServerRequest&>;
//modify body is not allowed
using Modifier = Delegate<RetType, HTTPServerResponse&>;

//handler will ignore @ignore_prefix in target prefix
HTTPHandler* new_fs_handler(fs::IFileSystem* fs,
                            std::string_view ignore_prefix = "");
HTTPHandler* new_reverse_proxy_handler(Director cb_Director,
                                       Modifier cb_Modifier, Client* client);

// maybe imply new_static_file later
// HTTPHandler new_static_file(std::string root);

MuxHandler* new_mux_handler();
HTTPServer* new_http_server();

} // end of namespace net
}
