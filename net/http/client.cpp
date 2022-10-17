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

#include "client.h"
#include <bitset>
#include <algorithm>
#include <random>
#include <queue>
#include <photon/common/alog-stdstring.h>
#include <photon/common/expirecontainer.h>
#include <photon/common/iovector.h>
#include <photon/common/string_view.h>
#include <photon/fs/filesystem.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>
#include <photon/net/utils.h>

namespace photon {
namespace net {

static const uint64_t kMinimalStreamLife = 300UL * 1000 * 1000;
static const uint64_t kDNSCacheLife = 3600UL * 1000 * 1000;
static constexpr char USERAGENT[] = "EASE/0.21.6";
static constexpr size_t SKIP_LIMIT = 4 * 1024;
static constexpr size_t LINE_BUFFER_SIZE = 4 * 1024;

class PooledDialer {
public:
    net::TLSContext* tls_ctx = nullptr;
    std::unique_ptr<ISocketClient> tcpsock;
    std::unique_ptr<ISocketClient> tlssock;
    std::unique_ptr<Resolver> resolver;

    //etsocket seems not support multi thread very well, use tcp_socket now. need to find out why
    PooledDialer() :
            tls_ctx(new_tls_context(nullptr, nullptr, nullptr)),
            tcpsock(new_tcp_socket_pool(new_tcp_socket_client())),
            tlssock(new_tcp_socket_pool(new_tls_client(tls_ctx, new_tcp_socket_client(), true), -1)),
            resolver(new_default_resolver(kDNSCacheLife)) {
    }

    ~PooledDialer() { delete tls_ctx; }

    ISocketStream* dial(std::string_view host, uint16_t port, bool secure,
                             uint64_t timeout = -1UL);

    template <typename T>
    ISocketStream* dial(T x, uint64_t timeout = -1UL) {
        return dial(x.host(), x.port(), x.secure(), timeout);
    }
};

class ResponseStream : public IStream {
public:
    ISocketStream *m_stream;
    char* m_partial_body_buf;
    size_t m_partial_body_remain, m_body_remain;
    bool m_abandon = false;
    ResponseStream(ISocketStream *stream, std::string_view body,
                   size_t body_remain, bool abandon) : m_stream(stream),
                   m_partial_body_buf((char*)body.data()),
                   m_partial_body_remain(body.size()),
                   m_body_remain(body_remain), m_abandon(abandon){}
    ~ResponseStream() {
        auto stream_remain = m_body_remain - m_partial_body_remain;
        if (stream_remain > SKIP_LIMIT || m_abandon) m_stream->close();
        if (stream_remain && !m_stream->skip_read(stream_remain)) m_stream->close();
        delete m_stream;
    }
    int close() override {
        return m_stream->close();
    }
    ssize_t read(void *buf, size_t count) override {
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
    ssize_t readv(const struct iovec *iov, int iovcnt) override {
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
    UNIMPLEMENTED(ssize_t write(const void *buf, size_t count) override);
    UNIMPLEMENTED(ssize_t writev(const struct iovec *iov, int iovcnt) override);
};

class ChunkedResponseStream : public ResponseStream {
public:
    char *m_get_line_buf;
    size_t m_chunked_remain = 0, m_line_size = 0, m_cursor = 0;
    bool m_finish = false;
    ChunkedResponseStream(ISocketStream *stream, std::string_view body,
                   size_t body_remain, bool abandon = false) :
                   ResponseStream(stream, body, body_remain, abandon) {
        m_line_size = body.size();
        m_get_line_buf = (char*)body.data();
    }
    ~ChunkedResponseStream() {
        if (!m_finish) close();
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
            auto ret =
                m_stream->read(tmp_buf, body_remain);
            if (ret != (ssize_t)body_remain) m_stream->close();
        }
        return true;
    }
    ssize_t read_from_line_buf(void* &buf, size_t &count) {
        //从line_buf读数据
        ssize_t ret = 0;
        while (m_cursor < m_line_size && !m_finish) {
            //读chunk
            auto read_from_line = std::min(count,
                                    std::min(m_chunked_remain,
                                    m_line_size - m_cursor));
            memcpy(buf, m_get_line_buf + m_cursor, read_from_line);
            m_cursor += read_from_line;
            buf = (char*)buf + read_from_line;
            ret += read_from_line;
            count -= read_from_line;
            m_chunked_remain -= read_from_line;
            // 获取chunk大小
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
        auto read_from_chunked_cnt = std::min(count, m_chunked_remain);
        auto r = m_stream->read(buf, read_from_chunked_cnt);
        if (r < 0) return r;
        buf = (char*)buf + r;
        m_chunked_remain -= r;
        count -= r;
        return r;
    }
    int get_new_chunk() {
        bool get_line_finish = false;
        while (!get_line_finish && !m_finish) {
            assert(m_line_size != LINE_BUFFER_SIZE);
            auto r = m_stream->recv(m_get_line_buf + m_line_size,
                                            LINE_BUFFER_SIZE - m_line_size);
            if (r < 0) return r;
            m_line_size += r;
            get_line_finish = pos_next_chunk(0);
        }
        return 0;
    }
    ssize_t read(void *buf, size_t count) override {
        ssize_t ret = 0;
        while (count > 0 && !m_finish) {
            ret += read_from_line_buf(buf, count);
            //从stream中读取chunk剩余部分
            if (m_chunked_remain > 0 && count > 0) {
                auto r = read_from_stream(buf, count);
                if (r < 0) return r;
                ret += r;
            }
            //获取新的chunk头(get_line)
            if (m_chunked_remain == 0) {
                auto r = get_new_chunk();
                if (r < 0) return r;
            }
        }
        return ret;
    }
};
ISocketStream* PooledDialer::dial(std::string_view host, uint16_t port, bool secure, uint64_t timeout) {
    LOG_DEBUG("Dial to `", host);
    std::string strhost(host);
    auto ipaddr = resolver->resolve(strhost.c_str());
    EndPoint ep(ipaddr, port);
    LOG_DEBUG("Connecting ` ssl: `", ep, secure);
    ISocketStream *sock = nullptr;
    if (secure) {
        tlssock->timeout(timeout);
        sock = tlssock->connect(ep);
    } else {
        tcpsock->timeout(timeout);
        sock = tcpsock->connect(ep);
    }
    if (sock) {
        LOG_DEBUG("Connected ` host : ` ssl: ` `", ep, host, secure, sock);
        return sock;
    }
    LOG_DEBUG("connect ssl : ` ep : `  host : ` failed", secure, ep, host);
    if (ipaddr.addr == 0) LOG_DEBUG("No connectable resolve result");
    // When failed, remove resolved result from dns cache so that following retries can try
    // different ips.
    resolver->discard_cache(strhost.c_str());
    return nullptr;
}

constexpr uint64_t code3xx() { return 0; }
template<typename...Ts>
constexpr uint64_t code3xx(uint64_t x, Ts...xs)
{
    return (1 << (x-300)) | code3xx(xs...);
}
constexpr static std::bitset<10>
    code_redirect_verb(code3xx(300, 301, 302, 307, 308));

static constexpr size_t kMinimalHeadersSize = 8 * 1024 - 1;
enum RoundtripStatus {
    ROUNDTRIP_SUCCESS,
    ROUNDTRIP_FAILED,
    ROUNDTRIP_REDIRECT,
    ROUNDTRIP_NEED_RETRY,
    ROUNDTRIP_FORCE_RETRY
};

class Client_impl : public Client {
public:
    PooledDialer m_dialer;
    CommonHeaders<> m_common_headers;
    ICookieJar *m_cookie_jar;
    Client_impl(ICookieJar *cookie_jar) :
        m_cookie_jar(cookie_jar) {}
    // struct deleter {
    //     void operator() (ISocketStream *s) {
    //         s->close();
    //         delete s;
    //     }
    // };
    using PooledSocketStream_ptr = std::unique_ptr<ISocketStream>;
    int redirect(Operation* op, PooledSocketStream_ptr &sock) {
        auto stream_remain = op->resp.content_length() - op->resp.partial_body().size();
        if (!op->resp["Content-Length"].empty() &&
                stream_remain <= SKIP_LIMIT && sock->skip_read(stream_remain)) {
            // delete sock.release(); // put it back to pool, by not closing it
            sock.reset();
        }

        auto location = op->resp["Location"];
        if (location.empty()) {
            LOG_ERROR_RETURN(EINVAL, ROUNDTRIP_FAILED,
                "redirect but has no field location");
        }
        LOG_DEBUG("Redirect to ", location);

        Verb v;
        auto sc = op->status_code - 300;
        if (sc == 3) {  // 303
            v = Verb::GET;
        } else if (sc < 10 && code_redirect_verb[sc]) {
            v = op->req.verb();
        } else {
            LOG_ERROR_RETURN(EINVAL, ROUNDTRIP_FAILED,
                "invalid 3xx status code: ", op->status_code);
        }

        if (op->req.redirect(v, location, op->enable_proxy) < 0) {
            LOG_ERRNO_RETURN(0, ROUNDTRIP_FAILED, "redirect failed");
        }
        return ROUNDTRIP_REDIRECT;
    }
    int concurreny = 0;
    int do_roundtrip(Operation* op, Timeout tmo) {
        concurreny++;
        LOG_DEBUG(VALUE(concurreny));
        DEFER(concurreny--);
        op->status_code = -1;
        if (tmo.timeout() == 0)
            LOG_ERROR_RETURN(ETIMEDOUT, ROUNDTRIP_FAILED, "connection timedout");
        auto &req = op->req;
        auto s = (m_proxy && !m_proxy_url.empty())
                     ? m_dialer.dial(m_proxy_url, tmo.timeout())
                     : m_dialer.dial(req, tmo.timeout());
        if (!s) LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "connection failed");
        PooledSocketStream_ptr sock(s);
        LOG_DEBUG("Sending request ` `", req.verb(), req.target());

        auto whole = req.whole();
        // LOG_DEBUG(VALUE(whole));
        auto req_size = whole.size();
        if (sock->write(whole.data(), req_size) != (ssize_t)req_size) {
            sock->close();
            LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "send header failed, retry");
        }
        sock->timeout(tmo.timeout());
        if (op->req_body_writer(sock.get()) < 0) {
            LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY, "ReqBodyCallback failed");
            sock->close();
        }
        LOG_DEBUG("Request sent, wait for response ` `", req.verb(),
                  req.target());
        auto space = req.get_remain_space();
        auto &resp = op->resp;
        if (space.second > kMinimalHeadersSize) {
            resp.reset(space.first, space.second, false);
        } else {
            auto buf = malloc(kMinimalHeadersSize);
            resp.reset((char *)buf, kMinimalHeadersSize, true);
        }
        while (1) {
            sock->timeout(tmo.timeout());
            auto ret = resp.append_bytes(sock.get());
            if (ret < 0) {
                sock->close();
                LOG_ERROR_RETURN(0, ROUNDTRIP_NEED_RETRY,
                                 "read response header failed");
            }
            if (ret == 0) break;
        }
        op->status_code = resp.status_code();
        LOG_DEBUG("Got response ` ` code=` || content_length=`", req.verb(),
                  req.target(), op->status_code, resp["Content-Length"]);
        if (m_cookie_jar) m_cookie_jar->get_cookies_from_headers(req.host(), &resp);
        if (op->status_code < 400 && op->status_code >= 300 && op->follow)
            return redirect(op, sock);
        bool abandon = (resp["Connection"] == "close") || (!resp["Trailer"].empty());
        if (op->req.verb() == Verb::HEAD) {
            op->resp_body.reset(new ResponseStream(sock.release(), resp.partial_body(),
                                                    0, abandon));
            return ROUNDTRIP_SUCCESS;
        }
        if (resp["Transfer-Encoding"] == "chunked") {
            if (resp.space_remain() < LINE_BUFFER_SIZE) {
                sock->close();
                LOG_ERROR_RETURN(ENOBUFS, ROUNDTRIP_FAILED, "run out of buffer");
            }
            op->resp_body.reset(new ChunkedResponseStream(sock.release(), resp.partial_body(),
                                                          0, abandon));
        } else {
            auto content_length = estring_view(resp["Content-Length"]).to_uint64();
            resp.content_length(content_length);
            op->resp_body.reset(new ResponseStream(sock.release(), resp.partial_body(),
                                                    content_length, abandon));
        }
        return ROUNDTRIP_SUCCESS;
    }

    int call(Operation* /*IN, OUT*/ op) override {
        auto content_length = op->req.content_length();
        auto encoding = op->req["Transfer-Encoding"];
        if ((content_length != 0) && (encoding == "chunked")) {
            op->status_code = -1;
            LOG_ERROR_RETURN(EINVAL, ROUNDTRIP_FAILED,
                            "Content-Length and Transfer-Encoding conflicted");
        }
        op->req.insert("User-Agent", USERAGENT);
        op->req.insert("Connection", "Keep-Alive");
        op->req.merge_headers(m_common_headers);
        if (m_cookie_jar && m_cookie_jar->set_cookies_to_headers(&op->req) != 0)
            LOG_ERROR_RETURN(0, -1, "set_cookies_to_headers failed");
        Timeout tmo(std::min(op->timeout.timeout(), m_timeout));
        int retry = 0, followed = 0, ret = 0;
        uint64_t sleep_interval = 0;
        while (followed <= op->follow && retry <= op->retry && tmo.timeout() != 0) {
            ret = do_roundtrip(op, tmo);
            if (ret == ROUNDTRIP_SUCCESS || ret == ROUNDTRIP_FAILED) break;
            switch (ret) {
                case ROUNDTRIP_NEED_RETRY:
                    photon::thread_usleep(sleep_interval * 1000UL);
                    sleep_interval = (sleep_interval + 500) * 2;
                    ++retry;
                    break;
                case ROUNDTRIP_REDIRECT:
                    retry = 0;
                    ++followed;
                    break;
                default:
                    break;
            }
            if (tmo.timeout() == 0)
                LOG_ERROR_RETURN(ETIMEDOUT, -1,
                                "connection timedout");
            if (followed > op->follow || retry > op->retry)
                LOG_ERROR_RETURN(ENOENT, -1,
                                "connection failed");
        }
        if (ret != ROUNDTRIP_SUCCESS) LOG_ERROR_RETURN(0, -1,"too many retry, roundtrip failed");
        return 0;
    }

    CommonHeaders<>* common_headers() override {
        return &m_common_headers;
    }
};

Client* new_http_client(ICookieJar *cookie_jar) { return new Client_impl(cookie_jar); }

}  // namespace net
}
