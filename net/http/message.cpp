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

#include "message.h"
#include <photon/common/utility.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/stream.h>
#include <photon/common/timeout.h>
#include <photon/net/socket.h>
#include "headers.h"
#include "url.h"
#include "parser.h"
#include "body.h"

namespace photon {
namespace net {
namespace http {

static ssize_t constexpr MAX_TRANSFER_BYTES = 4 * 1024;
static ssize_t constexpr RESERVED_INDEX_SIZE = 1024;

Message::~Message() {
    if (m_stream_ownership && m_stream) {
        if (m_abandon || (m_body_stream && m_body_stream->close() < 0) ) {
            LOG_DEBUG("close sockstream");
            m_stream->close();
        }
        delete m_stream;
    }

    if (m_buf_ownership && m_buf)
        free(m_buf);
}

int Message::receive_header(uint64_t timeout) {
    auto tmo = Timeout(timeout);
    int ret = 0;
    while (1) {
        m_stream->timeout(tmo.timeout());
        ret = receive_bytes(m_stream);
        if (ret < 0) {
            if (m_stream_ownership) m_stream->close();
            return ret;
        }
        if (ret == 1)
            return 1;
        break;
    }
    return prepare_body_read_stream();
}


int Message::receive_bytes(net::ISocketStream* stream) {
    if (m_buf_capacity - m_buf_size <= MAX_TRANSFER_BYTES + RESERVED_INDEX_SIZE)
        LOG_ERROR_RETURN(ENOBUFS, -1, "no buffer");

    auto rc = stream->recv(m_buf + m_buf_size, MAX_TRANSFER_BYTES);
    if (message_status == INIT && rc == 0)
        return 1;
    auto ret = (rc < 0) ? rc : append_bytes((uint16_t)rc);
    if (ret != 0 && rc == 0)
        LOG_ERROR_RETURN(0, -1, "Peer closed");
    return ret;
}

int Message::append_bytes(uint16_t size) {
    if (message_status == HEADER_PARSED)
        LOG_ERROR_RETURN(0, -1, "double parse");
    if(m_buf_size + size >= (uint64_t)m_buf_capacity)
        LOG_ERROR_RETURN(0, -1, "no buffer");
    std::string_view sv(m_buf + m_buf_size, size);
    auto income = m_buf + m_buf_size;
    auto left = income - 3;
    if (left < m_buf) left = m_buf;
    m_buf_size += size;
    std::string_view whole(left, m_buf + m_buf_size - left);
    auto pos = whole.find("\r\n\r\n");
    if (pos == whole.npos) return 1;

    pos += 4 - (income - left);
    auto body_begin = sv.begin() + pos - m_buf;
    m_body = {(uint16_t)body_begin, sv.size() - pos};
    Parser p({m_buf, m_buf_size});

    auto parse_res = parse_start_line(p);
    if (parse_res < 0) {
        return parse_res;
    }

    LOG_DEBUG("add headers, header buf size `", m_buf + m_buf_capacity - p.cur());
    if (headers.reset(p.cur(), m_buf + m_buf_capacity - p.cur(), m_buf + m_buf_size - p.cur()) < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to parse headers");

    m_abandon = (headers.get_value("Connection") == "close") || (!headers.get_value("Trailer").empty());
    message_status = HEADER_PARSED;
    LOG_DEBUG("header parsed");
    return 0;
}

int Message::send_header(net::ISocketStream* stream) {
    if (stream != nullptr) m_stream = stream; // update stream if needed

    if (m_keep_alive)
        headers.insert("Connection", "keep-alive");
    else
        headers.insert("Connection", "close");

    if (headers.space_remain() < 2)
        LOG_ERRNO_RETURN(ENOBUFS, -1, "no buffer");

    memcpy(m_buf + m_buf_size + headers.size(), "\r\n", 2);
    std::string_view sv = {m_buf, m_buf_size + headers.size() + 2UL};

    if (m_stream->write(sv.data(), sv.size()) != (ssize_t)sv.size())
        LOG_ERROR_RETURN(0, -1, "send header failed");
    message_status = HEADER_SENT;
    return prepare_body_write_stream();
}

ssize_t Message::read(void *buf, size_t count) {
    if (!m_body_stream)
        LOG_ERROR_RETURN(EIO, -1, "body not readable");
    return m_body_stream->read(buf, count);
}

ssize_t Message::readv(const struct iovec *iov, int iovcnt) {
    if (!m_body_stream)
        LOG_ERROR_RETURN(EIO, -1, "body not readable");
    return m_body_stream->readv(iov, iovcnt);
}

ssize_t Message::write(const void *buf, size_t count) {
    if (message_status < HEADER_SENT && send_header() < 0)
        return -1;
    message_status = BODY_SENT;
    if (!m_body_stream)
        LOG_ERROR_RETURN(EIO, -1, "body not writable");
    return m_body_stream->write(buf, count);
}

ssize_t Message::writev(const struct iovec *iov, int iovcnt) {
    if (message_status < HEADER_SENT && send_header() < 0)
        return -1;
    message_status = BODY_SENT;
    if (!m_body_stream)
        LOG_ERROR_RETURN(EIO, -1, "body not writable");
    return m_body_stream->writev(iov, iovcnt);
}

ssize_t Message::write_stream(IStream *input, size_t size_limit) {
    if (message_status < HEADER_SENT && send_header() < 0)
        return -1;
    message_status = BODY_SENT;
    if (input == nullptr)
        return 0;

    size_t buf_size = 65536;
    char seg_buf[buf_size + 4096];
    char *aligned_buf = (char*) (((uint64_t)(&seg_buf[0]) + 4095) / 4096 * 4096);
    size_t ret = 0;
    while (ret < size_limit) {
        size_t count = (size_limit-ret < buf_size) ? size_limit-ret : buf_size;
        ssize_t rc = input->read(aligned_buf, count);
        if (rc == 0) break; // end of stream
        if (rc < 0) {
            LOG_ERROR_RETURN(0, -1, "read stream failed");
        }
        ssize_t wc = write(aligned_buf, rc);
        if (wc != rc)
            LOG_ERRNO_RETURN(0, -1, "send body failed", VALUE(wc), VALUE(rc));
        ret += wc;
    }

    return ret;
}

int Message::skip_remain() {
    if (m_body_stream && m_body_stream->close() == 0) {
        if (m_stream_ownership)
            safe_delete(m_stream);
        return 0;
    }
    return -1;
}

int Message::send() {
    if (message_status < HEADER_SENT && send_header() < 0) {
        LOG_ERROR_RETURN(0, -1, "send response header failed");
    }
    m_body_stream.reset();
    return 0;
}

static constexpr size_t LINE_BUFFER_SIZE = 4 * 1024;

int Message::prepare_body_read_stream() {
    if (headers.chunked()) {
        if (headers.space_remain() < LINE_BUFFER_SIZE)
            LOG_ERROR_RETURN(ENOBUFS, -1, "no buffer");
        m_body_stream.reset(new_chunked_body_read_stream(m_stream, partial_body()));
    } else {
        m_body_stream.reset(new_body_read_stream(m_stream, partial_body(), body_size()));
    }
    return 0;
}

int Message::prepare_body_write_stream() {
    if (headers.chunked()) {
        m_body_stream.reset(new_chunked_body_write_stream(m_stream));
    } else {
        m_body_stream.reset(new_body_write_stream(m_stream, body_size()));
    }
    return 0;
}

ssize_t Message::resource_size() const {
    std::string_view ret;
    auto content_range = headers["Content-Range"];
    if (content_range.empty()) {
        ret = headers["Content-Length"];
        if (ret.empty()) return -1;
    } else {
        auto lenstr = content_range.find_first_of('/');
        if (lenstr == std::string_view::npos) return -1;
        ret = content_range.substr(lenstr + 1);
        if (ret.find_first_of('*') != std::string_view::npos) return -1;
    }
    return estring_view(ret).to_uint64();
}

uint64_t Message::body_size() const {
    if (m_verb == Verb::HEAD) return 0;
    auto it = headers.find("Content-Length");
    if (it != headers.end()) return estring_view(it.second()).to_uint64();
    // or calc from Content-Range
    it = headers.find("Content-Range");
    if (it == headers.end()) return 0;
    size_t start, end;
    if (sscanf("bytes %lu-%lu", it.second().data(), &start, &end) == 2) {
        return end-start+1;
    }
    if (sscanf("bytes */%lu", it.second().data(), &end) == 1) {
        return end;
    }
    return 0;
}

Verb string_to_verb(std::string_view v) {
    for (auto i = Verb::UNKNOWN; i <= Verb::UNLINK; i = Verb((int)i + 1)) {
        if (verbstr[i] == v) return i;
    }
    return Verb::UNKNOWN;
}

inline size_t full_url_size(const URL& u) {
    return u.target().size() + u.host_port().size() +
           (u.secure() ? sizeof(https_url_scheme) : sizeof(http_url_scheme)) - 1;
}

void Request::make_request_line(Verb v, const URL& u, bool enable_proxy) {
    m_secure = u.secure();
    m_port = u.port();
    m_verb = v;
    char* buf = m_buf;
    buf_append(buf, verbstr[v]);
    buf_append(buf, " ");
    uint16_t target_disp = buf - m_buf;
    m_target = {uint16_t(buf - m_buf), u.target().size()};
    if (enable_proxy) {
        m_target = {uint16_t(buf - m_buf), full_url_size(u)};
        buf_append(buf, u.secure() ? https_url_scheme : http_url_scheme);
        buf_append(buf, u.host_port());
    }
    buf_append(buf, u.target());
    uint16_t path_offset = (uint16_t)(u.path().data() - u.target().data()) + target_disp;
    m_path = {path_offset, u.path().size()};
    uint16_t query_offset = (uint16_t)(u.query().data() - u.target().data()) + target_disp;
    m_query = {query_offset, u.query().size()};
    buf_append(buf, " HTTP/1.1\r\n");
    m_buf_size = buf - m_buf;
}

Request::Request(void* buf, uint16_t buf_capacity, Verb v,
                std::string_view url, bool enable_proxy) : Message(buf, buf_capacity) {
    auto ret = reset(v, url, enable_proxy);
    if (ret != 0) assert(false);
}

int Request::reset(Verb v, std::string_view url, bool enable_proxy) {
    URL u(url);
    if ((size_t)m_buf_capacity <= u.target().size() + 21 + verbstr[v].size())
        LOG_ERROR_RETURN(ENOBUFS, -1, "out of buffer");

    LOG_DEBUG("requst reset ", VALUE(u.host()), VALUE(u.host_port()));

    Message::reset();
    make_request_line(v, u, enable_proxy);
    headers.reset(m_buf + m_buf_size, m_buf_capacity - m_buf_size);

    // Host is always the first header
    headers.insert("Host", u.host_port());
    return 0;
}

int Request::redirect(Verb v, estring_view location, bool enable_proxy) {
    estring full_location;
    if (!location.starts_with(http_url_scheme) && (!location.starts_with(https_url_scheme))) {
        full_location.appends(secure() ? https_url_scheme : http_url_scheme,
                             host(), location);
        location = full_location;
    }
    StoredURL u(location);
    auto new_request_line_size = verbstr[v].size() + sizeof(" HTTP/1.1\r\n");
    if (enable_proxy)
        new_request_line_size += full_url_size(u);
    else
        new_request_line_size += u.target().size();

    auto delta = new_request_line_size - m_buf_size;
    LOG_DEBUG(VALUE(delta));
    if (headers.reset_host(delta, u.host_port()) < 0)
        LOG_ERROR_RETURN(0, -1, "failed to move header data");

    m_buf_size = new_request_line_size;
    make_request_line(v, u, enable_proxy);
    return 0;
}

int Request::parse_request_line(Parser &p) {
    auto verb_str = p.extract_until_char(' ');
    m_verb = string_to_verb(m_buf | verb_str);
    if (verb() == Verb::UNKNOWN)
        LOG_ERROR_RETURN(0, -1, "invalid http method");
    p.skip_chars(' ');
    auto target = p.extract_until_char(' ');
    m_target = target;
    p.skip_chars(' ');
    p.skip_string("HTTP/");
    m_version = p.extract_until_char('\r');
    if (m_version.size() >= 6)
        LOG_ERROR_RETURN(0, -1, "invalid scheme");
    p.skip_string("\r\n");
    return 0;
}

int Response::parse_status_line(Parser &p) {
    p.skip_string("HTTP/");
    m_version = p.extract_until_char(' ');
    if (m_version.size() >= 6)
        LOG_ERROR_RETURN(0, -1, "invalid scheme");
    p.skip_chars(' ');
    auto code = p.extract_integer();
    if (code <= 0 || code >= 1000)
        LOG_ERROR_RETURN(0, -1, "invalid status code");
    m_status_code = (uint16_t)code;
    p.skip_chars(' ');
    m_status_message = p.extract_until_char('\r');
    p.skip_string("\r\n");
    return 0;
}

int Response::set_result(int code, std::string_view reason) {
    char* buf = m_buf;
    m_status_code = code;
    buf_append(buf, "HTTP/1.1 ");
    buf_append(buf, std::to_string(code));
    buf_append(buf, " ");
    auto message = reason;
    if (message.empty()) message = obsolete_reason(code);
    buf_append(buf, message);
    buf_append(buf, "\r\n");
    m_buf_size = buf - m_buf;
    headers.reset(m_buf + m_buf_size, m_buf_capacity - m_buf_size);
    return 0;
}


} // namespace http
} // namespace net
} // namespace photon
