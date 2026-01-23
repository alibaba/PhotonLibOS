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

#include "websocket.h"
#include <photon/common/alog-stdstring.h>
#include <photon/common/timeout.h>
#include <photon/common/checksum/digest.h>
#include <photon/net/socket.h>
#include <photon/net/utils.h>
#include <random>
#include <cstring>

namespace photon {
namespace net {
namespace http {

static constexpr char SHA1_MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static constexpr size_t MAX_HEADER_SIZE = 14;  // 2 + 8 (extended len) + 4 (mask)

// ============================================================================
// Frame encoding/decoding utilities
// ============================================================================

static uint32_t generate_masking_key() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    return std::uniform_int_distribution<uint32_t>{}(gen);
}

// Core implementation: apply mask to a buffer with offset, returns new offset
static size_t apply_mask_impl(void* data, size_t len, uint32_t mask, size_t offset) {
    if (len == 0) return offset;

    auto* p = static_cast<uint8_t*>(data);
    auto* k = reinterpret_cast<uint8_t*>(&mask);
    size_t idx = 0;

    // Process until 8-byte memory aligned
    while (idx < len && (reinterpret_cast<uintptr_t>(p + idx) & 7) != 0) {
        p[idx] ^= k[(offset + idx) & 3];
        idx++;
    }

    // Create 64-bit mask adjusted for current offset
    // Rotate mask to align with (offset + idx) & 3
    size_t shift = ((offset + idx) & 3) * 8;
    uint32_t adjusted_mask = (shift == 0) ? mask : (mask >> shift) | (mask << (32 - shift));
    uint64_t mask64 = (static_cast<uint64_t>(adjusted_mask) << 32) | adjusted_mask;

    // 8-byte batch processing
    while (idx + 8 <= len) {
        *reinterpret_cast<uint64_t*>(p + idx) ^= mask64;
        idx += 8;
    }

    // Process remaining bytes
    while (idx < len) {
        p[idx] ^= k[(offset + idx) & 3];
        idx++;
    }

    return offset + len;
}

static void apply_mask(void* data, size_t len, uint32_t mask) {
    apply_mask_impl(data, len, mask, 0);
}

static void apply_mask_iov(iovec* iov, int iovcnt, uint32_t mask) {
    size_t offset = 0;
    for (int i = 0; i < iovcnt; i++) {
        offset = apply_mask_impl(iov[i].iov_base, iov[i].iov_len, mask, offset);
    }
}

static size_t build_frame_header(uint8_t* buf, WebSocketOpcode opcode,
                                  size_t payload_len, bool masked,
                                  uint32_t* out_mask = nullptr) {
    size_t idx = 0;
    buf[idx++] = 0x80 | static_cast<uint8_t>(opcode);  // FIN + opcode
    
    if (payload_len <= 125) {
        buf[idx++] = (masked << 7) | payload_len;
    } else if (payload_len <= 0xFFFF) {
        buf[idx++] = (masked << 7) | 126;
        buf[idx++] = (payload_len >> 8) & 0xFF;
        buf[idx++] = payload_len & 0xFF;
    } else {
        buf[idx++] = (masked << 7) | 127;
        for (int i = 7; i >= 0; i--)
            buf[idx++] = (payload_len >> (i * 8)) & 0xFF;
    }
    
    if (masked) {
        uint32_t mask = generate_masking_key();
        memcpy(buf + idx, &mask, 4);
        idx += 4;
        if (out_mask) *out_mask = mask;
    }
    return idx;
}

static ssize_t parse_frame_header(ISocketStream* stream, WebSocketOpcode* opcode,
                                   bool* masked, uint32_t* mask) {
    uint8_t hdr[2];
    if (stream->read(hdr, 2) != 2)
        LOG_ERROR_RETURN(0, -1, "Failed to read frame header");
    
    *opcode = static_cast<WebSocketOpcode>(hdr[0] & 0x0F);
    *masked = (hdr[1] >> 7) & 1;
    size_t len = hdr[1] & 0x7F;
    
    if (len == 126) {
        uint8_t ext[2];
        if (stream->read(ext, 2) != 2)
            LOG_ERROR_RETURN(0, -1, "Failed to read 16-bit length");
        len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (stream->read(ext, 8) != 8)
            LOG_ERROR_RETURN(0, -1, "Failed to read 64-bit length");
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | ext[i];
    }
    
    if (*masked && stream->read(mask, 4) != 4)
        LOG_ERROR_RETURN(0, -1, "Failed to read masking key");
    
    return len;
}

// ============================================================================
// Handshake utilities
// ============================================================================

static std::string compute_accept_key(std::string_view client_key) {
    estring data = estring().appends(client_key, SHA1_MAGIC);
    uint8_t hash[20];
    sha1 hasher;
    hasher.update(data.data(), data.size());
    hasher.finalize(hash);
    Base64Encode({reinterpret_cast<char*>(hash), 20}, data);
    return data;
}

static std::string generate_websocket_key() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    static thread_local std::uniform_int_distribution<uint32_t> dist;
    uint32_t random[4] = {dist(gen), dist(gen), dist(gen), dist(gen)};
    std::string key;
    Base64Encode({reinterpret_cast<char*>(random), 16}, key);
    return key;
}

// ============================================================================
// WebSocket Stream Implementation
// ============================================================================

class WebSocketStreamImpl : public IWebSocketStream {
    ISocketStream* m_stream;
    bool m_is_client;
    bool m_is_closed = false;
    bool m_owns_stream;

public:
    WebSocketStreamImpl(ISocketStream* stream, bool is_client, bool owns_stream)
        : m_stream(stream), m_is_client(is_client), m_owns_stream(owns_stream) {}

    ~WebSocketStreamImpl() override {
        if (!m_is_closed) close(WebSocketCloseCode::GoingAway, "");
        if (m_owns_stream) delete m_stream;
    }

    ssize_t send_text(std::string_view text, uint64_t timeout) override {
        return send_frame(WebSocketOpcode::Text, text.data(), text.size(), timeout);
    }

    ssize_t send_binary(const void* data, size_t size, uint64_t timeout) override {
        return send_frame(WebSocketOpcode::Binary, data, size, timeout);
    }

    ssize_t send_binary(iovector_view iov, uint64_t timeout) override {
        return send_frame_iov(WebSocketOpcode::Binary, iov.iov, iov.iovcnt, timeout);
    }

    int ping(std::string_view data, uint64_t timeout) override {
        return send_frame(WebSocketOpcode::Ping, data.data(), data.size(), timeout) >= 0 ? 0 : -1;
    }

    ssize_t recv_frame(void* buf, size_t size, WebSocketOpcode* opcode, uint64_t timeout) override {
        iovec iov = {buf, size};
        return recv_frame_impl(&iov, 1, opcode, timeout, false);
    }

    ssize_t recv_frame(iovector* iov, WebSocketOpcode* opcode, uint64_t timeout) override {
        if (!iov) return -1;
        
        Timeout tmo(timeout);
        m_stream->timeout(tmo.timeout());
        
        // Parse header first to know payload size
        WebSocketOpcode op;
        bool masked;
        uint32_t mask = 0;
        ssize_t payload_len = parse_frame_header(m_stream, &op, &masked, &mask);
        if (payload_len < 0) return -1;
        if (opcode) *opcode = op;
        
        // Ensure capacity
        size_t available = iov->sum();
        if (available < static_cast<size_t>(payload_len)) {
            if (iov->push_back(payload_len - available) < payload_len - available)
                LOG_ERROR_RETURN(ENOBUFS, -1, "Insufficient iovector capacity");
        }
        
        // Read payload
        if (payload_len > 0) {
            auto view = iov->view();
            if (read_payload_iov(view, payload_len, masked, mask) < 0)
                return -1;
        }
        
        handle_control_frame(op, payload_len > 0 ? iov->view().iov : nullptr,
                            payload_len > 0 ? 1 : 0, payload_len);
        return payload_len;
    }

    int close(WebSocketCloseCode code, std::string_view reason) override {
        if (m_is_closed) return 0;
        
        uint8_t payload[2 + 125];  // Close code + max reason
        payload[0] = (static_cast<uint16_t>(code) >> 8) & 0xFF;
        payload[1] = static_cast<uint16_t>(code) & 0xFF;
        size_t len = 2;
        
        if (!reason.empty()) {
            size_t copy_len = std::min(reason.size(), sizeof(payload) - 2);
            memcpy(payload + 2, reason.data(), copy_len);
            len += copy_len;
        }
        
        ssize_t ret = send_frame(WebSocketOpcode::Close, payload, len, -1);
        m_is_closed = true;
        return ret >= 0 ? 0 : -1;
    }

    bool is_closed() const override { return m_is_closed; }
    ISocketStream* get_socket_stream() override { return m_stream; }

private:
    ssize_t send_frame(WebSocketOpcode opcode, const void* data, size_t size, uint64_t timeout) {
        iovec iov = {const_cast<void*>(data), size};
        return send_frame_iov(opcode, &iov, 1, timeout);
    }

    ssize_t send_frame_iov(WebSocketOpcode opcode, const iovec* iov, int iovcnt, uint64_t timeout) {
        if (!m_stream || m_is_closed) return -1;
        
        iovector_view payload(const_cast<iovec*>(iov), iovcnt);
        size_t payload_len = payload.sum();
        
        uint8_t header[MAX_HEADER_SIZE];
        uint32_t mask = 0;
        size_t header_len = build_frame_header(header, opcode, payload_len, m_is_client, &mask);
        
        Timeout tmo(timeout);
        m_stream->timeout(tmo.timeout());
        
        ssize_t expected = header_len + payload_len;
        ssize_t nwritten;
        
        IOVector send_buf;
        send_buf.push_back(header, header_len);
        
        if (m_is_client && payload_len > 0) {
            // Client must mask - copy and mask payload
            send_buf.push_back(payload_len);
            auto* masked = static_cast<uint8_t*>(send_buf.back().iov_base);
            payload.memcpy_to(masked, payload_len);
            apply_mask(masked, payload_len, mask);
        } else {
            // Server or empty payload - send directly
            for (int i = 0; i < iovcnt; i++)
                send_buf.push_back(iov[i]);
        }
        nwritten = m_stream->writev(send_buf.iovec(), send_buf.iovcnt());
        
        if (nwritten != expected)
            LOG_ERROR_RETURN(0, -1, "Failed to send frame");
        
        if (opcode == WebSocketOpcode::Close)
            m_is_closed = true;
        
        return payload_len;
    }

    ssize_t recv_frame_impl(iovec* iov, int iovcnt, WebSocketOpcode* opcode,
                            uint64_t timeout, bool header_parsed,
                            ssize_t payload_len = 0, bool masked = false, uint32_t mask = 0) {
        if (!m_stream) return -1;
        
        Timeout tmo(timeout);
        m_stream->timeout(tmo.timeout());
        
        WebSocketOpcode op;
        if (!header_parsed) {
            payload_len = parse_frame_header(m_stream, &op, &masked, &mask);
            if (payload_len < 0) return -1;
        } else {
            op = *opcode;
        }
        if (opcode) *opcode = op;
        
        // Check buffer capacity using iovector_view
        iovector_view view(iov, iovcnt);
        if (view.sum() < static_cast<size_t>(payload_len))
            LOG_ERROR_RETURN(ENOBUFS, -1, "Buffer too small for payload");
        
        if (payload_len > 0 && read_payload_iov(view, payload_len, masked, mask) < 0)
            return -1;
        
        handle_control_frame(op, iov, iovcnt, payload_len);
        return payload_len;
    }

    ssize_t read_payload_iov(iovector_view view, size_t len, bool masked, uint32_t mask) {
        // Shrink view to exact length needed
        view.shrink_to(len);
        
        if (m_stream->readv(view.iov, view.iovcnt) != static_cast<ssize_t>(len))
            LOG_ERROR_RETURN(0, -1, "Failed to read payload");
        
        if (masked)
            apply_mask_iov(view.iov, view.iovcnt, mask);
        
        return len;
    }

    void handle_control_frame(WebSocketOpcode op, const iovec* iov, int iovcnt, size_t len) {
        if (op == WebSocketOpcode::Close) {
            m_is_closed = true;
        } else if (op == WebSocketOpcode::Ping) {
            if (len > 0 && iov && iovcnt > 0)
                send_frame_iov(WebSocketOpcode::Pong, iov, iovcnt, -1);
            else
                send_frame(WebSocketOpcode::Pong, nullptr, 0, -1);
        }
    }
};

// ============================================================================
// Client connection
// ============================================================================

IWebSocketStream* websocket_connect(Client* client, std::string_view url, uint64_t timeout) {
    if (!client)
        LOG_ERROR_RETURN(EINVAL, nullptr, "Invalid client");
    
    Client::OperationOnStack<4 * 1024> op(client, Verb::GET, url);
    
    std::string key = generate_websocket_key();
    op.req.headers.insert("Upgrade", "websocket");
    op.req.headers.insert("Connection", "Upgrade");
    op.req.headers.insert("Sec-WebSocket-Key", key);
    op.req.headers.insert("Sec-WebSocket-Version", "13");
    op.timeout = Timeout(timeout);
    op.follow = 0;
    op.retry = 0;
    
    if (op.call() < 0 || op.status_code != 101)
        LOG_ERROR_RETURN(0, nullptr, "Upgrade failed: status=", op.status_code);
    
    if (op.resp.headers["Sec-WebSocket-Accept"] != compute_accept_key(key))
        LOG_ERROR_RETURN(0, nullptr, "Accept key mismatch");
    
    auto* stream = op.resp.steal_socket_stream();
    if (!stream)
        LOG_ERROR_RETURN(0, nullptr, "Failed to get socket");
    
    return new WebSocketStreamImpl(stream, true, true);
}

IWebSocketStream* Client::websocket_connect(std::string_view url, uint64_t timeout) {
    return http::websocket_connect(this, url, timeout);
}

// ============================================================================
// Server accept
// ============================================================================

IWebSocketStream* server_accept_websocket(Request& req, Response& resp) {
    auto upgrade = req.headers["Upgrade"];
    auto connection = req.headers["Connection"];
    auto version = req.headers["Sec-WebSocket-Version"];
    auto key = req.headers["Sec-WebSocket-Key"];
    
    if (upgrade != "websocket")
        LOG_ERROR_RETURN(0, nullptr, "Invalid Upgrade header");
    if (connection.find("Upgrade") == std::string_view::npos &&
        connection.find("upgrade") == std::string_view::npos)
        LOG_ERROR_RETURN(0, nullptr, "Invalid Connection header");
    if (version != "13")
        LOG_ERROR_RETURN(0, nullptr, "Unsupported version: ", version);
    if (key.empty())
        LOG_ERROR_RETURN(0, nullptr, "Missing Sec-WebSocket-Key");
    
    resp.set_result(101, "Switching Protocols");
    resp.headers.insert("Upgrade", "websocket");
    resp.headers.insert("Connection", "Upgrade");
    resp.headers.insert("Sec-WebSocket-Accept", compute_accept_key(key));
    resp.headers.content_length(0);
    
    if (resp.send() < 0)
        LOG_ERROR_RETURN(0, nullptr, "Failed to send response");
    
    auto* stream = req.get_socket_stream();
    if (!stream)
        LOG_ERROR_RETURN(0, nullptr, "Failed to get socket");
    
    return new WebSocketStreamImpl(stream, false, false);
}

// ============================================================================
// HTTP Handler wrapper
// ============================================================================

class WebSocketHTTPHandler : public HTTPHandler {
    WebSocketHandler m_handler;
public:
    explicit WebSocketHTTPHandler(WebSocketHandler h) : m_handler(h) {}
    
    int handle_request(Request& req, Response& resp, std::string_view) override {
        if (req.headers["Upgrade"] != "websocket") {
            resp.set_result(400, "Bad Request");
            resp.headers.content_length(0);
            return 0;
        }
        
        auto* ws = server_accept_websocket(req, resp);
        if (!ws) {
            resp.set_result(400, "Bad Request");
            resp.headers.content_length(0);
            return 0;
        }
        
        int ret = m_handler(ws);
        delete ws;
        
        resp.keep_alive(false);
        resp.message_status = BODY_SENT;
        return ret;
    }
};

HTTPHandler* new_websocket_handler(WebSocketHandler handler) {
    return new WebSocketHTTPHandler(handler);
}

} // namespace http
} // namespace net
} // namespace photon
