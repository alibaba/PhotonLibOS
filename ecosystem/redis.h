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

#pragma GCC diagnostic push
#if __GNUC__ >= 10
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif


class _RedisClient;
class refstring : public std::string_view {
    _RedisClient* _rc = nullptr;
    void add_ref();
    void del_ref();
public:
    using std::string_view::string_view;
    refstring(std::string_view sv) : std::string_view(sv) { }
    refstring(_RedisClient* bs, std::string_view sv) :
            std::string_view(sv), _rc(bs) { add_ref(); }
    refstring(const refstring& rhs) :
            std::string_view(rhs), _rc(rhs._rc) { add_ref(); }
    refstring& operator = (const refstring& rhs) {
        if (this == &rhs) return *this;
        *(std::string_view*)this = rhs;
        del_ref();
        _rc = rhs._rc;
        add_ref();
        return *this;
    }
    void release() {
        del_ref();
        _rc = nullptr;
        (std::string_view&) *this = {};
    }
    ~refstring() { del_ref(); }
};

class simple_string : public refstring {
public:
    simple_string() = default;
    simple_string(refstring rs) : refstring(rs) { }
    // simple_string(std::string_view sv) : refstring(sv) { }
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
    using refstring::refstring;
    bulk_string(refstring rs) : refstring(rs) { }
    bulk_string(std::string_view sv) : refstring(nullptr, sv) { }
    // template<size_t N>
    // bulk_string(const char(&s)[N]) : refstring(nullptr, {s, N-1}) { }
    using refstring::operator=;
    constexpr static char mark() { return '$'; }
};

class integer : public refstring {
public:
    int64_t val;
    integer() = default;
    integer(const integer&) = default;
    integer(int64_t v) : val(v) { };
    integer(const refstring& rs) :
        val(((estring_view&)rs).to_int64()) { }
    using refstring::operator=;
    constexpr static char mark() { return ':'; }
};

class array_header : public integer {
public:
    using integer::integer;
    array_header() = default;
    array_header(const array_header&) = default;
    array_header(integer x) : integer(x) { }
    constexpr static char mark() { return '*'; }
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

    bool is_failed() { return is_type<error_message>(); }

    template<typename T, typename = typename std::enable_if<
                    std::is_base_of<refstring, T>::value>::type>
    T& get() {
        assert(is_type<T>());
        return *(T*)value;
    }

    std::string_view get_error_message() {
        return get<error_message>();
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

    template<typename T>
    any& operator = (const T& x) { return set(x), *this; }
};

using net::ISocketStream;



#define CRLF "\r\n"
#define BSMARK "$"

class _RedisClient {
protected:
    ISocketStream* _s = nullptr;
    bool _s_ownership = false;
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

    struct _strint { int64_t _x; };
    struct _char { char _x; };
    struct _array_header { size_t n; };
    size_t __MAX_SIZE(std::string_view x) { return x.size(); }
    size_t __MAX_SIZE(int64_t x) { return 32; }
    size_t __MAX_SIZE(_strint x) { return 32; }
    size_t __MAX_SIZE(_array_header x) { return 32; }
    size_t __MAX_SIZE(_char x) { return 1; }

    explicit _RedisClient(ISocketStream* s, bool s_ownership, uint32_t bufsize) :
        _s(s), _s_ownership(s_ownership), _bufsize(bufsize) { }
    ~_RedisClient() {
        if (_s_ownership)
            delete _s;
    }

public:
    ssize_t flush(const void* extra_buffer = 0, size_t size = 0);
    bool flush_if_low_space(size_t threshold, const void* ebuf = 0, size_t size = 0) {
        return (_o + threshold < _bufsize) ? false :
                          (flush(ebuf, size), true);
    }
    _RedisClient& put(std::string_view x) {
        assert(_o + __MAX_SIZE(x) < _bufsize);
        memcpy(_o + obuf(), x.data(), x.size());
        _o += x.size();
        return *this;
    }
    template<typename...Ts>
    std::string_view _snprintf(const char* fmt, const Ts&...args) {
        auto buf = obuf() + _o;
        auto size = _bufsize - _o;
        int ret = snprintf(buf, size - _o, fmt, args...);
        assert(0 <= ret && (uint32_t)ret <= size);
        _o += ret;
        return {buf, (size_t)ret};
    }
    _RedisClient& put(_strint x) {
        assert(_o + __MAX_SIZE(x) < _bufsize);
        static_assert(sizeof(x) == sizeof(long long), "...");
        auto s = _snprintf("$00\r\n%ld\r\n", (long)x._x);
        assert(7 < s.size() && s.size() < 99);
        auto n = s.size() - 7;
        (char&)s[1] = '0' + n / 10;
        (char&)s[2] = '0' + n % 10;
        return *this;
    }
    _RedisClient& put(int64_t x) {
        assert(_o + __MAX_SIZE(x) < _bufsize);
        _snprintf("%ld", (long)x);
        return *this;
    }
    _RedisClient& put(_char x) {
        assert(_o + __MAX_SIZE(x) < _bufsize);
        obuf()[_o++] = x._x;
        return *this;
    }
    _RedisClient& put() { return *this; }
    template<typename Ta, typename Tb, typename...Ts>
    _RedisClient& put(const Ta& xa, const Tb& xb, const Ts&...xs) {
        return put(xa), put(xb, xs...);
    }
    _RedisClient& operator << (int64_t x) {
        // flush_if_low_space(__MAX_SIZE(x));
        return put(x);
    }
    _RedisClient& operator << (_char x) {
        // flush_if_low_space(__MAX_SIZE(x));
        return put(x);
    }
    _RedisClient& operator << (const _strint& x) {
        // flush_if_low_space(__MAX_SIZE(x));
        return put(x);
    }
    template<typename...Ts>
    _RedisClient& write_item(const Ts&...xs) {
        auto size = _sum(__MAX_SIZE(xs)...);
        flush_if_low_space(size);
        return put(xs...);
    }
    _RedisClient& operator << (std::string_view x) {
        return x.empty() ? write_item(BSMARK "-1" CRLF) :
            write_item(_char{BSMARK[0]}, (int64_t)x.size(), CRLF, x, CRLF);
    }
    _RedisClient& operator << (_array_header x) {
        return write_item(_char{array<>::mark()}, x.n, CRLF);
    }

    char get_char() {
        if (ensure_input_data(__MAX_SIZE(_char{'c'})) < 0) {
            return '\0';
        }
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
    ssize_t __refill(size_t atleast);  // refill input buffer
    int ensure_input_data(size_t min_available) {
        assert(_j >= _i);
        size_t available = _j - _i;
        if (available < min_available && __refill(min_available - available) < 0) {
            return -1;
        }
        return 0;
    }

    void write_items() { }
    template<typename T, typename...Ts>
    void write_items(const T& x, const Ts&...xs) {
        *this << x;
        write_items(xs...);
    }

    _strint filter(int64_t x) { return {x}; }
    std::string_view filter(std::string_view x) { return x; }

    template<typename...Args>
    void send_cmd_no_flush(std::string_view cmd, const Args&...args) {
        write_items(_array_header{1 + sizeof...(args)}, cmd, filter(args)...);
    }

    any parse_response_item();

    template<typename...Args>
    any execute(bulk_string cmd, const Args&...args) {
        send_cmd_no_flush(cmd, args...);
        if (flush() < 0) {
            return error_message("flush failed");
        }
        return parse_response_item();
    }

#define DEFINE_COMMAND(cmd)                                 \
    any cmd() { return execute(#cmd); }
#define DEFINE_COMMAND1(cmd, arg)                           \
    any cmd(bulk_string arg) {                              \
        return execute(#cmd, arg);                          \
    }
#define DEFINE_COMMAND1s(cmd, arg)                          \
    template<typename...arg##s_type>                        \
    any cmd(bulk_string arg, const arg##s_type&...arg##s) { \
        return execute(#cmd, arg, arg##s...);               \
    }
#define DEFINE_COMMAND1m(cmd, arg, more_args)               \
    template<typename...ARGS>                               \
    any cmd(bulk_string arg, const ARGS&...more_args) {     \
        return execute(#cmd, arg, more_args...);            \
    }
#define DEFINE_COMMAND1sn(cmd, arg1)                        \
    template<typename...arg1##s_type>                       \
    any cmd(bulk_string arg1,const arg1##s_type&...arg1##s){\
        auto n = (1 + sizeof...(arg1##s));            \
        return execute(#cmd, n, arg1, arg1##s...);          \
    }
#define DEFINE_COMMAND2(cmd, arg1, arg2)                    \
    any cmd(bulk_string arg1, bulk_string arg2) {           \
        return execute(#cmd, arg1, arg2);                   \
    }
#define DEFINE_COMMAND2m(cmd, arg1, arg2, more_args)        \
    template<typename...ARGS>                               \
    any cmd(bulk_string arg1, bulk_string arg2,             \
                             const ARGS&...more_args) {     \
        return execute(#cmd, arg1, arg2, more_args...);     \
    }
#define DEFINE_COMMAND2s(cmd, arg1, arg2)                   \
    template<typename...arg2##s_type>                       \
    any cmd(bulk_string arg1, bulk_string arg2,             \
                    const arg2##s_type&...arg2##s) {        \
        return execute(#cmd, arg1, arg2, arg2##s...);       \
    }
#define DEFINE_COMMAND2sn(cmd, arg1, arg2)                  \
    template<typename...arg2##s_type>                       \
    any cmd(bulk_string arg1, bulk_string arg2,             \
                    const arg2##s_type&...arg2##s) {        \
        auto n = (1 + sizeof...(arg2##s));            \
        return execute(#cmd, arg1, n, arg2, arg2##s...);    \
    }
#define DEFINE_COMMAND2fs(cmd, arg1)                        \
    template<typename...Fields>                             \
    any cmd(bulk_string arg1, bulk_string field1,           \
                    Fields...fields) {                      \
        auto n = (1 + sizeof...(fields));             \
        return execute(#cmd, arg1, "FIELDS", n,             \
                               field1, fields...);          \
    }
#define DEFINE_COMMAND3(cmd, arg1, arg2, arg3)              \
    any cmd(bulk_string arg1, bulk_string arg2,             \
                              bulk_string arg3) {           \
        return execute(#cmd, arg1, arg2, arg3);             \
    }
#define DEFINE_COMMAND3s(cmd, arg1, arg2, arg3)             \
    template<typename...arg3##s_type>                       \
    any cmd(bulk_string arg1, bulk_string arg2, bulk_string \
              arg3, const arg3##s_type&...arg3##s) {        \
        return execute(#cmd, arg1, arg2, arg3, arg3##s...); \
    }
#define DEFINE_COMMAND4(cmd, arg1, arg2, arg3, arg4)        \
    any cmd(bulk_string arg1, bulk_string arg2,             \
            bulk_string arg3, bulk_string arg4) {           \
        return execute(#cmd, arg1, arg2, arg3, arg4);       \
    }
#define DEFINE_COMMAND5(cmd, arg1, arg2, arg3, arg4, arg5)  \
    any cmd(bulk_string arg1, bulk_string arg2, bulk_string \
            arg3, bulk_string arg4, bulk_string arg5) {     \
        return execute(#cmd, arg1, arg2, arg3, arg4, arg5); \
    }
    // Generic
    DEFINE_COMMAND2 (COPY, source, destination);
    DEFINE_COMMAND1s(DEL, key);
    DEFINE_COMMAND1 (DUMP, key);
    DEFINE_COMMAND1s(EXISTS, key);
    DEFINE_COMMAND2 (EXPIRE, key, seconds);
    DEFINE_COMMAND2 (EXPIREAT, key, unix_time_seconds);
    DEFINE_COMMAND1 (EXPIRETIME, key);
    DEFINE_COMMAND1 (KEYS, pattern);
    DEFINE_COMMAND2 (MOVE, key, db);
    DEFINE_COMMAND1 (PERSIST, key);
    DEFINE_COMMAND2 (PEXPIRE, key, milliseconds);
    DEFINE_COMMAND2 (PEXPIREAT, key, unix_time_milliseconds);
    DEFINE_COMMAND1 (PEXPIRETIME, key);

    // Set
    DEFINE_COMMAND2s(SADD, key, member);
    DEFINE_COMMAND1 (SCARD, key);
    DEFINE_COMMAND1s(SDIFF, key);
    DEFINE_COMMAND2s(SDIFFSTORE, destination, key);
    DEFINE_COMMAND1s(SINTER, key);
    DEFINE_COMMAND2s(SINTERCARD, numkeys, key);
    DEFINE_COMMAND2s(SINTERSTORE, destination, key);
    DEFINE_COMMAND2 (SISMEMBER, key, member);
    DEFINE_COMMAND2s(SMISMEMBER, key, member);
    DEFINE_COMMAND1 (SMEMBERS, key);
    DEFINE_COMMAND3 (SMOVE, source, destination, member);
    DEFINE_COMMAND2 (SPOP, key, count);
    DEFINE_COMMAND1 (SPOP, key);
    DEFINE_COMMAND2 (SRANDMEMBER, key, count);
    DEFINE_COMMAND1 (SRANDMEMBER, key);
    DEFINE_COMMAND2s(SREM, key, member);
    DEFINE_COMMAND1s(SUNION, key);
    DEFINE_COMMAND2s(SUNIONSTORE, destination, key);

    // Hash
    DEFINE_COMMAND2s(HDEL, key, field);
    DEFINE_COMMAND2 (HEXISTS, key, field);
    DEFINE_COMMAND1 (HGETALL, key);
    DEFINE_COMMAND3 (HINCRBY, key, field, increment);
    DEFINE_COMMAND3 (HINCRBYFLOAT, key, field, increment);
    DEFINE_COMMAND1 (HKEYS, key);
    DEFINE_COMMAND1 (HLEN, key);
    DEFINE_COMMAND2 (HGET, key, field);
    DEFINE_COMMAND2s(HMGET, key, field);
    DEFINE_COMMAND3s(HMSET, key, field, value);
    DEFINE_COMMAND3s(HSET, key, field, value);
    DEFINE_COMMAND2fs(HPERSIST, key);
    DEFINE_COMMAND2fs(HEXPIRETIME, key);
    DEFINE_COMMAND2fs(HPEXPIRETIME, key);
    DEFINE_COMMAND2fs(HPTTL, key);
    DEFINE_COMMAND1 (HRANDFIELD, key);
    DEFINE_COMMAND2 (HRANDFIELD, key, count);
    DEFINE_COMMAND3 (HRANDFIELD, key, count, WITHVALUES);
    DEFINE_COMMAND3 (HSETNX, key, field, value);
    DEFINE_COMMAND2 (HSTRLEN, key, field);
    DEFINE_COMMAND2fs(HTTL, key);
    DEFINE_COMMAND1 (HVALS, key);

    // List
    DEFINE_COMMAND5 (BLMOVE, source, destination, LEFT_or_RIGHT1, LEFT_or_RIGHT2, timeout);
    DEFINE_COMMAND3 (BRPOPLPUSH, source, destination, timeout);
    DEFINE_COMMAND2 (LINDEX, key, index);
    DEFINE_COMMAND4 (LINSERT, key, BEFORE_or_AFTER, pivot, element);
    DEFINE_COMMAND1 (LLEN, key);
    DEFINE_COMMAND4 (LMOVE, source, destination, LEFT_or_RIGHT1, LEFT_or_RIGHT2);
    DEFINE_COMMAND1 (LPOP, key);
    DEFINE_COMMAND2 (LPOP, key, count);
    DEFINE_COMMAND2s(LPUSH, key, element);
    DEFINE_COMMAND2s(LPUSHX, key, element);
    DEFINE_COMMAND3 (LRANGE, key, start, stop);
    DEFINE_COMMAND3 (LREM, key, count, element);
    DEFINE_COMMAND3 (LSET, key, index, element);
    DEFINE_COMMAND3 (LTRIM, key, start, stop);
    DEFINE_COMMAND1 (RPOP, key);
    DEFINE_COMMAND2 (RPOP, key, count);
    DEFINE_COMMAND2 (RPOPLPUSH, source, destination);
    DEFINE_COMMAND2s(RPUSH, key, element);
    DEFINE_COMMAND2s(RPUSHX, key, element);

    //Sorted Set
    DEFINE_COMMAND1 (ZCARD, key);
    DEFINE_COMMAND3 (ZCOUNT, key, min, max);
    DEFINE_COMMAND2sn(ZDIFFSTORE, destination, key);
    DEFINE_COMMAND3 (ZINCRBY, key, increment, member);
    DEFINE_COMMAND3 (ZLEXCOUNT, key, min, max);
    DEFINE_COMMAND2s(ZMSCORE, key, member);
    DEFINE_COMMAND1 (ZPOPMAX, key);
    DEFINE_COMMAND2 (ZPOPMAX, key, count);
    DEFINE_COMMAND1 (ZPOPMIN, key);
    DEFINE_COMMAND2 (ZPOPMIN, key, count);
    DEFINE_COMMAND1 (ZRANDMEMBER, key);
    DEFINE_COMMAND2 (ZRANDMEMBER, key, count);
    DEFINE_COMMAND3 (ZRANDMEMBER, key, count, WITHSCORES);
    DEFINE_COMMAND3 (ZRANGEBYLEX, key, min, max);
    DEFINE_COMMAND3 (ZRANGEBYSCORE, key, min, max);
    DEFINE_COMMAND4 (ZRANGESTORE, dst, src, min, max);
    DEFINE_COMMAND2 (ZRANK, key, member);
    DEFINE_COMMAND2s(ZREM, key, member);
    DEFINE_COMMAND3 (ZREMRANGEBYLEX, key, min, max);
    DEFINE_COMMAND3 (ZREMRANGEBYRANK, key, start, stop);
    DEFINE_COMMAND3 (ZREMRANGEBYSCORE, key, min, max);
    DEFINE_COMMAND3 (ZREVRANGE, key, start, stop);
    DEFINE_COMMAND3 (ZREVRANGEBYLEX, key, max, min);
    DEFINE_COMMAND3 (ZREVRANGEBYSCORE, key, max, min);
    DEFINE_COMMAND2 (ZREVRANK, key, member);
    DEFINE_COMMAND2 (ZSCAN, key, cursor);
    DEFINE_COMMAND2 (ZSCORE, key, member);
    DEFINE_COMMAND1sn(ZUNION, key);
    DEFINE_COMMAND2sn(ZUNIONSTORE, destination, key);

    // HyperLogLog
    DEFINE_COMMAND1m(PFADD, key, elements);
    DEFINE_COMMAND1s(PFCOUNT, key);
    DEFINE_COMMAND2 (PFDEBUG, subcommand, key);
    DEFINE_COMMAND1m(PFMERGE, destkey, sourcekeys);
    DEFINE_COMMAND  (PFSELFTEST);

    //Bitmap
    DEFINE_COMMAND1 (BITCOUNT, key);
    DEFINE_COMMAND1m(BITFIELD, key, args);
    DEFINE_COMMAND1m(BITFIELD_RO, key, args);
    DEFINE_COMMAND3s(BITOP, AND_OR_XOR_NOT, destkey, key);
    DEFINE_COMMAND2m(BITPOS, key, bit, opt_start_end_BYTE_BIT);
    DEFINE_COMMAND2 (GETBIT, key, offset);
    DEFINE_COMMAND3 (SETBIT, key, offset, value);

    //Strings
    DEFINE_COMMAND2 (APPEND, key, value);
    DEFINE_COMMAND1 (DECR, key);
    DEFINE_COMMAND2 (DECRBY, key, decrement);
    DEFINE_COMMAND1 (GET, key);
    DEFINE_COMMAND1 (GETDEL, key);
    DEFINE_COMMAND1m(GETEX, key, args);
    DEFINE_COMMAND3 (GETRANGE, key, start, end);
    DEFINE_COMMAND2 (GETSET, key, value);
    DEFINE_COMMAND1 (INCR, key);
    DEFINE_COMMAND2 (INCRBY, key, increment);
    DEFINE_COMMAND2 (INCRBYFLOAT, key, increment);
    DEFINE_COMMAND2m(LCS, key1, key2, args);
    DEFINE_COMMAND1s(MGET, key);
    DEFINE_COMMAND2m(MSET, key, value, more_key_values);
    DEFINE_COMMAND2m(MSETNX, key, value, more_key_values);
    DEFINE_COMMAND3 (PSETEX, key, milliseconds, value);
    DEFINE_COMMAND2m(SET, key, value, more_key_values);
    DEFINE_COMMAND3 (SETEX, key, seconds, value);


#undef DEFINE_COMMAND1
#undef DEFINE_COMMAND1s
#undef DEFINE_COMMAND2s
    template<typename...Fields>
    any __hexp(bulk_string op, bulk_string key, bulk_string time,
                bulk_string condition, const Fields&...fields) {
        bool ok = (condition == "NX" || condition == "XX" ||
                   condition == "GT" || condition == "LT" );
        assert(condition.empty() || ok);
        auto n = (sizeof...(fields));
        if (condition.empty() || !ok) {
            return execute(op, key, time, "FIELDS",n , fields...);
        } else {
            return execute(op, key, time, condition, "FIELDS", n, fields...);
        }
    }
    template<typename...Fields>
    any HEXPIRE(bulk_string key, bulk_string seconds, bulk_string condition,
                bulk_string field, const Fields&...fields) {
        return __hexp("HEXPIRE", key, seconds, condition, field, fields...);
    }
    template<typename...Fields>
    any HEXPIREAT(bulk_string key, bulk_string seconds, bulk_string condition,
                  bulk_string field, const Fields&...fields) {
        return __hexp("HEXPIREAT", key, seconds, condition, field, fields...);
    }
    template<typename...Fields>
    any HPEXPIRE(bulk_string key, bulk_string milliseconds, bulk_string condition,
                 bulk_string field, const Fields&...fields) {
        return __hexp("HPEXPIRE", key, milliseconds, condition, field, fields...);
    }
    template<typename...Fields>
    any HPEXPIREAT(bulk_string key, bulk_string unix_time_milliseconds, bulk_string condition,
                 bulk_string field, const Fields&...fields) {
        return __hexp("HPEXPIREAT", key, unix_time_milliseconds, condition, field, fields...);
    }

    any SSCAN(bulk_string key, bulk_string cursor, bulk_string pattern, uint64_t count) {
        return execute("SSCAN", key, cursor, "PATTERN", pattern, "COUNT", count);
    }
    any SSCAN(bulk_string key, bulk_string cursor, bulk_string pattern) {
        return execute("SSCAN", key, cursor, "PATTERN", pattern);
    }
    any SSCAN(bulk_string key, bulk_string cursor, uint64_t count) {
        return execute("SSCAN", key, cursor, "COUNT", count);
    }
    any SSCAN(bulk_string key, bulk_string cursor) {
        return execute("SSCAN", key, cursor);
    }
    any ZRANGEBYLEX(bulk_string key, bulk_string min, bulk_string max,
                    bulk_string limit_offset, bulk_string limit_count) {
        return execute("ZRANGEBYLEX", key, min, max, "LIMIT", limit_offset, limit_count);
    }
    any ZRANGEBYSCORE(bulk_string key, bulk_string min,
                      bulk_string max, bulk_string WITHSCORES) {
        return execute("ZRANGEBYSCORE", key, min, max, "WITHSCORES");
    }
    any ZRANGEBYSCORE(bulk_string key, bulk_string min, bulk_string max, bulk_string
          WITHSCORES, bulk_string limit_offset, bulk_string limit_count) {
        return execute("ZRANGEBYSCORE", key, min, max, "WITHSCORES",
                       "LIMIT", limit_offset, limit_count);
    }
    any ZRANK(bulk_string key, bulk_string member, bulk_string WITHSCORE) {
        return execute("ZRANK", key, member, "WITHSCORE");
    }
    any ZREVRANGE(bulk_string key, bulk_string start, bulk_string stop, bulk_string WITHSCORE) {
        return execute("ZREVRANGE", key, start, stop, "WITHSCORE");
    }
    any ZREVRANGEBYLEX(bulk_string key, bulk_string max, bulk_string min,
                       bulk_string limit_offset, bulk_string limit_count) {
        return execute("ZREVRANGEBYLEX", key, max, min,
                       "LIMIT", limit_offset, limit_count);
    }
    any ZREVRANGEBYSCORE(bulk_string key, bulk_string max, bulk_string min, bulk_string
          WITHSCORES, bulk_string limit_offset, bulk_string limit_count) {
        return execute("ZREVRANGEBYSCORE", key, max, min, "WITHSCORES",
                       "LIMIT", limit_offset, limit_count);
    }
    any ZREVRANK(bulk_string key, bulk_string member, bulk_string WITHSCORE) {
        return execute("ZREVRANK", key, member, "WITHSCORE");
    }
    any ZSCAN_PATTERN(bulk_string key, bulk_string cursor, bulk_string pattern) {
        return execute("ZSCAN", key, cursor, "PATTERN", pattern);
    }
    any ZSCAN_COUNT(bulk_string key, bulk_string cursor, bulk_string count) {
        return execute("ZSCAN", key, cursor, "COUNT", count);
    }
    any ZSCAN(bulk_string key, bulk_string cursor, bulk_string pattern, bulk_string count) {
        return execute("ZSCAN", key, cursor, "PATTERN", pattern, "COUNT", count);
    }
    any EXPIRE(bulk_string key, bulk_string seconds, bulk_string NX_or_XX_or_GT_or_LT) {
        assert(NX_or_XX_or_GT_or_LT == "NX" || NX_or_XX_or_GT_or_LT == "XX" ||
               NX_or_XX_or_GT_or_LT == "GT" || NX_or_XX_or_GT_or_LT == "LT");
        return execute("EXPIRE", key, seconds, NX_or_XX_or_GT_or_LT);
    }
    any EXPIREAT(bulk_string key, bulk_string unix_time_seconds, bulk_string NX_or_XX_or_GT_or_LT) {
        assert(NX_or_XX_or_GT_or_LT == "NX" || NX_or_XX_or_GT_or_LT == "XX" ||
               NX_or_XX_or_GT_or_LT == "GT" || NX_or_XX_or_GT_or_LT == "LT");
        return execute("EXPIREAT", key, unix_time_seconds, NX_or_XX_or_GT_or_LT);
    }
    any PEXPIRE(bulk_string key, bulk_string milliseconds, bulk_string NX_or_XX_or_GT_or_LT) {
        assert(NX_or_XX_or_GT_or_LT == "NX" || NX_or_XX_or_GT_or_LT == "XX" ||
               NX_or_XX_or_GT_or_LT == "GT" || NX_or_XX_or_GT_or_LT == "LT");
        return execute("PEXPIRE", key, milliseconds, NX_or_XX_or_GT_or_LT);
    }
    any PEXPIREAT(bulk_string key, bulk_string unix_time_milliseconds, bulk_string NX_or_XX_or_GT_or_LT) {
        assert(NX_or_XX_or_GT_or_LT == "NX" || NX_or_XX_or_GT_or_LT == "XX" ||
               NX_or_XX_or_GT_or_LT == "GT" || NX_or_XX_or_GT_or_LT == "LT");
        return execute("PEXPIREAT", key, unix_time_milliseconds, NX_or_XX_or_GT_or_LT);
    }
    any OBJECT_ENCODING(bulk_string key) {
        return execute("OBJECT", "ENCODING", key);
    }
    any OBJECT_FREQ(bulk_string key) {
        return execute("OBJECT", "FREQ", key);
    }
    any OBJECT_IDLETIME(bulk_string key) {
        return execute("OBJECT", "IDLETIME", key);
    }
    any OBJECT_IREFCOUNT(bulk_string key) {
        return execute("OBJECT", "REFCOUNT", key);
    }
    any BITCOUNT(bulk_string key, bulk_string start, bulk_string end) {
        return execute("BITCOUNT", key, start, end);
    }
    any BITCOUNT(bulk_string key, bulk_string start, bulk_string end, bulk_string BYTE_or_BIT) {
        assert(BYTE_or_BIT == "BYTE" || BYTE_or_BIT == "BIT");
        return execute("BITCOUNT", key, start, end, BYTE_or_BIT);
    }

    // ZMPOP numkeys key [key ...] <MIN | MAX> [COUNT count]
    // ZRANGE key start stop [BYSCORE | BYLEX] [REV] [LIMIT offset count] [WITHSCORES]
    // HSCAN key cursor [MATCH pattern] [COUNT count] [NOVALUES]
    // BLMPOP timeout numkeys key [key ...] <LEFT | RIGHT> [COUNT count]
    // BLPOP key [key ...] timeout
    // BRPOP key [key ...] timeout
    // LPOS key element [RANK rank] [COUNT num-matches] [MAXLEN len]
    // LMPOP numkeys key [key ...] <LEFT | RIGHT> [COUNT count]
    // BZMPOP timeout numkeys key [key ...] <MIN | MAX> [COUNT count]
    // BZPOPMAX key [key ...] timeout
    // BZPOPMIN key [key ...] timeout
    // ZADD key [NX | XX] [GT | LT] [CH] [INCR] score member [score member ...]
    // ZDIFF numkeys key [key ...] [WITHSCORES]
    // ZINTER numkeys key [key ...] [WEIGHTS weight [weight ...]] [AGGREGATE <SUM | MIN | MAX>] [WITHSCORES]
    // ZINTERCARD numkeys key [key ...] [LIMIT limit]
    // ZINTERSTORE destination numkeys key [key ...] [WEIGHTS weight [weight ...]] [AGGREGATE <SUM | MIN | MAX>]
    // ZRANGESTORE dst src min max [BYSCORE | BYLEX] [REV] [LIMIT offset count]
    // ZUNION numkeys key [key ...] [WEIGHTS weight [weight ...]] [AGGREGATE <SUM | MIN | MAX>] [WITHSCORES]
    // ZUNIONSTORE destination numkeys key [key ...] [WEIGHTS weight [weight ...]] [AGGREGATE <SUM | MIN | MAX>]

};

inline void refstring::add_ref() { if (_rc) _rc->_refcnt++; }
inline void refstring::del_ref() { if (_rc) _rc->_refcnt--; }

template<uint32_t BUF_SIZE = 16*1024UL>
class __RedisClient : public _RedisClient {
    char _buf[BUF_SIZE * 2];
public:
    __RedisClient(ISocketStream* s, bool s_ownership) : _RedisClient(s, s_ownership, BUF_SIZE) { }
};

using RedisClient = __RedisClient<16*1024UL>;

#pragma GCC diagnostic pop


}
}
