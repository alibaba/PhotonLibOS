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

#include <sys/uio.h>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string_view>
#include <photon/common/stream.h>
#include "headers.h"

namespace photon {
namespace net {
namespace http {

struct FrameHeader {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint8_t  type;
    uint32_t length : 24;
#else
    uint32_t length : 24;
    uint8_t  type;
#endif
    uint8_t  flags;
    uint32_t stream_id;

#define DECLARE_FLAG(name, bit)                      \
    bool     name()   const { return flags & bit; }  \
    void     set_##name()   { flags |= bit; }        \

    DECLARE_FLAG(ack,         0x01);
    DECLARE_FLAG(end_stream,  0x01);
    DECLARE_FLAG(end_headers, 0x04);
    DECLARE_FLAG(padded,      0x08);
    DECLARE_FLAG(priority,    0x20);

#undef DECLARE_FLAG

    uint8_t padding() const { return padded() ? ext(0) : 0; }
    void padding(uint8_t padding) { set_padded(); ext(0) = padding; }

    enum Type : uint8_t {
        DATA           = 0,
        HEADERS        = 1,
        PRIORITY       = 2,
        RST_STREAM     = 3,
        RESET_STREAM   = 3,
        SETTINGS       = 4,
        PUSH_PROMISE   = 5,
        PING           = 6,
        GOAWAY         = 7,
        WINDOW_UPDATE  = 8,
        CONTINUATION   = 9,
    };

    enum ErrorCode : uint8_t {
        NO_ERROR            = 0x00,
        PROTOCOL_ERROR      = 0x01,
        INTERNAL_ERROR      = 0x02,
        FLOW_CONTROL_ERROR  = 0x03,
        SETTINGS_TIMEOUT    = 0x04,
        STREAM_CLOSED       = 0x05,
        FRAME_SIZE_ERROR    = 0x06,
        REFUSED_STREAM      = 0x07,
        CANCEL              = 0x08,
        COMPRESSION_ERROR   = 0x09,
        CONNECT_ERROR       = 0x0a,
        ENHANCE_YOUR_CALM   = 0x0b,
        INADEQUATE_SECURITY = 0x0c,
        HTTP_1_1_REQUIRED   = 0x0d,
    };

    enum Flag : uint8_t {
        FLAG_ACK          = 0x01,
        FLAG_END_STREAM   = 0x01,
        FLAG_END_HEADERS  = 0x04,
        FLAG_PADDED       = 0x08,
        FLAG_PRIORITY     = 0x20,
    };

    constexpr static uint32_t MASK_31BIT = 0x7fffffff;

    template<typename T = uint8_t>
    T& ext(size_t offset) {
        auto end = (char*)(this + 1);
        return *(T*)(end + offset);
    }
    template<typename T = uint8_t>
    const T& ext(size_t offset) const {
        auto end = (char*)(this + 1);
        return *(T*)(end + offset);
    }

    void byte_order_encode();
    void byte_order_decode();
}__attribute__((packed));
static_assert(sizeof(FrameHeader) == 9, "");

struct Setting {
    uint16_t id;
    uint32_t value;

    enum Id : uint16_t {
        HEADER_TABLE_SIZE      = 1,
        ENABLE_PUSH            = 2,
        MAX_CONCURRENT_STREAMS = 3,
        INITIAL_WINDOW_SIZE    = 4,
        MAX_FRAME_SIZE         = 5,
        MAX_HEADER_LIST_SIZE   = 6,
    };
}__attribute__((packed));

class H2Stream;

class H2Connection {
public:
    H2Connection(IStream* stream, bool ownership);

    // Connection lifecycle
    int send_preface();
    int recv_preface();
    int send_settings(const Setting* settings, size_t count);
    int send_goaway(uint32_t last_stream_id, uint32_t error_code, const void* debug_data = nullptr, size_t debug_len = 0);

    // Stream management
    H2Stream create_stream();
    H2Stream accept_stream();

    // Per-stream data transfer (called by H2Stream)
    int send_headers(uint32_t stream_id, const Headers& headers, bool end_stream);
    int recv_headers(uint32_t stream_id, Headers& headers, bool* end_stream = nullptr);
    int send_data(uint32_t stream_id, const iovec* iov, int iovcnt, bool end_stream);
    ssize_t recv_data(uint32_t stream_id, iovec* iov, int iovcnt, bool* end_stream = nullptr);

    // Per-stream control (called by H2Stream)
    int reset_stream(uint32_t stream_id, uint32_t error_code);
    int close_stream(uint32_t stream_id);
    int update_stream_window(uint32_t stream_id, uint32_t increment);

    // Control frames (connection-level)
    int send_ping(const void* opaque_data = nullptr);

    // Frame-level access (for advanced use)
    int read_frame(int stream_id, void* buf, size_t len);
    int write_frame(int stream_id, const void* buf, size_t len);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

class H2Stream {
public:
    // Stream states as defined in RFC 9113 Section 5.1
    enum class State : uint32_t {
        Idle = 0,
        ReservedLocal,
        ReservedRemote,
        Open,
        HalfClosedLocal,
        HalfClosedRemote,
        Closed
    };

    H2Stream(H2Connection* conn, uint32_t id)
        : _conn(conn), _id(id), _state(State::Open) {}
    H2Stream(const H2Stream&) = default;

    uint32_t id() const { return _id; }
    State state() const { return _state; }
    operator bool() const { return _conn; }

    int send_headers(const Headers& headers, bool end_stream = false) {
        return _conn->send_headers(_id, headers, end_stream);
    }
    int recv_headers(Headers& headers, bool* end_stream = nullptr) {
        return _conn->recv_headers(_id, headers, end_stream);
    }
    int send_data(const void* data, size_t len, bool end_stream = false) {
        iovec iov{(void*)data, len};
        return _conn->send_data(_id, &iov, 1, end_stream);
    }
    int send_data(const iovec* iov, int iovcnt, bool end_stream = false) {
        return _conn->send_data(_id, iov, iovcnt, end_stream);
    }
    ssize_t recv_data(void* buf, size_t len, bool* end_stream = nullptr) {
        iovec iov{buf, len};
        return _conn->recv_data(_id, &iov, 1, end_stream);
    }
    ssize_t recv_data(iovec* iov, int iovcnt, bool* end_stream = nullptr) {
        return _conn->recv_data(_id, iov, iovcnt, end_stream);
    }

    int reset(uint32_t error_code) {
        _state = State::Closed;
        return _conn->reset_stream(_id, error_code);
    }
    int close() {
        _state = State::Closed;
        return _conn->close_stream(_id);
    }

    int update_window(uint32_t increment) {
        return _conn->update_stream_window(_id, increment);
    }

private:
    H2Connection* _conn;
    uint32_t _id;
    State _state;
};

}
}
}
