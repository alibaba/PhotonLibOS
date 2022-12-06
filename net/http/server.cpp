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

#include "server.h"
#include <string>
#include <fcntl.h>
#include <vector>
#include <photon/net/socket.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/estring.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/httpfs/httpfs.h>
#include <photon/fs/range-split.h>
#include <photon/thread/list.h>
#include <photon/thread/thread11.h>
#include "url.h"
#include "client.h"
#include "message.h"
#include "body.h"


#ifndef MSG_MORE
# define MSG_MORE 0
#endif

namespace photon {
namespace net {
namespace http {

class HTTPServerImpl : public HTTPServer {
public:
    struct SockItem: public intrusive_list_node<SockItem> {
        SockItem(net::ISocketStream* sock): sock(sock) {}
        net::ISocketStream* sock = nullptr;
    };

    struct HandlerRecord {
        estring pattern;
        HTTPHandler* obj;
        bool ownership;
        DelegateHTTPHandler handler;
        int handle(Request &req, Response &resp) {
            return obj ? obj->handle_request(req, resp, pattern)
                       : handler(req, resp, pattern);
        }
    };

    enum class Status {
        running = 1,
        stopping = 2,
    } status = Status::running;

    HandlerRecord m_default_handler = {"", nullptr, false, {this, &HTTPServerImpl::not_found_handler}};
    uint64_t m_workers = 0;
    intrusive_list<SockItem> m_connection_list;
    std::vector<HandlerRecord> m_handlers;

    HTTPServerImpl() {}
    ~HTTPServerImpl() {
        status = Status::stopping;
        for (const auto& it: m_connection_list) {
            it->sock->shutdown(ShutdownHow::ReadWrite);
        }
        while (m_workers != 0) {
            photon::thread_usleep(50 * 1000);
        }
        for (const auto& it: m_handlers) {
            if (it.ownership) delete it.obj;
        }
        if (m_default_handler.ownership)
            delete m_default_handler.obj;
    }

    int not_found_handler(Request &req, Response &resp, std::string_view) {
        resp.set_result(404);
        resp.headers.content_length(0);
        return 0;
    }

    int mux_handler(Request &req, Response &resp) {
        estring_view target = req.target();
        for (auto &h : m_handlers) {
            if (target.starts_with(h.pattern)) {
                LOG_DEBUG("found handler, pattern `", h.pattern);
                return h.handle(req, resp);
            }
        }
        LOG_DEBUG("use default handler");
        return m_default_handler.handle(req, resp);
    }

    int handle_connection(net::ISocketStream* sock) override {
        m_workers++;
        DEFER(m_workers--);
        SockItem sock_item(sock);
        m_connection_list.push_back(&sock_item);
        DEFER(m_connection_list.erase(&sock_item));

        char req_buf[64*1024];
        char resp_buf[64*1024];
        Request req(req_buf, 64*1024-1);
        Response resp(resp_buf, 64*1024-1);

        while (status == Status::running) {
            req.reset(sock, false);

            auto rec_ret = req.receive_header();
            if (rec_ret < 0) {
                LOG_ERROR_RETURN(0, -1, "read request header failed");
            }
            if (rec_ret == 1) {
                LOG_DEBUG("exit");
                return -1;
            }

            LOG_DEBUG("Request Accepted", VALUE(req.verb()), VALUE(req.target()), VALUE(req.headers["Authorization"]));

            resp.reset(sock, false);
            resp.keep_alive(req.keep_alive());

            auto ret = mux_handler(req, resp);
            if (ret < 0) {
                LOG_ERROR_RETURN(0, -1, "handler error ",  VALUE(req.verb()), VALUE(req.target()));
            }

            if (resp.send() < 0) {
                LOG_ERROR_RETURN(0, -1, "failed to send");
            }

            if (!resp.keep_alive())
                break;

            if (req.skip_remain() < 0)
                break;
        }
        return 0;
    }

    void add_handler(DelegateHTTPHandler handler, std::string_view pattern) override {
        LOG_DEBUG("add handler, pattern=`", pattern);
        if (pattern == "") {
            m_default_handler.handler = handler;
            m_default_handler.obj = nullptr;
            m_default_handler.ownership = false;
        } else {
            m_handlers.emplace_back(HandlerRecord{pattern, nullptr, false, handler});
        }
    }
    void add_handler(HTTPHandler* handler, bool ownership, std::string_view pattern) override {
        LOG_DEBUG("add handler, pattern=`", pattern);
        if (pattern == "") {
            m_default_handler.obj = handler;
            m_default_handler.ownership = ownership;
        } else {
            m_handlers.emplace_back(HandlerRecord{pattern, handler, ownership, {}});
        }
    }
};


constexpr static uint64_t KminFileLife = 30 * 1000UL * 1000UL;

class FsHandler : public HTTPHandler {
public:
    fs::IFileSystem* m_fs;

    FsHandler(fs::IFileSystem* fs): m_fs(fs) {}

    void failed_resp(Response &resp, int result = 404) {
        resp.set_result(result);
        resp.headers.content_length(0);
        resp.keep_alive(true);
    }
    int handle_request(Request &req, Response &resp, std::string_view prefix) override {
        LOG_DEBUG("enter fs handler");
        DEFER(LOG_DEBUG("leave fs handler"));
        auto target = req.target();
        auto pos = target.find("?");
        std::string query;
        if (pos != std::string_view::npos) {
            query = std::string(target.substr(pos + 1));
            target = target.substr(0, pos);
        }
        estring filename(target);

        if (!prefix.empty())
            filename = filename.substr(prefix.size() - 1);

        LOG_DEBUG(VALUE(filename));
        auto file = m_fs->open(filename.c_str(), O_RDONLY);
        if (!file) {
            failed_resp(resp);
            LOG_ERROR_RETURN(0, 0, "open file ` failed", target);
        }
        DEFER(delete file);
        if (!query.empty()) file->ioctl(fs::HTTP_URL_PARAM, query.c_str());

        struct stat buf;
        if (file->fstat(&buf) < 0) {
            failed_resp(resp);
            LOG_ERROR_RETURN(0, 0, "stat file ` failed", target);
        }
        auto file_end_pos = buf.st_size - 1;
        auto range = req.headers.range();
        if ((range.first < 0) && (range.second < 0)) {
            range.first = 0;
            range.second = file_end_pos;
        }
        if (range.first < 0) {
            range.first = file_end_pos - range.second;
            range.second = file_end_pos;
        }
        if (range.second < 0) {
            range.second = file_end_pos;
        }
        if ((range.second < range.first) || (range.first > file_end_pos)
                                         || (range.second > file_end_pos)) {
            failed_resp(resp, 416);
            LOG_ERROR_RETURN(0, 0, "invalid request range ", target);
        }
        auto req_size = range.second - range.first + 1;
        if (req_size == buf.st_size)
            resp.set_result(200);
        else {
            resp.set_result(206);
            resp.headers.content_range(range.first, range.second, buf.st_size);
        }

        resp.headers.content_length(req_size);
        if (req.verb() == Verb::HEAD)
            return 0;
        file->lseek(range.first, SEEK_SET);
        return resp.write_stream(&*file, req_size);
    }
};

class ProxyHandler : public HTTPHandler {
public:
    Director m_director;
    Modifier m_modifier;
    Client* m_client;
    bool m_client_ownership;
    ProxyHandler(Director cb_director, Modifier cb_modifier, Client* client, bool client_ownership):
        m_director(cb_director), m_modifier(cb_modifier),
        m_client(client), m_client_ownership(client_ownership) {}

    ~ProxyHandler() {
        if (m_client_ownership)
            delete m_client;
    }

    int handle_request(Request &req, Response &resp, std::string_view) override {
        LOG_DEBUG("enter proxy handler, url : `", req.target());
        int ret = 0;
        DEFER(LOG_DEBUG("leave proxy handler : ` ", req.target(), VALUE(ret)));

        Client::OperationOnStack<64 * 1024 - 1> op(m_client);
        ret = m_director(req, op.req);
        if (ret < 0) return ret;

        op.body_stream = &req;
        op.follow = 0;
        if (op.call() != 0) {
            resp.set_result(502);
            resp.headers.content_length(0);
            resp.keep_alive(false);
            LOG_ERROR_RETURN(0, 0, "http call failed");
        }

        ret = m_modifier(op.resp, resp);
        if (ret < 0) return ret;

        resp.write_stream((IStream*)(&op.resp));

        return 0;
    }
};

class ForwardProxyHandler: public ProxyHandler {
public:
    ForwardProxyHandler(Client* client, bool client_ownership):
        ProxyHandler({this, &default_forward_proxy_director},
                     {this, &default_forward_proxy_modifier},
                     client, client_ownership) {}

    int tunnel_copy(ISocketStream *src, ISocketStream *dst) {
        size_t buf_size = 65536;
        char seg_buf[buf_size + 4096];
        char *aligned_buf = (char*) (((uint64_t)(&seg_buf[0]) + 4095) / 4096 * 4096);

        while (true) {
            ssize_t rc = src->recv(aligned_buf, buf_size);
            if (rc == 0) {
                LOG_DEBUG("end of stream");
                break;
            }
            if (rc < 0) {
                LOG_ERRNO_RETURN(0, -1, "read src stream failed");
            }
            ssize_t wc = dst->write(aligned_buf, rc);
            if (wc != rc)
                LOG_ERRNO_RETURN(0, -1, "write dst stream failed", VALUE(wc), VALUE(rc));
        }

        return 0;
    }

    int handle_request(Request &req, Response &resp, std::string_view prefix) override {
        if (req.verb() ==  Verb::CONNECT) {
            auto pos = req.target().find(":");
            estring_view host, port;
            if (pos != std::string_view::npos) {
                host = req.target().substr(0, pos);
                port = req.target().substr(pos + 1);
            } else {
                host = req.target();
                port = "443";
            }

            auto server_stream = m_client->native_connect(host, port.to_uint64());
            if (server_stream == nullptr) {
                resp.set_result(502);
                LOG_ERRNO_RETURN(0, 0, "failed to connect to host `", req.target());
            }
            DEFER(delete server_stream);

            auto client_stream = req.get_socket_stream();
            resp.set_result(200, "Connection Established");
            resp.send();
            bool stopped = false;

            auto th = photon::thread_enable_join(photon::thread_create11([&, other=photon::CURRENT]{
                tunnel_copy(server_stream, client_stream);
                if (!stopped)
                    photon::thread_interrupt(other, ECANCELED);
            }));

            tunnel_copy(client_stream, server_stream);
            stopped = true;

            photon::thread_interrupt((thread*)th, ECANCELED);
            photon::thread_join(th);
            LOG_DEBUG("tunnel exit");
            return 0;
        }

        return ProxyHandler::handle_request(req, resp, prefix);
    }

    static int default_forward_proxy_director(void*, Request &src, Request &dst) {
        LOG_DEBUG("request target = `", src.target());
        dst.reset(src.verb(), src.target());
        for (auto kv = src.headers.begin(); kv != src.headers.end(); kv++) {
            if (kv.first() != "Host") dst.headers.insert(kv.first(), kv.second(), 1);
        }
        return 0;
    }

    static int default_forward_proxy_modifier(void*, Response &src, Response &dst) {
        dst.set_result(src.status_code());
        for (auto kv : src.headers) {
            dst.headers.insert(kv.first, kv.second);
        }
        return 0;
    }
};


HTTPServer* new_http_server() {
    return new HTTPServerImpl();
}

HTTPHandler* new_fs_handler(fs::IFileSystem* fs) {
    return new FsHandler(fs);
}

HTTPHandler* new_proxy_handler(Director cb_director, Modifier cb_modifier, Client* client, bool client_ownership) {
    if (client == nullptr) {
        client = new_http_client();
        client_ownership = true;
    }
    return new ProxyHandler(cb_director, cb_modifier, client, client_ownership);
}

HTTPHandler* new_default_forward_proxy_handler(uint64_t timeout) {
    auto c = new_http_client();
    c->timeout(timeout);
    return new ForwardProxyHandler(c, true);
}


} // namespace http
} // namespace net
} // namespace photon
