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
#include "body.h"
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>
#include <photon/common/string_view.h>
#include <photon/fs/filesystem.h>
#include <photon/net/socket.h>
#include <photon/common/estring.h>
#include <photon/common/stream.h>
#include <tuple>
#include <utility>

namespace photon {
namespace net {
namespace http {

static constexpr size_t SKIP_LIMIT = 4 * 1024;
static constexpr size_t LINE_BUFFER_SIZE = 4 * 1024;

class ROStream: public IStream {
public:
    virtual int close() override {
        return 0;
    }
    UNIMPLEMENTED(ssize_t write(const void *buf, size_t count) override);
    UNIMPLEMENTED(ssize_t writev(const struct iovec *iov, int iovcnt) override);
};

class WOStream: public IStream {
public:
    virtual int close() override {
        return 0;
    }
    UNIMPLEMENTED(ssize_t read(void *buf, size_t count) override);
    UNIMPLEMENTED(ssize_t readv(const struct iovec *iov, int iovcnt) override);
};

class BodyReadStream : public ROStream {
public:
    BodyReadStream(net::ISocketStream *stream, std::string_view body,
                   size_t body_remain) : m_stream(stream),
                   m_partial_body_buf((char*)body.data()),
                   m_partial_body_remain(body.size()),
                   m_body_remain(body_remain) {
    }
    ~BodyReadStream() {
    }
    virtual int close() override {
        auto stream_remain = m_body_remain - m_partial_body_remain;
        if (stream_remain > SKIP_LIMIT) return -1;
        if (stream_remain && !m_stream->skip_read(stream_remain)) return -1;
        return 0;
    }

    virtual ssize_t read(void *buf, size_t count) override {
        ssize_t ret = 0;
        if (count > m_body_remain) count = m_body_remain;
        auto read_from_remain = std::min(count, m_partial_body_remain);
        if (read_from_remain > 0) {
            memcpy(buf, m_partial_body_buf, read_from_remain);
            buf = (char*)buf + read_from_remain;
            count -= read_from_remain;
            m_body_remain -= read_from_remain;
            m_partial_body_remain -= read_from_remain;
            m_partial_body_buf += read_from_remain;
            ret += read_from_remain;
        }
        if (count > 0) {
            auto r = m_stream->read(buf, count);
            if (r < 0)
                return r;
            m_body_remain -= r;
            ret += r;
        }
        return ret;
    }

    virtual ssize_t readv(const struct iovec *iov, int iovcnt) override {
        ssize_t ret = 0;
        auto iovec = IOVector(iov, iovcnt);
        while (!iovec.empty()) {
            auto tmp = read(iovec.front().iov_base, iovec.front().iov_len);
            if (tmp < 0) return tmp;
            if (tmp == 0) break;
            iovec.extract_front(tmp);
            ret += tmp;
        }
        return ret;
    }

protected:
    net::ISocketStream *m_stream;
    char* m_partial_body_buf;
    size_t m_partial_body_remain, m_body_remain;
};

class ChunkedBodyReadStream : public BodyReadStream {
public:
    ChunkedBodyReadStream(net::ISocketStream *stream, std::string_view body) :
                   BodyReadStream(stream, body, 0) {
        m_line_size = body.size();
        m_get_line_buf = (char*)body.data();
    }
    int close() override {
        if (!m_finish) return -1;
        return 0;
    }
    bool pos_next_chunk(int pos) {
        estring_view line(m_get_line_buf + pos, m_line_size - pos);
        auto p = line.find("\r\n");
        if (p == std::string_view::npos) return false;
        // Quote from rfc2616#section-3.6.1: The chunk-size field is a string of hex digits
        // indicating the size of the chunk.
        m_chunked_remain = line.substr(0, p).hex_to_uint64();
        if (m_chunked_remain != 0 || p == 0) {
            m_cursor += p + 2;
            return true;
        }
        m_finish = true;
        auto body_remain = pos + p + 4 - m_line_size;
        if (body_remain > 2) {
            close();
            return true;
        }
        if (body_remain > 0) {
            char tmp_buf[4];
            auto ret = m_stream->read(tmp_buf, body_remain);
            if (ret != (ssize_t)body_remain) m_stream->close();
        }
        return true;
    }
    ssize_t read_from_line_buf(void* &buf, size_t &count) {
        //read from line_buf
        ssize_t ret = 0;
        while (count > 0 && m_cursor < m_line_size && !m_finish) {
            //read chunk
            auto read_from_line = std::min(count,
                                           std::min(m_chunked_remain, m_line_size - m_cursor));
            memcpy(buf, m_get_line_buf + m_cursor, read_from_line);
            m_cursor += read_from_line;
            buf = (char*)buf + read_from_line;
            ret += read_from_line;
            count -= read_from_line;
            m_chunked_remain -= read_from_line;
            // get chunk size
            if (m_chunked_remain == 0 && !pos_next_chunk(m_cursor)) {
                memmove(m_get_line_buf,
                        m_get_line_buf + m_cursor, m_line_size - m_cursor);
                m_line_size -= m_cursor;
                m_cursor = 0;
                return ret;
            }
            if (m_cursor == m_line_size) m_cursor = m_line_size = 0;
        }
        return ret;
    }
    ssize_t read_from_stream(void* &buf, size_t &count) {
        LOG_DEBUG("read from stream count ", count);
        auto read_from_chunked_cnt = std::min(count, m_chunked_remain);
        auto r = m_stream->read(buf, read_from_chunked_cnt);
        if (r < 0) return r;
        buf = (char*)buf + r;
        m_chunked_remain -= r;
        count -= r;
        return r;
    }
    int get_new_chunk() {
        if (m_cursor < m_line_size) {
            if (pos_next_chunk(m_cursor))
                return 0;
            else {
                memmove(m_get_line_buf,
                        m_get_line_buf + m_cursor, m_line_size - m_cursor);
                m_line_size -= m_cursor;
                m_cursor = 0;
            }
        }

        bool get_line_finish = false;
        while (!get_line_finish && !m_finish) {
            assert(m_line_size != LINE_BUFFER_SIZE);
            auto r = m_stream->recv(m_get_line_buf + m_line_size,
                                    LINE_BUFFER_SIZE - m_line_size);
            if (r < 0) return r;
            if (r == 0) {
                LOG_ERROR_RETURN(0, -1, "Peer closed");
            }
            m_line_size += r;
            if (m_line_size <= 2) continue; // too small
            get_line_finish = pos_next_chunk(0);
        }
        return 0;
    }
    virtual ssize_t read(void *buf, size_t count) override {
        ssize_t ret = 0;
        while (count > 0 && !m_finish) {
            ret += read_from_line_buf(buf, count);
            //read remain from stream
            if (m_chunked_remain > 0 && count > 0) {
                auto r = read_from_stream(buf, count);
                if (r < 0) return r;
                ret += r;
                if (m_chunked_remain > 0 && r == 0) break;
            }
            // get new chunk header
            if (m_chunked_remain == 0) {
                auto r = get_new_chunk();
                if (r < 0) return r;
            }
        }
        return ret;
    }

protected:
    char *m_get_line_buf;
    size_t m_chunked_remain = 0, m_line_size = 0, m_cursor = 0;
    bool m_finish = false;
};


class BodyWriteStream: public WOStream {
public:
    BodyWriteStream(net::ISocketStream *stream, size_t size): m_stream(stream), m_size(size) {
    }

    virtual ssize_t write(const void *buf, size_t count) override {
        if (m_cnt + count > m_size) LOG_WARN("data size overflow");
        ssize_t wc = std::min(count, m_size - m_cnt);
        if (m_stream->write(buf, wc) != wc) return -1;
        m_cnt += wc;
        return wc;
    }

    virtual ssize_t writev(const struct iovec *iov, int iovcnt) override {
        auto count = iovector_view((struct iovec*)iov, iovcnt).sum();
        if (m_cnt + count > m_size) LOG_WARN("data size overflow");
        ssize_t wc = std::min(count, m_size - m_cnt);
        if (m_stream->writev(iov, iovcnt) != wc) return -1;
        m_cnt += wc;
        return wc;
    }

protected:
    net::ISocketStream *m_stream;
    size_t m_size = 0;
    size_t m_cnt = 0;
};

class ChunkedBodyWriteStream: public WOStream {
public:
    ChunkedBodyWriteStream(net::ISocketStream *stream): m_stream(stream) {
    }
    ~ChunkedBodyWriteStream() {
        close();
    }
    virtual int close() override {
        if (m_finish) return 0;
        if (write(nullptr, 0) == 0) {
            m_finish = true;
            return 0;
        }
        return -1;
    }

    virtual ssize_t write(const void *buf, size_t count) override {
        char chunk_size[10];
        auto size = snprintf(chunk_size, sizeof(chunk_size), "%x\r\n", (unsigned)count);
        if (size <= 0) return -1;
        struct iovec iov[3] = {{chunk_size, (size_t)size}, {(void*)buf, count}, {&chunk_size[size - 2], 2}};
        ssize_t total = size + count + 2;
        if (m_stream->writev(iov, 3) != total) return -1;
        return count;
    }

    virtual ssize_t writev(const struct iovec *iov, int iovcnt) override {
        char chunk_size[10];
        ssize_t count = iovector_view((struct iovec*)iov, iovcnt).sum();
        auto size = snprintf(chunk_size, sizeof(chunk_size), "%x\r\n", (unsigned)count);
        if (m_stream->write(chunk_size, size) != size) return -1;
        if (m_stream->writev(iov, iovcnt) != count) return -1;
        if (m_stream->write(&chunk_size[size - 2], 2) != 2) return -1;
        return count;
    }

protected:
    net::ISocketStream *m_stream;
    bool m_finish = false;
};


IStream *new_chunked_body_read_stream(net::ISocketStream *stream, std::string_view body) {
    return new ChunkedBodyReadStream(stream, body);
}

IStream *new_body_read_stream(net::ISocketStream *stream, std::string_view body, size_t body_remain) {
    return new BodyReadStream(stream, body, body_remain);
}

IStream *new_body_write_stream(net::ISocketStream *stream, size_t size) {
    return new BodyWriteStream(stream, size);
}

IStream *new_chunked_body_write_stream(net::ISocketStream *stream) {
    return new ChunkedBodyWriteStream(stream);
}

} // namespace http
} // namespace net
} // namespace photon
