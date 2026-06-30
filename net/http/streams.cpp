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

#include "streams.h"
#include <unordered_map>
#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/common/conststr.h>
#include <photon/common/string_view.h>
#include <photon/thread/thread.h>
#include "huffman/codec.h"

namespace photon {
namespace net {
namespace http {

struct DataFrameHeader : public FrameHeader {
    size_t   size()    const { return sizeof(FrameHeader) + padded(); }
}__attribute__((packed));
static_assert(sizeof(DataFrameHeader) == 9, "");

struct HeadersFrameHeader : public FrameHeader {
    size_t   size()         const { return sizeof(FrameHeader) + padded() + priority() * 5; }
    bool     exclusive()  const { return priority() ? (ext(padded()) >> 31) : 0; }
    uint32_t stream_dep() const { return priority() ? (ext<uint32_t>(padded()) & MASK_31BIT) : 0; }
    uint8_t  weight()     const { return priority() ?  ext(padded() + 4) : 0; }
    void     set_priority(uint32_t stream_dep, uint8_t exclusive, uint8_t weight) {
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
                    ext(padded()) = (exclusive << 31) | (stream_dep & MASK_31BIT);
                    ext(padded() + 4) = weight;
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
                    FrameHeader::set_priority();
    }
    void byte_order_encode();
    void byte_order_decode();
}__attribute__((packed));
static_assert(sizeof(HeadersFrameHeader) == 9, "");

struct PriorityFrameHeader : public FrameHeader {
    uint32_t stream_dependency : 31;
    uint8_t  exclusive : 1;
    uint8_t  weight;
    void byte_order_encode();
    void byte_order_decode();
    size_t   size()       const { return sizeof(*this); }
}__attribute__((packed));
static_assert(sizeof(PriorityFrameHeader) == 14, "");

struct ResetStreamFrameHeader : public FrameHeader {
    uint32_t error_code;
    void byte_order_encode();
    void byte_order_decode();
    size_t   size()       const { return sizeof(*this); }
}__attribute__((packed));
static_assert(sizeof(ResetStreamFrameHeader) == 13, "");

struct SettingsFrameHeader : public FrameHeader {
    void byte_order_encode();
    void byte_order_decode();
    void _bswap_all();

    size_t   size()       const { return sizeof(*this); }
    size_t   count()        const {
        assert(length % sizeof(Setting) == 0);
        return length / sizeof(Setting);
    }
    Setting& operator[](size_t i) const {
        assert(i < count());
        return ((Setting*)&ext(0))[i];
    }
}__attribute__((packed));
static_assert(sizeof(SettingsFrameHeader) == 9, "");

struct PushPromiseFrameHeader : public FrameHeader {
    size_t   size()       const { return sizeof(*this) + padded() + 4; }
    const uint32_t& promise_stream_id() const { return ext<uint32_t>(padded()); }
    void set_promise_stream_id(uint32_t id) { ext<uint32_t>(padded()) = id & MASK_31BIT; }
    void byte_order_encode();
    void byte_order_decode();
}__attribute__((packed));
static_assert(sizeof(PushPromiseFrameHeader) == 9, "");

struct PingFrameHeader : public FrameHeader {
    char data[8];
    size_t   size()       const { return sizeof(*this); }
}__attribute__((packed));
static_assert(sizeof(PingFrameHeader) == 17, "");

struct GoawayFrameHeader : public FrameHeader {
    uint32_t last_stream_id;
    uint32_t error_code;
    void byte_order_encode();
    void byte_order_decode();
    size_t   size()       const { return sizeof(*this); }
    bool has_additional_data() const { return length > size(); }
}__attribute__((packed));
static_assert(sizeof(GoawayFrameHeader) == 17, "");

struct WindowUpdateFrameHeader : public FrameHeader {
    uint32_t window_size_increment;
    void byte_order_encode();
    void byte_order_decode();
    size_t   size()       const { return sizeof(*this); }
}__attribute__((packed));
static_assert(sizeof(WindowUpdateFrameHeader) == 13, "");

struct ContinuationFrameHeader : public FrameHeader {
    size_t   size()       const { return sizeof(*this); }
}__attribute__((packed));
static_assert(sizeof(ContinuationFrameHeader) == 9, "");

static void LE_bswap32(void* p) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    auto p_ = (uint32_t*)p;
    *p_ = __builtin_bswap32(*p_);
#endif
}
static void LE_bswap16(void* p) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    auto p_ = (uint16_t*)p;
    *p_ = __builtin_bswap16(*p_);
#endif
}

void FrameHeader::byte_order_encode() {
    LE_bswap32(this);
    stream_id &= MASK_31BIT;
    LE_bswap32(&stream_id);
}

void FrameHeader::byte_order_decode() {
    LE_bswap32(this);
    LE_bswap32(&stream_id);
    stream_id &= MASK_31BIT;
}
void HeadersFrameHeader::byte_order_encode() {
    FrameHeader::byte_order_encode();
    if (padded())
        LE_bswap32(&ext<uint32_t>(1));
}
void HeadersFrameHeader::byte_order_decode() {
    FrameHeader::byte_order_decode();
    if (padded())
        LE_bswap32(&ext<uint32_t>(1));
}
void PriorityFrameHeader::byte_order_encode() {
    FrameHeader::byte_order_encode();
    LE_bswap32(&ext(0));
}
void PriorityFrameHeader::byte_order_decode() {
    FrameHeader::byte_order_decode();
    LE_bswap32(&ext(0));
}
void ResetStreamFrameHeader::byte_order_encode() {
    FrameHeader::byte_order_encode();
    LE_bswap32(&error_code);
}
void ResetStreamFrameHeader::byte_order_decode() {
    FrameHeader::byte_order_decode();
    LE_bswap32(&error_code);
}
void SettingsFrameHeader::_bswap_all() {
    for (size_t i = 0; i < count(); ++i) {
        auto& p = (*this)[i];
        LE_bswap16(&p.id);
        LE_bswap32(&p.value);
    }
}
void SettingsFrameHeader::byte_order_encode() {
    _bswap_all();
    FrameHeader::byte_order_encode();
}
void SettingsFrameHeader::byte_order_decode() {
    FrameHeader::byte_order_decode();
    _bswap_all();
}
void PushPromiseFrameHeader::byte_order_encode() {
    FrameHeader::byte_order_encode();
    auto& psid = ext<uint32_t>(padded());
    psid &= MASK_31BIT; LE_bswap32(&psid);
}
void PushPromiseFrameHeader::byte_order_decode() {
    FrameHeader::byte_order_decode();
    auto& psid = ext<uint32_t>(padded());
    LE_bswap32(&psid); psid &= MASK_31BIT;
}
void GoawayFrameHeader::byte_order_encode() {
    FrameHeader::byte_order_encode();
    last_stream_id &= MASK_31BIT;
    LE_bswap32(&last_stream_id);
    LE_bswap32(&error_code);
}
void GoawayFrameHeader::byte_order_decode() {
    FrameHeader::byte_order_decode();
    LE_bswap32(&last_stream_id);
    last_stream_id &= MASK_31BIT;
    LE_bswap32(&error_code);
}
void WindowUpdateFrameHeader::byte_order_encode() {
    FrameHeader::byte_order_encode();
    LE_bswap32(&window_size_increment);
}
void WindowUpdateFrameHeader::byte_order_decode() {
    FrameHeader::byte_order_decode();
    LE_bswap32(&window_size_increment);
}

// HPACK integer encoding/decoding (RFC 7541 Section 5.1)

// Field representation masks (RFC 7541 Section 6)
constexpr uint8_t HPACK_INDEXED         = 0x80;  // Section 6.1
constexpr uint8_t HPACK_LITERAL_INDEXED = 0x40;  // Section 6.2.1
constexpr uint8_t HPACK_HUFFMAN         = 0x80;  // Huffman flag for strings

inline uint8_t make_octet(uint8_t value, uint8_t flags, uint8_t prefix_length) {
    assert(1 <= prefix_length && prefix_length <= 8);
    assert(flags < (1U << (8 - prefix_length)));
    assert(value < (1U << prefix_length));
    return value | (flags << prefix_length);
}

static void hpack_encode_integer(char*& ptr, uint8_t flags, uint8_t prefix_length, uint64_t x) {
    assert(1 <= prefix_length && prefix_length <= 8);
    assert(flags < (1U << (8 - prefix_length)));
    auto max_prefix = (1U << prefix_length) - 1U;
    if (x < max_prefix) {
        *ptr++ = make_octet(x, flags, prefix_length);
    } else {
        *ptr++ = make_octet(max_prefix, flags, prefix_length);
        x -= max_prefix;
        while (x >= 128) {
            *ptr++ = x % 128 + 128;
            x /= 128;
        }
        *ptr++ = x;
    }
}

struct integer {
    uint64_t value;
    uint8_t flags;
    operator uint64_t() const { return value; }
};

static integer hpack_decode_integer(const char*& ptr, uint8_t prefix_length) {
    uint8_t octet = *ptr++;
    uint8_t flags = octet >> prefix_length;
    uint8_t mask = (1U << prefix_length) - 1;
    uint8_t prefix = octet & mask;
    if (prefix < mask)
        return {prefix, flags};

    uint64_t x = 0;
    for (int i = 9; i; --i) {
        uint8_t next = *ptr++;
        if (next >= 128) {
            x += next - 128;
            x *= 128;
        } else {
            x += next;
            break;
        }
    };
    return {prefix + x, flags};
}

// HPACK string encoding (RFC 7541 Section 5.2)
// Returns number of bytes written, or -1 on error
static ssize_t hpack_encode_string(char*& ptr, std::string_view str, bool use_huffman = true) {
    char* start = ptr;
    if (use_huffman) {
        auto encoded_len = huffman_encoded_length(str);
        hpack_encode_integer(ptr, 1, 7, encoded_len);
        ssize_t ret = huffman_encode(str, ptr, ptr + encoded_len);
        if (ret < 0) return -1;
        ptr += ret;
    } else {
        hpack_encode_integer(ptr, 0, 7, str.size());
        memcpy(ptr, str.data(), str.size());
        ptr += str.size();
    }
    return ptr - start;
}

// HPACK string decoding (RFC 7541 Section 5.2)
static ssize_t hpack_decode_string(const char*& ptr, const char* end, char* out_buf, size_t out_buf_size) {
    if (ptr >= end) return -1;
    bool huffman_encoded = (*ptr & HPACK_HUFFMAN) != 0;
    auto len = hpack_decode_integer(ptr, 7).value;
    if (ptr + len > end) return -1;
    if (len > out_buf_size) return -1;
    if (huffman_encoded) {
        ssize_t decoded_len = huffman_decode(std::string_view(ptr, len), out_buf, out_buf_size);
        if (decoded_len < 0) return -1;
        ptr += len;
        return decoded_len;
    } else {
        memcpy(out_buf, ptr, len);
        ptr += len;
        return (ssize_t)len;
    }
}

// HPACK static table (RFC 7541 Appendix A)
const static auto static_header_names = ConstString::make_compact_str_array<uint16_t>(
    /* 0  */  TSTRING("_"),
    /* 1  */  TSTRING(":authority"),
    /* 2  */  TSTRING(":method"),
    /* 3  */  TSTRING(":method"),
    /* 4  */  TSTRING(":path"),
    /* 5  */  TSTRING(":path"),
    /* 6  */  TSTRING(":scheme"),
    /* 7  */  TSTRING(":scheme"),
    /* 8  */  TSTRING(":status"),
    /* 9  */  TSTRING(":status"),
    /* 10 */  TSTRING(":status"),
    /* 11 */  TSTRING(":status"),
    /* 12 */  TSTRING(":status"),
    /* 13 */  TSTRING(":status"),
    /* 14 */  TSTRING(":status"),
    /* 15 */  TSTRING("accept-charset"),
    /* 16 */  TSTRING("accept-encoding"),
    /* 17 */  TSTRING("accept-language"),
    /* 18 */  TSTRING("accept-ranges"),
    /* 19 */  TSTRING("accept"),
    /* 20 */  TSTRING("access-control-allow-origin"),
    /* 21 */  TSTRING("age"),
    /* 22 */  TSTRING("allow"),
    /* 23 */  TSTRING("authorization"),
    /* 24 */  TSTRING("cache-control"),
    /* 25 */  TSTRING("content-disposition"),
    /* 26 */  TSTRING("content-encoding"),
    /* 27 */  TSTRING("content-language"),
    /* 28 */  TSTRING("content-length"),
    /* 29 */  TSTRING("content-location"),
    /* 30 */  TSTRING("content-range"),
    /* 31 */  TSTRING("content-type"),
    /* 32 */  TSTRING("cookie"),
    /* 33 */  TSTRING("date"),
    /* 34 */  TSTRING("etag"),
    /* 35 */  TSTRING("expect"),
    /* 36 */  TSTRING("expires"),
    /* 37 */  TSTRING("from"),
    /* 38 */  TSTRING("host"),
    /* 39 */  TSTRING("if-match"),
    /* 40 */  TSTRING("if-modified-since"),
    /* 41 */  TSTRING("if-none-match"),
    /* 42 */  TSTRING("if-range"),
    /* 43 */  TSTRING("if-unmodified-since"),
    /* 44 */  TSTRING("last-modified"),
    /* 45 */  TSTRING("link"),
    /* 46 */  TSTRING("location"),
    /* 47 */  TSTRING("max-forwards"),
    /* 48 */  TSTRING("proxy-authenticate"),
    /* 49 */  TSTRING("proxy-authorization"),
    /* 50 */  TSTRING("range"),
    /* 51 */  TSTRING("referer"),
    /* 52 */  TSTRING("refresh"),
    /* 53 */  TSTRING("retry-after"),
    /* 54 */  TSTRING("server"),
    /* 55 */  TSTRING("set-cookie"),
    /* 56 */  TSTRING("strict-transport-security"),
    /* 57 */  TSTRING("transfer-encoding"),
    /* 58 */  TSTRING("user-agent"),
    /* 59 */  TSTRING("vary"),
    /* 60 */  TSTRING("via"),
    /* 61 */  TSTRING("www-authenticate"));

const static auto static_header_values = ConstString::make_compact_str_array<uint16_t>(
    /* 0  */  TSTRING("_"),
    /* 1  */  TSTRING(""),
    /* 2  */  TSTRING("GET"),
    /* 3  */  TSTRING("POST"),
    /* 4  */  TSTRING("/"),
    /* 5  */  TSTRING("/index.html"),
    /* 6  */  TSTRING("http"),
    /* 7  */  TSTRING("https"),
    /* 8  */  TSTRING("200"),
    /* 9  */  TSTRING("204"),
    /* 10 */  TSTRING("206"),
    /* 11 */  TSTRING("304"),
    /* 12 */  TSTRING("400"),
    /* 13 */  TSTRING("404"),
    /* 14 */  TSTRING("500"),
    /* 15 */  TSTRING(""),
    /* 16 */  TSTRING("gzip, deflate"));

static int hpack_find_static_entry(std::string_view name, std::string_view value) {
    static constexpr struct { uint8_t hash; uint8_t index; } index[] = {
        {0x02,32},{0x0b,58},{0x14,16},{0x19,35},
        {0x21,17},{0x25,25},{0x26,48},{0x2a,28},
        {0x2f,52},{0x37, 4},{0x3f, 6},{0x43,51},
        {0x47,54},{0x4a,44},{0x4c,37},{0x4e,33},
        {0x50,19},{0x52,59},{0x57,15},{0x5d,50},
        {0x65,30},{0x67,22},{0x69,46},{0x75,18},
        {0x79,42},{0x7b, 1},{0x86, 8},{0x89,61},
        {0x93,55},{0x9e,41},{0x9f,26},{0xa1,34},
        {0xa4,40},{0xa8,49},{0xac,27},{0xad,53},
        {0xb0,36},{0xb1,29},{0xb8,56},{0xb9,20},
        {0xc1,23},{0xc2,24},{0xc9,57},{0xce,45},
        {0xcf,43},{0xd8,60},{0xd9,39},{0xda,31},
        {0xdb, 2},{0xe5,21},{0xeb,47},{0xf6,38},
    };

    uint8_t h = 0;
    for (auto c : name)
        h = h * 185 + (unsigned char)c;
    auto it = std::lower_bound(std::begin(index), std::end(index), h,
        [](const auto& e, uint8_t h) { return e.hash < h; });
    if (it == std::end(index) || it->hash != h ||
        name != static_header_names[it->index]) return 0;
    size_t i = it->index;
    if (i < static_header_values.size()) {
        do { if (value == static_header_values[i]) return (int)i; }
        while(++i < static_header_values.size() &&
            name == static_header_names[i]);
    }
    return -(int)it->index; // only name matches
}

enum class HpackIndexing {
    Incremental,  // RFC 7541 6.2.1 — flags=1, prefix=6
    NoIndex,      // RFC 7541 6.2.2 — flags=0, prefix=4
    NeverIndex,   // RFC 7541 6.2.3 — flags=1, prefix=4
};

// HPACK header field encoding (RFC 7541 Section 6)
static ssize_t hpack_encode_field(char*& ptr,
            std::string_view name, std::string_view value,
            HpackIndexing indexing = HpackIndexing::Incremental) {
    char* start = ptr;
    int static_idx = hpack_find_static_entry(name, value);

    uint8_t flags = 1, prefix = 6;
    switch (indexing) {
        case HpackIndexing::Incremental: flags = 1; prefix = 6; break;
        case HpackIndexing::NoIndex:     flags = 0; prefix = 4; break;
        case HpackIndexing::NeverIndex:  flags = 1; prefix = 4; break;
    }

    if (static_idx > 0) { // Indexed Header Field — exact match (6.1)
        hpack_encode_integer(ptr, 1, 7, static_idx);
    } else if (static_idx < 0) { // Literal Header Field with indexed name
        hpack_encode_integer(ptr, flags, prefix, -static_idx);
        if (hpack_encode_string(ptr, value) < 0) return -1;
    } else { // Literal Header Field — new name
        hpack_encode_integer(ptr, flags, prefix, 0);
        if (hpack_encode_string(ptr, name) < 0) return -1;
        if (hpack_encode_string(ptr, value) < 0) return -1;
    }
    return ptr - start;
}

// Public API: Encode a list of headers into an HPACK-encoded header block
// headers: array of name-value pairs (name1, value1, name2, value2, ...)
// num_headers: number of header pairs
// out_buf: output buffer for encoded data
// out_buf_size: size of output buffer
// Returns: number of bytes written, or -1 on error
static ssize_t hpack_encode_headers(const Headers& headers,
                                     char* out_buf, size_t out_buf_size) {
    char* ptr = out_buf;
    char* end = out_buf + out_buf_size;

    for (auto kv : headers) {
        ssize_t ret = hpack_encode_field(ptr, kv.first, kv.second);
        if (ret < 0) return -1;
        if (ptr > end) return -1;
    }

    return ptr - out_buf;
}

// Decode HPACK-encoded header block into name-value pairs
// encoded_buf: HPACK-encoded header block
// encoded_len: length of encoded data
// out_headers: output array of string pointers (name1, value1, name2, value2, ...)
// max_headers: maximum number of header pairs to decode
// Returns: number of header pairs decoded, or -1 on error
static int hpack_decode_headers(const char* encoded_buf, size_t encoded_len,
                                 Headers& out_headers) {
    const char* ptr = encoded_buf;
    const char* end = encoded_buf + encoded_len;
    char name_buf[1024];
    char value_buf[4096];

    while (ptr < end) {
        uint8_t first_byte = *ptr;

        if (first_byte & HPACK_INDEXED) {
            // Indexed Header Field (Section 6.1)
            uint64_t index = hpack_decode_integer(ptr, 7).value;
            if (index == 0 || index >= static_header_names.size()) return -1;
            auto value = (index < static_header_values.size())
                ? static_header_values[index] : std::string_view{};
            out_headers.insert(static_header_names[index], value);
        } else if (first_byte & HPACK_LITERAL_INDEXED) {
            // Literal Header Field with Incremental Indexing (Section 6.2.1)
            uint64_t name_index = hpack_decode_integer(ptr, 6).value;

            std::string_view name_sv;
            if (name_index > 0) {
                if (name_index >= static_header_names.size()) return -1;
                name_sv = static_header_names.at(name_index);
            } else {
                ssize_t name_len = hpack_decode_string(ptr, end, name_buf, sizeof(name_buf) - 1);
                if (name_len < 0) return -1;
                name_buf[name_len] = '\0';
                name_sv = name_buf;
            }

            ssize_t value_len = hpack_decode_string(ptr, end, value_buf, sizeof(value_buf) - 1);
            if (value_len < 0) return -1;
            value_buf[value_len] = '\0';

            out_headers.insert(name_sv, value_buf);
        } else {
            // Literal Without Indexing (6.2.2) or Never Indexed (6.2.3) — prefix=4
            uint64_t name_index = hpack_decode_integer(ptr, 4).value;

            std::string_view name_sv;
            if (name_index > 0) {
                if (name_index >= static_header_names.size()) return -1;
                name_sv = static_header_names.at(name_index);
            } else {
                ssize_t name_len = hpack_decode_string(ptr, end, name_buf, sizeof(name_buf) - 1);
                if (name_len < 0) return -1;
                name_buf[name_len] = '\0';
                name_sv = name_buf;
            }

            ssize_t value_len = hpack_decode_string(ptr, end, value_buf, sizeof(value_buf) - 1);
            if (value_len < 0) return -1;
            value_buf[value_len] = '\0';

            out_headers.insert(name_sv, value_buf);
        }
    }

    return 0;
}

const static char http2_preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
// const static char http2_alpn[] = "h2";

class H2Connection::Impl {
public:
    IStream* _stream;
    mutex _mutex;
    uint32_t _next_stream_id = 0;
    bool _ownership;

    explicit Impl(IStream* stream, bool ownership) :
        _stream(stream), _ownership(ownership) {
        _next_stream_id = 1;
    }

    ~Impl() {
        if (_ownership)
            delete _stream;
    }

    template<typename T, size_t N = 20>
    struct Extension : public T {
        static_assert(std::is_base_of<FrameHeader, T>::value, "T must be FrameHeader");
        static_assert(sizeof(T) <= N, "T must be smaller than N");
        Extension() = default;
        Extension(uint8_t type, uint32_t stream_id) {
            this->type = type;
            this->stream_id = stream_id;
            this->flags = 0;
            this->length = 0;
        }
        char _extension[N - sizeof(T)];
    };

    using GenericFrame = Extension<FrameHeader>;

    int recv_headers(uint32_t stream_id, Headers& headers, bool* end_stream) {
        if (_pending_recv.length && _pending_recv.stream_id == stream_id &&
            _pending_recv.type == FrameHeader::HEADERS) {
            _frame = _pending_recv;
            _pending_recv.length = 0;
        } else {
            auto frame = read_frame_header(stream_id);
            if (!_frame.length)
                LOG_ERROR_RETURN(EIO, -1, "failed to read frame for stream ", stream_id);
            _frame = frame;
        }
        if (_frame.type != FrameHeader::HEADERS)
            LOG_ERROR_RETURN(EPROTO, -1, "expected HEADERS frame");
        if (end_stream) *end_stream = _frame.end_stream();

        char buf[8192];
        size_t payload_len = _frame.length;
        if (payload_len > sizeof(buf))
            LOG_ERROR_RETURN(ENOBUFS, -1, "header block too large");
        if (payload_len > 0) {
            auto ret = _stream->read(buf, payload_len);
            if (ret < 0) LOG_ERRNO_RETURN(0, -1, "failed to read header block");
            if ((size_t)ret < payload_len) LOG_ERROR_RETURN(EIO, -1, "incomplete header block");
        }

        return hpack_decode_headers(buf, payload_len, headers);
    }

    int send_data(uint32_t stream_id, const iovec* iov, int iovcnt, bool end_stream) {
        size_t total = iovector_view((iovec*)iov, iovcnt).sum();
        Extension<DataFrameHeader> h(FrameHeader::DATA, stream_id);
        h.length = total;
        if (end_stream) h.set_end_stream();
        return write_frame(h, iov, iovcnt);
    }
    struct HeadersOptions {
        uint8_t padding = 0;
        bool end_stream = false;
        bool priority   = false;
        uint8_t weight  = 0;
        uint32_t stream_dependency;
        uint8_t exclusive;
    };
    int write_headers(uint32_t stream_id, const iovec* iov, int iovcnt, HeadersOptions opt) {
        size_t fields_size = iovector_view((iovec*)iov, iovcnt).sum();
        Extension<HeadersFrameHeader> h(FrameHeader::HEADERS, stream_id);
        if (opt.end_stream) h.set_end_stream();
        if (opt.padding)  { h.set_padded(); h.ext(0) = opt.padding; }
        if (opt.priority)   h.set_priority(opt.stream_dependency,
                             opt.exclusive, opt.weight);
        h.set_end_headers();
        h.length = h.size() - sizeof(FrameHeader) + fields_size + opt.padding;
        return write_frame(h, iov, iovcnt);
    }

    // Write headers using HPACK encoding
    // headers: array of name-value pairs (name1, value1, name2, value2, ...)
    // num_headers: number of header pairs
    int write_headers_hpack(uint32_t stream_id, const Headers& headers, HeadersOptions opt) {
        char hpack_buf[8192];
        ssize_t encoded_len = hpack_encode_headers(headers, hpack_buf, sizeof(hpack_buf));
        if (encoded_len < 0)
            LOG_ERROR_RETURN(EINVAL, -1, "failed to encode headers with HPACK");

        iovec iov{hpack_buf, (size_t)encoded_len};
        return write_headers(stream_id, &iov, 1, opt);
    }

    // Decode HPACK-encoded header block from a buffer
    // encoded_buf: HPACK-encoded header data
    // encoded_len: length of encoded data
    // out_headers: output array of string pointers (caller must free with free())
    // max_headers: maximum number of header pairs
    // Returns: number of header pairs decoded, or -1 on error
    int decode_headers_hpack(const char* encoded_buf, size_t encoded_len,
                             Headers& headers) {
        return hpack_decode_headers(encoded_buf, encoded_len, headers);
    }

    int write_priority(uint32_t stream_id, bool exclusive, uint32_t stream_dependency, uint8_t weight) {
        Extension<PriorityFrameHeader> h(FrameHeader::PRIORITY, stream_id);
        h.length = h.size() - sizeof(FrameHeader);
        h.exclusive = exclusive;
        h.stream_dependency = stream_dependency;
        h.weight = weight;
        return write_frame(h);
    }
    int write_push_promise(uint32_t stream_id, uint32_t promised_stream_id, uint8_t padding,
            bool end_headers, const iovec* iov, int iovcnt) {
        size_t fields_size = iovector_view((iovec*)iov, iovcnt).sum();
        Extension<PushPromiseFrameHeader> h(FrameHeader::PUSH_PROMISE, stream_id);
        if (end_headers) h.set_end_headers();
        if (padding)   { h.set_padded(); h.ext(0) = padding; }
        h.set_promise_stream_id(promised_stream_id);
        h.length = h.size() - sizeof(FrameHeader) + fields_size + padding;
        return write_frame(h, iov, iovcnt);
    }
    int write_ping(bool ack, const void* data) {
        Extension<PingFrameHeader> h(FrameHeader::PING, 0);
        if (ack) h.set_ack();
        memcpy(h.data, data, 8);
        h.length = h.size() - sizeof(FrameHeader);
        return write_frame(h);
    }
    int send_goaway(uint32_t last_stream_id, uint32_t error_code, const void* debug_data, size_t debug_len) {
        iovec iov{(void*)debug_data, debug_len};
        Extension<GoawayFrameHeader> h(FrameHeader::GOAWAY, 0);
        h.last_stream_id = last_stream_id;
        h.error_code = error_code;
        h.length = h.size() - sizeof(FrameHeader) + debug_len;
        return write_frame(h, &iov, 1);
    }
    int write_window_update(uint32_t stream_id, uint32_t increment) {
        Extension<WindowUpdateFrameHeader> h(FrameHeader::WINDOW_UPDATE, stream_id);
        h.window_size_increment = increment;
        h.length = h.size() - sizeof(FrameHeader);
        return write_frame(h);
    }
    int write_continuation(uint32_t stream_id, bool end_headers, const iovec* iov, int iovcnt) {
        size_t fields_size = iovector_view((iovec*)iov, iovcnt).sum();
        Extension<ContinuationFrameHeader> h(FrameHeader::CONTINUATION, stream_id);
        if (end_headers) h.set_end_headers();
        h.length = h.size() - sizeof(FrameHeader) + fields_size;
        return write_frame(h, iov, iovcnt);
    }

    // Connection lifecycle implementation
    int send_preface() {
        auto ret = _stream->write(http2_preface, sizeof(http2_preface) - 1);
        if (ret != (ssize_t)(sizeof(http2_preface) - 1))
            LOG_ERRNO_RETURN(0, -1, "failed to send connection preface");
        return 0;
    }

    int recv_preface() {
        char buf[sizeof(http2_preface) - 1];
        ssize_t ret = _stream->read(buf, sizeof(buf));
        if (ret != sizeof(buf))
            LOG_ERROR_RETURN(EIO, -1, "failed to read connection preface");
        if (memcmp(buf, http2_preface, sizeof(buf)) != 0)
            LOG_ERROR_RETURN(EPROTO, -1, "invalid connection preface");
        return 0;
    }

    int send_settings(const Setting* settings, size_t count) {
        Extension<SettingsFrameHeader> h(FrameHeader::SETTINGS, 0);
        h.length = count * sizeof(Setting);
        iovec iov{(void*)settings, count * sizeof(*settings)};
        return write_frame(h, &iov, 1);
    }

    uint32_t create_stream_id() {
        uint32_t stream_id = _next_stream_id;
        _next_stream_id += 2;
        return stream_id;
    }

    uint32_t accept_stream_id() {
        while (true) {
            if (_do_read_frame_header() < 0) return 0;
            if (process_generic_frame()) continue;
            if (_frame.type == FrameHeader::HEADERS && (_frame.stream_id & 1)) {
                _pending_recv = _frame;
                return _frame.stream_id;
            }
            stream_error(_frame.stream_id, FrameHeader::PROTOCOL_ERROR);
        }
    }

    // Per-stream data transfer
    int send_headers(uint32_t stream_id, const Headers& headers, bool end_stream) {
        HeadersOptions opt;
        opt.end_stream = end_stream;
        return write_headers_hpack(stream_id, headers, opt);
    }

    ssize_t recv_data(uint32_t stream_id, iovec* iov, int iovcnt, bool* end_stream) {
        auto frame = read_frame_header(stream_id);
        if (_frame.length == 0) {
            if (end_stream) *end_stream = _frame.end_stream();
            return 0;
        }
        if (frame.type != FrameHeader::DATA)
            LOG_ERROR_RETURN(EPROTO, -1, "expected DATA frame");
        if (end_stream) *end_stream = _frame.end_stream();
        size_t remaining = _frame.length;
        ssize_t total = 0;
        for (int i = 0; i < iovcnt && remaining > 0; i++) {
            size_t to_read = std::min(remaining, iov[i].iov_len);
            ssize_t ret = _stream->read(iov[i].iov_base, to_read);
            if (ret < 0) LOG_ERRNO_RETURN(0, -1, "failed to read data");
            remaining -= ret;
            total += ret;
        }
        return total;
    }

    // Per-stream control
    int reset_stream(uint32_t stream_id, uint32_t error_code) {
        Extension<ResetStreamFrameHeader> h(FrameHeader::RST_STREAM, stream_id);
        h.length = h.size() - sizeof(FrameHeader);
        h.error_code = error_code;
        return write_frame(h);
    }

    int close_stream(uint32_t stream_id) {
        return 0;
    }

    int update_stream_window(uint32_t stream_id, uint32_t increment) {
        return write_window_update(stream_id, increment);
    }

    // Control frames implementation
    int send_ping(const void* opaque_data) {
        uint8_t data[8] = {0};
        if (opaque_data)
            memcpy(data, opaque_data, 8);
        return write_ping(false, data);
    }

    template<typename T, size_t N>
    int write_frame(Extension<T, N>& h, const iovec* iov = nullptr, int iovcnt = 0) {
        h.byte_order_encode();
        DEFER(h.byte_order_decode());
        auto hdr_size = std::max(h.size(), sizeof(FrameHeader));
        return write_frame(&h, hdr_size, iov, iovcnt, h.padding());
    }
    int write_frame(const void* buf, size_t len, const iovec* iov, int iovcnt, uint8_t padding = 0) {
        IOVector iov_arr(iov, iovcnt);
        iov_arr.push_front({(void*)buf, len});
        const static char padding_buf[256] = {0};
        if (padding) iov_arr.push_back({(void*)padding_buf, padding});
        auto ret = _stream->writev(iov_arr.iovec(), iov_arr.iovcnt());
        if (ret < 0 || (size_t)ret != iov_arr.sum())
            LOG_ERROR_RETURN(EIO, -1, "failed to write frame");
        return 0;
    }

    GenericFrame _frame;
    GenericFrame _pending_recv = {};
    uint8_t _header_length = 0, _remaining_length = 0;
    int _do_read_frame_header() {
        if (_remaining_length) {
            assert(_header_length < sizeof(_frame));
            memmove(&_frame, (char*)&_frame + _header_length, _remaining_length);
        }
        auto ret = _stream->read((char*)&_frame + _remaining_length,
                                  sizeof(FrameHeader) - _remaining_length);
        if (ret < 0) LOG_ERRNO_RETURN(0, -1, "failed to read socket");
        if (ret < (ssize_t)sizeof(FrameHeader)) LOG_ERROR_RETURN(EBADF, -1, "socket closed");

        _frame.byte_order_decode();
        if (_frame.type > FrameHeader::CONTINUATION)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid frame type ", _frame.type);

        _header_length = 0;
        ret += _remaining_length;
        switch(_frame.type) {
            #define CASE(HEADER, Header) case FrameHeader::HEADER: \
                _header_length = ((Header ## FrameHeader*)&_frame)->size(); break;
            CASE(DATA, Data);
            CASE(HEADERS, Headers);
            CASE(PRIORITY, Priority);
            CASE(RST_STREAM, ResetStream);
            CASE(SETTINGS, Settings);
            CASE(PUSH_PROMISE, PushPromise);
            CASE(PING, Ping);
            CASE(GOAWAY, Goaway);
            CASE(WINDOW_UPDATE, WindowUpdate);
            CASE(CONTINUATION, Continuation);
            default: LOG_ERROR_RETURN(EINVAL, -1, "unknown frame type ", _frame.type);
            #undef CASE
        }
        if (likely(ret >= _header_length)) {
            _remaining_length = ret - _header_length;
        } else {
            _remaining_length = 0;
            ssize_t more = _header_length - ret;
            ret = _stream->read((char*)&_frame + ret, more);
            if (ret < 0) LOG_ERRNO_RETURN(0, -1, "failed to read socket");
            if (ret < more) LOG_ERROR_RETURN(EBADF, -1, "socket closed");
        }
        return 0;
    }

    bool process_generic_frame() {
        if (_frame.stream_id == 0) {
            switch (_frame.type) {
                case FrameHeader::SETTINGS:
                    if (!_frame.ack()) {
                        Extension<SettingsFrameHeader> h(FrameHeader::SETTINGS, 0);
                        h.set_ack();
                        h.length = 0;
                        write_frame(h);
                    }
                    return true;
                case FrameHeader::PING:
                    if (!_frame.ack()) {
                        write_ping(true, &_frame.ext(0));
                    }
                    return true;
                case FrameHeader::GOAWAY:
                    return true;
                case FrameHeader::WINDOW_UPDATE:
                    return true;
            }
        }
        return false;
    }

    void stream_error(uint32_t stream_id, uint32_t error_code) {
        Extension<ResetStreamFrameHeader> h(FrameHeader::RST_STREAM, stream_id);
        h.length = h.size() - sizeof(FrameHeader);
        h.error_code = error_code;
        write_frame(h);
    }

    photon::spinlock _readers_lock;
    std::unordered_map<uint32_t, std::pair<thread*, GenericFrame*>> _readers;
    GenericFrame read_frame_header(uint32_t stream_id) {
        GenericFrame result;
        memset(&result, 0, sizeof(result));
        int ret = _mutex.try_lock();
        if (ret < 0) {
            auto ok = ({ SCOPED_LOCK(_readers_lock);
                _readers.insert({stream_id, {CURRENT, &result}}).second; });
            if (!ok)
                LOG_ERROR_RETURN(EINVAL, result, "failed to insert reader");
            ret = thread_usleep(-1);
            if (!result.length) goto do_read;
            assert(result.stream_id == stream_id);
            { SCOPED_LOCK(_readers_lock);
            _readers.erase(stream_id); }
            return result;
        }

    do_read:
        DEFER(_mutex.unlock());
        while(true) {
            ret = _do_read_frame_header();
            if (ret < 0) return result;
            if (process_generic_frame()) continue;
            if (_frame.stream_id == stream_id) {
                result = _frame;
                auto th = [&]() -> thread* {
                    SCOPED_LOCK(_readers_lock);
                    if (_readers.empty()) return nullptr;
                    auto& p = _readers.begin()->second;
                    p.second->length = 0;
                    return p.first;
                }();
                if (th) thread_interrupt(th);
                return result;
            } else {
                auto th = [&]() -> thread* {
                    SCOPED_LOCK(_readers_lock);
                    auto it = _readers.find(_frame.stream_id);
                    if (it == _readers.end()) return nullptr;
                    auto& p = it->second;
                    *p.second = _frame;
                    return p.first;
                }();
                if (th) thread_interrupt(th);
                else stream_error(_frame.stream_id, FrameHeader::PROTOCOL_ERROR);
            }
        }
        return result;
    }

    int read_frame(int stream_id, void* buf, size_t len) {
        read_frame_header(stream_id);
        if (!_frame.length)
            LOG_ERROR_RETURN(EIO, -1, "failed to read frame for stream ", stream_id);
        size_t payload_len = _frame.length;
        if (payload_len > len)
            LOG_ERROR_RETURN(ENOBUFS, -1, "buffer too small");
        if (payload_len > 0) {
            auto ret = _stream->read(buf, payload_len);
            if (ret < 0) LOG_ERRNO_RETURN(0, -1, "failed to read frame payload");
            if ((size_t)ret < payload_len) LOG_ERROR_RETURN(EIO, -1, "incomplete frame payload");
        }
        return payload_len;
    }

    int write_frame(int stream_id, const void* buf, size_t len) {
        SCOPED_LOCK(_mutex);
        return (int)_stream->write(buf, len);
    }
};

H2Connection::H2Connection(IStream* stream, bool ownership)
    : _impl(std::make_unique<Impl>(stream, ownership)) {}

int H2Connection::send_preface() { return _impl->send_preface(); }
int H2Connection::recv_preface() { return _impl->recv_preface(); }
int H2Connection::send_settings(const Setting* settings, size_t count) { return _impl->send_settings(settings, count); }
int H2Connection::send_goaway(uint32_t last_stream_id, uint32_t error_code, const void* debug_data, size_t debug_len) { return _impl->send_goaway(last_stream_id, error_code, debug_data, debug_len); }

H2Stream H2Connection::create_stream() { return H2Stream(this, _impl->create_stream_id()); }
H2Stream H2Connection::accept_stream() {
    uint32_t id = _impl->accept_stream_id();
    return id ? H2Stream(this, id) : H2Stream(nullptr, 0);
}

int H2Connection::send_headers(uint32_t stream_id, const Headers& headers, bool end_stream) { return _impl->send_headers(stream_id, headers, end_stream); }
int H2Connection::recv_headers(uint32_t stream_id, Headers& headers, bool* end_stream) { return _impl->recv_headers(stream_id, headers, end_stream); }
int H2Connection::send_data(uint32_t stream_id, const iovec* iov, int iovcnt, bool end_stream) { return _impl->send_data(stream_id, iov, iovcnt, end_stream); }
ssize_t H2Connection::recv_data(uint32_t stream_id, iovec* iov, int iovcnt, bool* end_stream) { return _impl->recv_data(stream_id, iov, iovcnt, end_stream); }

int H2Connection::reset_stream(uint32_t stream_id, uint32_t error_code) { return _impl->reset_stream(stream_id, error_code); }
int H2Connection::close_stream(uint32_t stream_id) { return _impl->close_stream(stream_id); }
int H2Connection::update_stream_window(uint32_t stream_id, uint32_t increment) { return _impl->update_stream_window(stream_id, increment); }

int H2Connection::send_ping(const void* opaque_data) { return _impl->send_ping(opaque_data); }

int H2Connection::read_frame(int stream_id, void* buf, size_t len) { return _impl->read_frame(stream_id, buf, len); }
int H2Connection::write_frame(int stream_id, const void* buf, size_t len) { return _impl->write_frame(stream_id, buf, len); }

}
}
}
