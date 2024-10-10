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
#include <stdlib.h>
#include <inttypes.h>
#include <tuple>
#include <photon/net/socket.h>
#include <photon/common/estring.h>
#include <photon/common/tuple-assistance.h>
namespace photon {
namespace redis {

class BufferedStream;
class refstring : public std::string_view {
    BufferedStream* _bs = nullptr;
    void add_ref();
    void del_ref();
public:
    using std::string_view::string_view;
    refstring(BufferedStream* bs, std::string_view sv) :
            std::string_view(sv), _bs(bs) { add_ref(); }
    refstring(const refstring& rhs) :
            std::string_view(rhs), _bs(rhs._bs) { add_ref(); }
    refstring& operator = (const refstring& rhs) {
        if (this == &rhs) return *this;
        *(std::string_view*)this = rhs;
        del_ref();
        _bs = rhs._bs;
        add_ref();
        return *this;
    }
    void release() {
        del_ref();
        _bs = nullptr;
        (std::string_view&) *this = {};
    }
    ~refstring() { del_ref(); }
};

class simple_string : public refstring {
public:
    simple_string() = default;
    simple_string(refstring rs) : refstring(rs) { }
    using refstring::operator=;
    constexpr static char mark() { return '+'; }
};

class error_message : public refstring {
public:
    error_message() = default;
    error_message(refstring rs) : refstring(rs) { }
    using refstring::operator=;
    constexpr static char mark() { return '-'; }
};

class bulk_string : public refstring {
public:
    bulk_string() = default;
    bulk_string(refstring rs) : refstring(rs) { }
    using refstring::operator=;
    constexpr static char mark() { return '$'; }
};

class integer : public refstring {
public:
    int64_t val;
    integer() = default;
    integer(int64_t v) : val(v) { };
    integer(const refstring& rs) :
        val(((estring_view&)rs).to_int64()) { }
    using refstring::operator=;
    constexpr static char mark() { return ':'; }
};

class null { };

template<typename...Ts>
class array : public std::tuple<Ts...> {
public:
    array() = default;
    using std::tuple<Ts...>::tuple;
    using std::tuple<Ts...>::operator=;
    constexpr static char mark() { return '*'; }
};

template<typename...Ts>
array<Ts...> make_array(const Ts&...xs) {
    return {xs...};
}

struct any {
    constexpr static size_t MAX1 = std::max(sizeof(simple_string), sizeof(error_message));
    constexpr static size_t MAX2 = std::max(sizeof(bulk_string), sizeof(integer));
    constexpr static size_t MAX = std::max(MAX1, MAX2);
    char value[MAX] = {0}, mark = 0;

    template<typename T, typename = typename std::enable_if<
                    std::is_base_of<refstring, T>::value>::type>
    bool is_type() { return mark == T::mark(); }

    template<>
    bool is_type<integer>() { return mark == integer::mark() ||
                                     mark == array<>::mark(); }

    template<typename T, typename = typename std::enable_if<
                    std::is_base_of<refstring, T>::value>::type>
    T& get() {
        assert(is_type<T>());
        return *(T*)value;
    }

    template<typename T, typename = typename
        std::enable_if<std::is_base_of<refstring, T>::value>::type>
    void set(const T& x) {
        *(T*)value = x;
        mark = T::mark();
    }

    any() = default;
    template<typename T>
    any(const T& x) { set(x); }
    any(array<>, integer n) { set(n); mark = array<>::mark(); }

    template<typename T>
    any& operator = (const T& x) { return set(x), *this; }
};

using net::ISocketStream;

// template<typename T, typename = typename std::enable_if<
//     !std::is_base_of<std::string_view, T>::value>::type>
// inline constexpr size_t __MAX_SIZE(T) { return INT32_MAX; }

inline constexpr size_t __MAX_SIZE(std::string_view x) { return x.size(); }
inline constexpr size_t __MAX_SIZE(int64_t x) { return 32; }
inline constexpr size_t __MAX_SIZE(uint64_t x) { return 32; }
inline constexpr size_t __MAX_SIZE(char x) { return 1; }

constexpr static char CRLF[] = "\r\n";

class BufferedStream {
protected:
    ISocketStream* _s = nullptr;
    uint32_t _i = 0, _j = 0, _o = 0, _bufsize = 0, _refcnt = 0;
    char _xbuf[0];

    friend class refstring;

    size_t _sum() const { return 0; }
    template<typename T, typename...Ts>
    size_t _sum(T x, const Ts&...xs) {
        return x + _sum(xs...);
    }
    char* ibuf() { return _xbuf; }
    char* obuf() { return _xbuf + _bufsize; }
    std::string_view __getstring(size_t length);
    std::string_view __getline();

    explicit BufferedStream(ISocketStream* s, uint32_t bufsize) :
        _s(s), _bufsize(bufsize) { }

public:
    void flush(const void* extra_buffer = 0, size_t size = 0);
    bool flush_if_low_space(size_t threshold, const void* ebuf = 0, size_t size = 0) {
        return (_o + threshold < _bufsize) ? false :
                          (flush(ebuf, size), true);
    }
    BufferedStream& put(std::string_view x) {
        assert(_o + __MAX_SIZE(x) < _bufsize);
        memcpy(_o + obuf(), x.data(), x.size());
        _o += x.size();
        return *this;
    }
    BufferedStream& put(int64_t x) {
        assert(_o + __MAX_SIZE(x) < _bufsize);
        static_assert(sizeof(x) == sizeof(long long), "...");
        _o += snprintf(_o + obuf(), _bufsize - _o, "%ld", (long)x);
        return *this;
    }
    BufferedStream& put(uint64_t x) {
        assert(_o + __MAX_SIZE(x) < _bufsize);
        static_assert(sizeof(x) == sizeof(long long), "...");
        _o += snprintf(_o + obuf(), _bufsize - _o, "%lu", (unsigned long)x);
        return *this;
    }
    BufferedStream& put(char x) {
        assert(_o + __MAX_SIZE(x) < _bufsize);
        obuf()[_o++] = x;
        return *this;
    }
    BufferedStream& put() { return *this; }
    template<typename Ta, typename Tb, typename...Ts>
    BufferedStream& put(const Ta& xa, const Tb& xb, const Ts&...xs) {
        return put(xa), put(xb, xs...);
    }
    // BufferedStream& operator << (std::string_view x) {
    //     auto f = flush_if_low_space(__MAX_SIZE(x), x.data(), x.size());
    //     return f ? *this : put(x);
    // }
    // BufferedStream& operator << (int64_t x) {
    //     flush_if_low_space(__MAX_SIZE(x));
    //     return put(x);
    // }
    template<typename...Ts>
    BufferedStream& write_item(const Ts&...xs) {
        auto size = _sum(__MAX_SIZE(xs)...);
        flush_if_low_space(size);
        return put(xs...);
    }
    BufferedStream& operator << (simple_string x) {
        return write_item(x.mark(), x, CRLF);
    }
    BufferedStream& operator << (error_message x) {
        return write_item(x.mark(), x, CRLF);
    }
    BufferedStream& operator << (integer x) {
        return write_item(x.mark(), x.val, CRLF);
    }
    BufferedStream& operator << (bulk_string x) {
        return x.empty() ? write_item(x.mark(), "-1\r\n") :
            write_item(x.mark(), (int64_t)x.size(), CRLF, x, CRLF);
    }
    BufferedStream& operator << (null) {
        return *this << bulk_string{};
    }
    void write_items() { }
    template<typename T, typename...Ts>
    void write_items(const T& x, const Ts&...xs) {
        *this << x;
        write_items(xs...);
    }
    template<typename...Ts>
    BufferedStream& operator << (const array<Ts...>& x) {
        write_item(x.mark(), (int64_t)sizeof...(Ts), CRLF);
        auto f = [this](const Ts&...xs) { write_items(xs...); };
        tuple_assistance::apply(f, (const std::tuple<Ts...>&)x);
        return *this;
    }

    char get_char() {
        ensure_input_data(__MAX_SIZE('c'));
        return ibuf()[_i++];
    }
    refstring getline() {
        return {this, __getline()};
    }
    refstring getstring(size_t length) {
        return {this, __getstring(length)};
    }
    simple_string get_simple_string() { return getline(); }
    error_message get_error_message() { return getline(); }
    integer       get_integer()       { return getline(); }
    bulk_string   get_bulk_string()   {
        auto length = get_integer().val;
        if (length >= 0)
            return getstring((size_t)length);
        return {};
    }
    void __refill(size_t atleast);  // refill input buffer
    void ensure_input_data(size_t min_available) {
        assert(_j >= _i);
        size_t available = _j - _i;
        if (available < min_available)
            __refill(min_available - available);
    }
    any parse_item();
};

inline void refstring::add_ref() { if (_bs) _bs->_refcnt++; }
inline void refstring::del_ref() { if (_bs) _bs->_refcnt--; }

template<uint32_t BUF_SIZE = 16*1024UL>
class _BufferedStream : public BufferedStream {
    char _buf[BUF_SIZE * 2];
public:
    _BufferedStream(ISocketStream* s) : BufferedStream(s, BUF_SIZE) { }
};

}
}

