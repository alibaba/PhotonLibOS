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

#include <cassert>
#include <cerrno>
#include <memory>
#include <sys/uio.h>
#include <photon/net/http/verb.h>
#include <photon/common/estring.h>
#include <photon/common/stream.h>
#include <photon/common/callback.h>
#include <photon/net/http/headers.h>

namespace photon {
namespace net {

class ISocketStream;

namespace http {

class Parser;
class HTTPServerImpl;
class ClientImpl;

enum MessageStatus {
    INIT,
    HEADER_SENT,
    BODY_SENT,
    HEADER_PARSED,
};

std::string_view obsolete_reason(int code);
class Message : public IStream {
public:
    Message() = default;
    Message(void* buf, uint16_t buf_capacity, bool buf_ownership = false) {
        reset(buf, buf_capacity, buf_ownership);
    }
    Message(const Message &rhs) = delete;
    Message(Message &&rhs) = default;
    ~Message();

    void reset(void* buf, uint16_t buf_capacity, bool buf_ownership = false,
                ISocketStream* s = nullptr, bool stream_ownership = false) {
        reset(s, stream_ownership);
        if (m_buf_ownership && m_buf) {
            free(m_buf);
        }
        m_buf = (char*)buf;
        m_buf_capacity = buf_capacity;
        m_buf_ownership = buf_ownership;
    }
    void reset(ISocketStream* s, bool stream_ownership = false) {
        reset();
        m_stream = s;
        m_stream_ownership = stream_ownership;
    }
    void reset() {
        headers.reset();
        m_buf_size = 0;
        m_body_stream.reset();
        m_stream_ownership = false;
        reset_status();
    }
    void reset_status() {
        message_status = INIT;
    }
    bool keep_alive() const {
        return m_keep_alive;
    }
    int keep_alive(bool ka) {
        m_keep_alive = ka;
        return 0;
    }
    std::string_view version() const {
        return std::string_view{m_buf, m_buf_size} | m_version;
    }
    Verb verb() const { return m_verb;}

    ssize_t read(void *buf, size_t count) override;
    ssize_t readv(const struct iovec *iov, int iovcnt) override;
    ssize_t write(const void *buf, size_t count) override;
    ssize_t writev(const struct iovec *iov, int iovcnt) override;
    ssize_t write_stream(IStream *stream, size_t size_limit = -1);

    // size of body
    size_t body_size() const;
    // size of origin resource
    ssize_t resource_size() const;

    // in general, it is called automatically
    // in some cases, users can call it manually
    int send();

    int message_status = 0;
    Headers headers;

protected:
    virtual int prepare_body_read_stream();
    virtual int prepare_body_write_stream();
    virtual int parse_start_line(Parser &p) = 0;

    // return 0 if header recvd
    // return 1 if end of stream
    // return negative if an error occured
    int receive_header(uint64_t timeout = -1UL);
    int send_header(net::ISocketStream* stream = nullptr);
    int receive_bytes(net::ISocketStream* stream);
    // return 0 if "\r\n\r\n" is recvd
    // return 1 if "\r\n\r\n" is not recvd
    // return a negative number if an error occured
    int append_bytes(uint16_t size);

    int skip_remain();
    int close() { return 0; }

    std::string_view partial_body() const {
        return std::string_view{m_buf, m_buf_size} | m_body;
    }

    char* m_buf;
    uint16_t m_buf_capacity;
    uint16_t m_buf_size = 0;
    bool m_buf_ownership = false;
    rstring_view16 m_body, m_version;
    std::unique_ptr<IStream> m_body_stream;
    net::ISocketStream* m_stream = nullptr;
    bool m_stream_ownership = false;
    bool m_abandon;
    bool m_keep_alive = true;
    Verb m_verb = Verb::UNKNOWN;

    friend class HTTPServerImpl;
    friend class ClientImpl;
};

class URL;

class Request : public Message {
public:
    Request() = default;
    Request(const Request &rhs) = delete;
    Request(void* buf, uint16_t buf_capacity, Verb v,
                   std::string_view url, bool enable_proxy = false);
    Request(void* buf, uint16_t buf_capacity): Message(buf, buf_capacity) {}
    Request(Request &&rhs) = default;
    int reset(Verb v, std::string_view url, bool enable_proxy = false);
    void reset(ISocketStream* s, bool stream_ownership = false) {
        Message::reset(s, stream_ownership);
    }

    std::string_view target() const {
        return std::string_view{m_buf, m_buf_size} | m_target;
    }
    bool secure() const {
        return m_secure;
    }
    uint16_t port() const {
        return m_port;
    }
    std::string_view host() const {
        return headers["Host"];
    }
    std::string_view abs_path() const {
        return std::string_view{m_buf, m_buf_size} | m_path;
    }
    std::string_view query() const {
        return std::string_view{m_buf, m_buf_size} | m_query;
    }

    using RemainSpace = std::pair<char*, size_t>;
    RemainSpace get_remain_space() const {
        return RemainSpace(m_buf + m_buf_size + headers.size(), headers.space_remain());
    }
    int redirect(Verb v, estring_view location, bool enable_proxy = false);

    net::ISocketStream* get_socket_stream() {
        return m_stream;
    }
protected:
    int parse_request_line(Parser &p);
    int parse_start_line(Parser &p) override {
        return parse_request_line(p);
    }

    void make_request_line(Verb v, const URL& u, bool enable_proxy);
    rstring_view16 m_target, m_path, m_query;
    uint16_t m_port = 80;
    bool m_secure = false;
};

class Response : public Message {
public:
    Response() = default;
    Response(char* buf, uint16_t buf_capacity): Message(buf, buf_capacity) {}
    Response(const Response &rhs) = delete;
    Response(Response &&rhs) = default;

    Response& operator=(const Response &rhs) = delete;
    Response& operator=(const Response &&rhs) = delete;

    void reset(char* buf, uint16_t buf_capacity, bool buf_ownership = false,
        ISocketStream* stream = nullptr, bool stream_ownership = false, Verb v = Verb::UNKNOWN) {
        Message::reset(buf, buf_capacity, buf_ownership, stream, stream_ownership);
        m_status_code = 0;
        m_verb = v;
    }
    void reset(ISocketStream* s, bool stream_ownership = false) {
        Message::reset(s, stream_ownership);
    }

    std::string_view status_message() const {
        return m_status_message | m_buf;
    }

    uint16_t status_code() const {
        return m_status_code;
    }

    int set_result(int code, std::string_view reason = "");
protected:
    int parse_status_line(Parser &p);
    int parse_start_line(Parser &p) override {
        return parse_status_line(p);
    }
    rstring_view16 m_status_message;
    uint16_t m_status_code = 0;
};

} // namespace http
} // namespace net
} // namespace photon