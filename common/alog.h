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
#include <cstdio>
#include <cinttypes>
#include <cstring>
#include <cerrno>
#include <cassert>
#include <ctime>
#include <utility>
#include <type_traits>

#include <photon/common/utility.h>
#include <photon/common/conststr.h>
#include <photon/common/retval.h>

#define ALOG_COLOR_BLACK "\033[30m"
#define ALOG_COLOR_RED "\033[31m"
#define ALOG_COLOR_GREEN "\033[32m"
#define ALOG_COLOR_YELLOW "\033[33m"
#define ALOG_COLOR_BLUE "\033[34m"
#define ALOG_COLOR_MAGENTA "\033[35m"
#define ALOG_COLOR_CYAN "\033[36m"
#define ALOG_COLOR_LIGHTGRAY "\033[37m"
#define ALOG_COLOR_DARKGRAY "\033[90m"
#define ALOG_COLOR_LIGHTRED "\033[91m"
#define ALOG_COLOR_LIGHTGREEN "\033[92m"
#define ALOG_COLOR_LIGHTYELLOW "\033[93m"
#define ALOG_COLOR_LIGHTBLUE "\033[94m"
#define ALOG_COLOR_LIGHTMAGENTA "\033[95m"
#define ALOG_COLOR_LIGHTCYAN "\033[96m"
#define ALOG_COLOR_LIGHTWHITE "\033[97m"
#define ALOG_COLOR_NOTHING ""

class ILogOutput {
protected:
    // output object should be destructed via `destruct()`
    // make alog to used in the construct/destruct of global variables
    ~ILogOutput() = default;

public:
    virtual void write(int level, const char* begin, const char* end) = 0;
    virtual int get_log_file_fd() = 0;
    virtual uint64_t set_throttle(uint64_t t = -1UL) = 0;
    virtual uint64_t get_throttle() = 0;
    virtual void destruct() = 0;
    template <size_t N>
    void set_level_color(int level, const char (&color)[N]) {
        set_level_color(level, color, N - 1);
    }
    virtual void set_level_color(int level, const char* color,
                                 size_t length) { /* ignored by default */ }
    void preset_color();
    void clear_color();
};

extern ILogOutput * const log_output_null;
extern ILogOutput * const log_output_stderr;
extern ILogOutput * const log_output_stdout;

ILogOutput* new_log_output_file(const char* fn, uint64_t rotate_limit = UINT64_MAX, int max_log_files = 10, uint64_t throttle = -1UL);
ILogOutput* new_log_output_file(int fd, uint64_t throttle = -1UL);
ILogOutput* new_async_log_output(ILogOutput* output);

// old-style log_output_file & log_output_file_close
// return 0 when successed, -1 for failed
int log_output_file(int fd, uint64_t rotate_limit = UINT64_MAX, uint64_t throttle = -1UL);
int log_output_file(const char* fn, uint64_t rotate_limit = UINT64_MAX, int max_log_files = 10, uint64_t throttle = -1UL);
int log_output_file_close();

#ifndef LOG_BUFFER_SIZE
// size of a temp buffer on stack to format a log, deallocated after output
#define LOG_BUFFER_SIZE 4096
#endif

// wrapper for an integer, together with format information
struct ALogInteger
{
public:
    template<typename T, ENABLE_IF(std::is_integral<T>::value)>
    ALogInteger(T x, char shift)
    {
        _signed = std::is_signed<T>::value;
        if (_signed) _svalue = x;
        else _uvalue = x;
        _shift = shift;
    }
    uint64_t uvalue() const  { return _uvalue; }
    int64_t  svalue() const  { return _svalue; }
    bool is_signed()  const  { return _signed; }
    char shift()      const  { return _shift; }
    char width()      const  { return _width; }
    char padding()    const  { return _padding; }
    bool comma()      const  { return _comma; }
    bool lower()      const  { return _lower; }
    ALogInteger& width(char x)       { this->_width = x;      return *this; }
    ALogInteger& padding(char x)     { this->_padding = x;    return *this; }
    ALogInteger& comma(bool x)       { this->_comma = x;      return *this; }
    ALogInteger& lower(bool x)       { this->_lower = x;      return *this; }

protected:
    union
    {
        uint64_t _uvalue;
        int64_t _svalue;
    };
    char _shift;
    char _width = 0;
    char _padding = ' ';
    bool _comma = false;
    bool _lower = false;
    bool _signed;
};

template<typename T>
ALogInteger HEX(T x) { return ALogInteger(x, 4).padding('0'); }

template<typename T>
ALogInteger DEC(T x) { return ALogInteger(x, 10); }

template<typename T>
ALogInteger OCT(T x) { return ALogInteger(x, 3); }

template<typename T>
ALogInteger BIN(T x) { return ALogInteger(x, 1); }


// wrapper for an floating point value, together with format information
struct FP
{
public:
    explicit FP(double x) : _value(x) { }
    double value()    const  { return _value; }
    char width()      const  { return _width; }
    char padding()    const  { return _padding; }
    char precision()  const  { return _precision; }
    bool comma()      const  { return _comma; }
    bool lower()      const  { return _lower; }
    bool scientific() const  { return _scientific; }
    FP& width(char x)        { _width = x;      return *this; }
    FP& width(char x, char y){ _width = x; _precision = y; return *this; }
    FP& precision(char x, char y) { return width(x,y); }
    FP& precision(char x)    { _precision = x;  return *this; }
    FP& padding(char x)      { _padding = x;    return *this; }
    FP& comma(bool x)        { _comma = x;      return *this; }
    FP& lower(bool x)        { _lower = x;      return *this; }
    FP& scientific(bool x)   { _scientific = x; return *this; }

protected:
    double _value;
    char _width = -1;
    char _precision = -1;
    char _padding = '0';
    bool _comma = false;
    bool _lower = false;
    bool _scientific = false;
};

struct ALogString
{
    const char* const s;
    size_t size;

    constexpr ALogString(const char* s_, size_t n) : s(s_), size(n) { }

    constexpr char operator[](size_t i) const { return s[i]; }
};

struct ALogStringL : public ALogString
{
    using ALogString::ALogString;

    template<size_t N> constexpr
    ALogStringL(const char (&s_)[N]) : ALogString(s_, N-1) { }
};

struct ALogStringPChar : public ALogString
{
    using ALogString::ALogString;
};


// use ENABLE_IF_NOT_SAME, so that string literals (char[]) are not matched as 'const char*&' (T == char)
template<typename T, ENABLE_IF_NOT_SAME(T, char*)> inline
const T& alog_forwarding(const T& x) { return x; }

// matches string literals and const char[], but not char[]
template<size_t N> inline
ALogStringL alog_forwarding(const char (&s)[N]) { return ALogStringL(s); }

// matches named char[] (not const!)
template<size_t N> inline
ALogStringPChar alog_forwarding(char (&s)[N]) { return ALogStringPChar(s, strlen(s)); }

// matches (const) char*
// we have to use the template form, otherwise it will override the templates of char[N]
template<typename T, ENABLE_IF_SAME(T, char)> inline
ALogStringPChar alog_forwarding(const T * const & s) { return ALogStringPChar(s, s ? strlen(s) : 0); }

struct ALogBuffer
{
    char* ptr;
    uint32_t size;
    uint32_t level;
    void consume(size_t n) { ptr += n; size -= n; }
};

#define ALOG_DEBUG 0
#define ALOG_INFO  1
#define ALOG_WARN  2
#define ALOG_ERROR 3
#define ALOG_FATAL 4
#define ALOG_TEMP  5
#define ALOG_AUDIT 6

inline void ILogOutput::preset_color() {
    set_level_color(ALOG_DEBUG, ALOG_COLOR_DARKGRAY);
    set_level_color(ALOG_INFO, ALOG_COLOR_LIGHTGRAY);
    set_level_color(ALOG_WARN, ALOG_COLOR_YELLOW);
    set_level_color(ALOG_ERROR, ALOG_COLOR_RED);
    set_level_color(ALOG_FATAL, ALOG_COLOR_MAGENTA);
    set_level_color(ALOG_TEMP, ALOG_COLOR_CYAN);
    set_level_color(ALOG_AUDIT, ALOG_COLOR_GREEN);
}
inline void ILogOutput::clear_color() {
    set_level_color(ALOG_DEBUG, ALOG_COLOR_NOTHING);
    set_level_color(ALOG_INFO, ALOG_COLOR_NOTHING);
    set_level_color(ALOG_WARN, ALOG_COLOR_NOTHING);
    set_level_color(ALOG_ERROR, ALOG_COLOR_NOTHING);
    set_level_color(ALOG_FATAL, ALOG_COLOR_NOTHING);
    set_level_color(ALOG_TEMP, ALOG_COLOR_NOTHING);
    set_level_color(ALOG_AUDIT, ALOG_COLOR_NOTHING);
}

class LogFormatter
{
public:
    __attribute__((always_inline))
    void put(ALogBuffer& buf, char c)
    {
        if (buf.size == 0) return;
        *buf.ptr = c;
        buf.consume(1);
    }

    __attribute__((always_inline))
    void put(ALogBuffer& buf, const ALogString& s)
    {
        if (buf.size < s.size) return;
        memcpy(buf.ptr, s.s, s.size);
        buf.consume(s.size);
    }

    void put(ALogBuffer& buf, void* p)
    {
        put(buf, HEX((uint64_t)p).width(16));
    }

    template<typename T>
    using extend_to_64 = typename std::conditional<std::is_signed<T>::value, int64_t, uint64_t>::type;

    template<typename T, ENABLE_IF(std::is_integral<T>::value)>
    void put(ALogBuffer& buf, T x, int = 0)
    {
        const uint32_t upper_bound = sizeof(x) * 3;
        if (buf.size < upper_bound) return;
        put_integer(buf, extend_to_64<T>(x));
    }

    template<typename T, ENABLE_IF(std::is_enum<T>::value)>
    void put(ALogBuffer& buf, T x, void* = 0)
    {
        put_integer(buf, (int64_t)x);
    }

    void put(ALogBuffer& buf, ALogInteger x)
    {
        auto s = x.shift();
        if (s == 1 || s == 3 || s == 4) put_integer_hbo(buf, x);
        else /* if (s == 10) */         put_integer_dec(buf, x);
    }

    void put(ALogBuffer& buf, double x)
    {
        snprintf(buf, "%0.4f", x);
    }

    void put(ALogBuffer& buf, FP x);


protected:
    void snprintf(ALogBuffer& buf, const char* fmt, double x)
    {
        int ret = ::snprintf(buf.ptr, buf.size, fmt, x);
        if (ret < 0) return;
        buf.consume(ret);
    }

    void put_integer_hbo(ALogBuffer& buf, ALogInteger x);
    void put_integer_dec(ALogBuffer& buf, ALogInteger x);
    void put_integer(ALogBuffer& buf, uint64_t x);
    void put_integer(ALogBuffer& buf, int64_t x)
    {
        uint64_t xx;
        if (x < 0)
        {
            put(buf, '-');
            xx = (uint64_t)-x;
        } else xx = (uint64_t)x;
        put_integer(buf, xx);
    }
};

struct ALogLogger {
    ILogOutput* log_output;
    uint32_t log_level;

    template <typename T>
    ALogLogger& operator<<(T&& rhs) {
        rhs.logger = this;
        return *this;
    }
};

extern ALogLogger default_logger;
extern ALogLogger default_audit_logger;

inline int get_log_file_fd() { return default_logger.log_output->get_log_file_fd(); }

extern uint32_t& log_output_level;
extern ILogOutput*& log_output;

static LogFormatter log_formatter;

struct LogBuffer : public ALogBuffer
{
public:
    LogBuffer(ILogOutput *output) : log_output(output)
    {
        ptr = buf;
        size = sizeof(buf) - 2;
    }
    ~LogBuffer() __INLINE__
    {
        log_output->write(level, buf, ptr);
    }
    template<typename T>
    __attribute__((always_inline))
    LogBuffer& operator << (const T& x)
    {
        log_formatter.put(*this, alog_forwarding(x));
        return *this;
    }
    LogBuffer& printf()
    {
        return *this;
    }
    template<typename T, typename...Ts>
    LogBuffer& printf(T&& x, Ts&&...xs)
    {
        (*this) << std::forward<T>(x);
        return printf(std::forward<Ts>(xs)...);
    }

protected:
    ILogOutput *log_output;
    char buf[LOG_BUFFER_SIZE];
    void operator = (const LogBuffer& rhs);
    LogBuffer(const LogBuffer& rhs) = default;
};

struct Prologue
{
    const char *addr_func, *addr_file;
    int len_func, len_file;
    int line, level;

    template <size_t N, typename FILEN>
    constexpr Prologue(const char (&addr_func_)[N], FILEN addr_file_, int line_,
                       int level_)
        : addr_func(addr_func_),
          addr_file(addr_file_.chars),
          len_func(N - 1),
          len_file(addr_file_.len),
          line(line_),
          level(level_) {}
};

LogBuffer& operator << (LogBuffer& log, const Prologue& pro);

template <typename T>
using CopyOrRef = std::conditional<
    std::is_trivial<typename std::remove_reference<T>::type>::value &&
        (sizeof(typename std::remove_reference<T>::type) <= 16),
    const T, const T&>;

struct STFMTLogBuffer : public LogBuffer
{
    using LogBuffer::LogBuffer;

    __INLINE__ ~STFMTLogBuffer() = default;

    template <typename ST, typename T, typename... Ts>
    auto print_fmt(ST st, T&& t, Ts&&... ts) ->
        typename std::enable_if<ST::template cut<'`'>().tail.chars[0] !=
                                '`'>::type
    {
        printf<const ALogString&,
               typename CopyOrRef<decltype(alog_forwarding(t))>::type>(
            ALogString(st.template cut<'`'>().head.chars,
                       st.template cut<'`'>().head.len),
            alog_forwarding(std::forward<T>(t)));
        print_fmt(st.template cut<'`'>().tail, std::forward<Ts>(ts)...);
    }

    template <typename ST, typename T, typename... Ts>
    auto print_fmt(ST st, T&& t, Ts&&... ts) ->
        typename std::enable_if<ST::template cut<'`'>().tail.chars[0] ==
                                '`'>::type
    {
        printf<const ALogString&, const char>(
            ALogString(st.template cut<'`'>().head.chars,
                       st.template cut<'`'>().head.len),
            '`');
        print_fmt(st.template cut<'`'>().tail.template cut<'`'>().tail,
                  std::forward<T>(t), std::forward<Ts>(ts)...);
    }

    template <typename ST>
    void print_fmt(ST)
    {
        printf<const ALogString&>(ALogString(ST::chars, ST::len));
    }

    template <typename T, typename... Ts>
    void print_fmt(ConstString::TString<> st, T&& t, Ts&&... ts)
    {
        printf<typename CopyOrRef<decltype(alog_forwarding(t))>::type,
               typename CopyOrRef<decltype(alog_forwarding(ts))>::type...>(
            alog_forwarding(std::forward<T>(t)),
            alog_forwarding(std::forward<Ts>(ts))...);
    }
};

template<typename FMT, typename...Ts> inline
STFMTLogBuffer __log__(int level, ILogOutput* output, const Prologue& prolog, FMT fmt, Ts&&...xs)
{
    STFMTLogBuffer log(output);
    log << prolog;
    log.print_fmt(fmt, std::forward<Ts>(xs)..., '\n');
    log.level = level;
    return log;
}

template<typename BuilderType>
struct LogBuilder {
    uint32_t level;
    BuilderType builder;
    ALogLogger* logger;
    bool done;

    LogBuilder(int level_, BuilderType&& builder_, ALogLogger* logger_)
        : level(level_),
          builder(std::forward<BuilderType>(builder_)),
          logger(logger_),
          done(false) {}
    LogBuilder(LogBuilder&& rhs)
        : level(rhs.level),
          builder(std::move(rhs.builder)),
          logger(rhs.logger),
          done(rhs.done) {
        // it might copy
        // so just make sure the other one will not output
        rhs.done = true;
    }
    ~LogBuilder() __INLINE__ {
        if (!done && level >= logger->log_level) {
            builder(logger->log_output);
            done = true;
        }
    }
};

#define DEFINE_PROLOGUE(level)                                                  \
    auto _prologue_file_r = TSTRING(__FILE__).reverse();                        \
    constexpr auto _partial_file = ConstString::TSpliter<'/', ' ',              \
            decltype(_prologue_file_r)>::Current::reverse();                    \
    constexpr static Prologue prolog(__func__, _partial_file, __LINE__, level);

#define _IS_LITERAL_STRING(x) \
    (sizeof(#x) > 2 && (#x[0] == '"') && (#x[sizeof(#x) - 2] == '"'))

#define __LOG__(attr, logger, level, first, ...)    ({                 \
        DEFINE_PROLOGUE(level);                                        \
        auto L = [&](ILogOutput* out) __attribute__(attr) {            \
            if (_IS_LITERAL_STRING(first)) {                           \
                return __log__(level, out, prolog,                     \
                               TSTRING(#first).template strip<'\"'>(), \
                               ##__VA_ARGS__);                         \
            } else {                                                   \
                return __log__(level, out, prolog,                     \
                               ConstString::TString<>(), first,        \
                               ##__VA_ARGS__);                         \
            }                                                          \
        };                                                             \
        LogBuilder<decltype(L)>(level, ::std::move(L), &logger);       \
    })


#define LOG_DEBUG(...)  (__LOG__((noinline, cold), default_logger, ALOG_DEBUG, __VA_ARGS__))
#define LOG_INFO(...)   (__LOG__((noinline, cold), default_logger, ALOG_INFO, __VA_ARGS__))
#define LOG_WARN(...)   (__LOG__((noinline, cold), default_logger, ALOG_WARN, __VA_ARGS__))
#define LOG_ERROR(...)  (__LOG__((noinline, cold), default_logger, ALOG_ERROR, __VA_ARGS__))
#define LOG_FATAL(...)  (__LOG__((noinline, cold), default_logger, ALOG_FATAL, __VA_ARGS__))
#define LOG_TEMP(...)                                    \
    {                                                    \
        auto _err_bak = errno;                           \
        __LOG__((noinline, cold), default_logger,        \
                                ALOG_TEMP, __VA_ARGS__); \
        errno = _err_bak;                                \
    }
#ifndef DISABLE_AUDIT
#define LOG_AUDIT(...) (__LOG__((), default_audit_logger, ALOG_AUDIT, __VA_ARGS__))
#else
#define LOG_AUDIT(...)
#endif

inline void set_log_output(ILogOutput* output) {
    default_logger.log_output = output;
}

inline void set_log_output_level(int l)
{
    default_logger.log_level = l;
}

struct ERRNO
{
    int *ptr, no;
    ERRNO() : ptr(&errno), no(*ptr) { }
    constexpr ERRNO(int no_) : ptr(0), no(no_) { }
    void set(int x) { assert(ptr); *ptr = x; }
};

LogBuffer& operator << (LogBuffer& log, ERRNO e);

inline LogBuffer& operator << (LogBuffer& log, const photon::reterr& rvb) {
    auto x = rvb._errno;
    return x ? (log << ERRNO((int)x)) : log;
}

template<typename T> inline
LogBuffer& operator << (LogBuffer& log, const photon::retval<T>& v) {
    return v.succeeded() ? (log << v.get()) : (log << v.error());
}

template<> inline
LogBuffer& operator << <void> (LogBuffer& log, const photon::retval<void>& v) {
    return log << v.error();
}

template<typename T>
struct NamedValue
{
    ALogStringL name;
    typename CopyOrRef<T>::type value;
};

template<size_t N, typename T>
inline NamedValue<T> make_named_value(const char (&name)[N], T&& value)
{
    return NamedValue<T> {ALogStringL(name), std::forward<T>(value)};
}

template <ssize_t N, ssize_t M>
inline NamedValue<const char*> make_named_value(const char (&name)[N],
                                                char (&value)[M]) {
    return {ALogStringL(name), value};
}

#define VALUE(x) make_named_value(#x, x)

template <typename T>
inline LogBuffer& operator<<(LogBuffer& log, const NamedValue<T>& v) {
    return log.printf('[', v.name, '=', v.value, ']');
}

// output a log message, set errno, then return a value
// keep errno unchaged if new_errno == 0
#define LOG_ERROR_RETURN(new_errno, retv, ...) {    \
    int xcode = (int)(new_errno);                   \
    if (xcode == 0) xcode = errno;                  \
    LOG_ERROR(__VA_ARGS__);                         \
    errno = xcode;                                  \
    return retv;                                    \
}

// output a log message with errno info, set errno, then return a value
// keep errno unchaged if new_errno == 0
#define LOG_ERRNO_RETURN(new_errno, retv, ...) {    \
    ERRNO eno;                                      \
    LOG_ERROR(__VA_ARGS__, ' ', eno);               \
    if (new_errno) eno.set(new_errno);              \
    return retv;                                    \
}

// err can be either an error number of int, or an retval<T>
#define LOG_ERROR_RETVAL(err, ...) do {             \
    reterr e{err};                                  \
    LOG_ERROR(__VA_ARGS__, ' ', e);                 \
    return e;                                       \
} while(0)

#define LOG_ERRNO_RETVAL(...) LOG_ERROR_RETVAL(errno, __VA_ARGS__)

// Acts like a LogBuilder
// but able to do operations when log builds
template <typename Builder, typename Append>
struct __LogAppender : public Builder {
    // using Builder members
    using Builder::level;
    using Builder::builder;
    using Builder::logger;
    using Builder::done;
    Append append;
    explicit __LogAppender(Builder&& rhs, Append&& append_)
        : Builder(std::move(rhs)), append(std::move(append_)) {}
    __LogAppender(__LogAppender&& rhs)
        : Builder(std::move(rhs)), append(std::move(rhs.append)) {}
    ~__LogAppender() {
        if (!done && level >= logger->log_level) {
            append(builder(logger->log_output));
            done = true;
        }
    }
};

template <typename Builder, typename Append>
inline __LogAppender<typename std::remove_reference<Builder>::type,
                     typename std::remove_reference<Append>::type>
__log_append_tail(Builder&& builder, Append&& append) {
    return __LogAppender<typename std::remove_reference<Builder>::type,
                         typename std::remove_reference<Append>::type>(
        std::move(builder), std::move(append));
}

template <uint64_t N>
struct __limit_first_n {
    uint64_t count = 0;
    bool operator()() { return (++count) > N; }
    void reset() { count = 0; }
    void append_tail(LogBuffer& buffer) { buffer << " <" << count << " log(s)>\n"; }
};

template <uint64_t N>
struct __limit_every_n {
    uint64_t count = 0;
    bool operator()() { return ((++count) % N) != 1; }
    void reset() { count = 0; }
    void append_tail(LogBuffer& buffer) { buffer << " <" << count << " log(s)>\n"; }
};

template <time_t T>
struct __limit_every_t {
    time_t last = 0;
    time_t now = 0;
    time_t duration = 0;
    bool operator()() {
        now = time(0);
        auto l = last;
        if (l + T <= now) {
            if (l) duration = now - l;
            last = now;
        }
        return l + T > now;
    }
    void reset() { last = 0; }
    void append_tail(LogBuffer& buffer) {
        if (duration)
            buffer << " <last log " << duration << " sec>\n";
        duration = 0;
    }
};

template <uint64_t N, time_t T>
struct __limit_first_n_every_t {
    __limit_every_t<T> lt;
    __limit_first_n<N> lf;
    uint64_t cnt = 0;
    bool operator()() {
        if (!lt()) {
            cnt = lf.count;
            lf.reset();
        }
        return lf();
    }
    void reset() {
        lt.reset();
        lf.reset();
    }
    void append_tail(STFMTLogBuffer& buffer) {
        if (cnt) {
            buffer << " <" << cnt << " log(s) in " << T << " sec>\n";
            cnt = 0;
        }
    }
};

#define __LOG_WITH_LIMIT(type, ...)                                  \
    [&] {                                                            \
        static type limiter;                                         \
        auto __logstat__ = __VA_ARGS__;                              \
        __logstat__.done = limiter();                                \
        return __log_append_tail(std::move(__logstat__),             \
                                 [](STFMTLogBuffer buffer) {         \
                                     limiter.append_tail(buffer);    \
                                 });                                 \
    }()

// output log once every T seconds.
// example: LOG_EVERY_T(10, LOG_INFO(....))
// this log line will only log once in 10 secs.
// useful for logs in repeated checking
#define LOG_EVERY_T(T, ...) __LOG_WITH_LIMIT(__limit_every_t<T>, __VA_ARGS__)

// output log once every N rounds.
// example: LOG_EVERY_N(10, LOG_INFO(....))
// this log line will only print 1 time every 10 rounds.
#define LOG_EVERY_N(N, ...) __LOG_WITH_LIMIT(__limit_every_n<N>, __VA_ARGS__)

// output log only first N rounds.
// example: LOG_FIRST_N(10, LOG_INFO(....))
// this log line will only print 10 rounds, after that, nothing will logged
#define LOG_FIRST_N(N, ...) __LOG_WITH_LIMIT(__limit_first_n<N>, __VA_ARGS__)

// output log only first N rounds every T seconds.
// example: LOG_FIRST_N_EVERY_T(10, 60, LOG_INFO(....))
// this log line will only print 10 rounds every 60 seconds.
// Because of macro can not parse template correctly,
// here used a scoped type alais
#define LOG_FIRST_N_EVERY_T(N, T, ...)                      \
    ({                                                      \
        using __limit_type = __limit_first_n_every_t<N, T>; \
        __LOG_WITH_LIMIT(__limit_type, __VA_ARGS__);        \
    })
