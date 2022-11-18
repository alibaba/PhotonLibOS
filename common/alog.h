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
    int level;
    char* ptr;
    uint32_t size;
    uint32_t reserved;
    void consume(size_t n) { ptr += n; size -= n; }
};

#define ALOG_DEBUG 0
#define ALOG_INFO  1
#define ALOG_WARN  2
#define ALOG_ERROR 3
#define ALOG_FATAL 4
#define ALOG_TEMP  5
#define ALOG_AUDIT 6

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

struct LogBuffer;
struct ALogLogger {
    int log_level;
    ILogOutput* log_output;

    template <typename T>
    ALogLogger& operator<<(T&& rhs) {
        rhs.logger = this;
        return *this;
    }
};

extern ALogLogger default_logger;
extern ALogLogger default_audit_logger;

inline int get_log_file_fd() { return default_logger.log_output->get_log_file_fd(); }

extern int& log_output_level;
extern ILogOutput*& log_output;

static LogFormatter log_formatter;

struct LogBuffer : public ALogBuffer
{
public:
    LogBuffer(ILogOutput *output): log_output(output)
    {
        ptr = buf;
        size = sizeof(buf) - 2;
    }
    ~LogBuffer()
    {
        printf('\n');
        *ptr = '\0';
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
    char buf[LOG_BUFFER_SIZE];
    ILogOutput *log_output;
    void operator = (const LogBuffer& rhs);
    LogBuffer(const LogBuffer& rhs);
};

struct Prologue
{
    uint64_t addr_func, addr_file;
    int len_func, len_file;
    int line, level;
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

template<typename FMT, typename...Ts> __attribute__((noinline, cold))
static void __log__(int level, ILogOutput* output, const Prologue& prolog, FMT fmt, Ts&&...xs)
{
    STFMTLogBuffer log(output);
    log << prolog;
    log.print_fmt(fmt, std::forward<Ts>(xs)...);
    log.level = level;
}

template<typename BuilderType>
struct LogBuilder {
    int level;
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
    ~LogBuilder() {
        if (!done && level >= logger->log_level) {
            builder(logger->log_output);
            done = true;
        }
    }
};

#if __GNUC__ >= 9
#define DEFINE_PROLOGUE(level, prolog)                                          \
    static constexpr const char* _prologue_func = __func__;                     \
    const static Prologue prolog{                                               \
        (uint64_t) _prologue_func,  (uint64_t)__FILE__, sizeof(__func__) - 1,   \
        sizeof(__FILE__) - 1, __LINE__, level};
#else
#define DEFINE_PROLOGUE(level, prolog)                                  \
    const static Prologue prolog{                                       \
        (uint64_t) __func__,  (uint64_t)__FILE__, sizeof(__func__) - 1, \
        sizeof(__FILE__) - 1, __LINE__, level};
#endif

#define _IS_LITERAL_STRING(x) \
    (sizeof(#x) > 2 && (#x[0] == '"') && (#x[sizeof(#x) - 2] == '"'))

#define __LOG__(logger, level, first, ...)                               \
    ({                                                                   \
        DEFINE_PROLOGUE(level, prolog);                                  \
        auto __build_lambda__ = [&](ILogOutput* __output_##__LINE__) {   \
            if (_IS_LITERAL_STRING(first)) {                             \
                __log__(level, __output_##__LINE__, prolog,              \
                        TSTRING(#first).template strip<'\"'>(),          \
                        ##__VA_ARGS__);                                  \
            } else {                                                     \
                __log__(level, __output_##__LINE__, prolog,              \
                        ConstString::TString<>(), first, ##__VA_ARGS__); \
            }                                                            \
        };                                                               \
        LogBuilder<decltype(__build_lambda__)>(                          \
            level, ::std::move(__build_lambda__), &logger);                \
    })

#define LOG_DEBUG(...) (__LOG__(default_logger, ALOG_DEBUG, __VA_ARGS__))
#define LOG_INFO(...) (__LOG__(default_logger, ALOG_INFO, __VA_ARGS__))
#define LOG_WARN(...) (__LOG__(default_logger, ALOG_WARN, __VA_ARGS__))
#define LOG_ERROR(...) (__LOG__(default_logger, ALOG_ERROR, __VA_ARGS__))
#define LOG_FATAL(...) (__LOG__(default_logger, ALOG_FATAL, __VA_ARGS__))
#define LOG_TEMP(...)                                    \
    {                                                    \
        auto _err_bak = errno;                           \
        __LOG__(default_logger, ALOG_TEMP, __VA_ARGS__); \
        errno = _err_bak;                                \
    }
#define LOG_AUDIT(...) (__LOG__(default_audit_logger, ALOG_AUDIT, __VA_ARGS__))

inline void set_log_output(ILogOutput* output) {
    default_logger.log_output = output;
}

inline void set_log_output_level(int l)
{
    default_logger.log_level = l;
}

struct ERRNO
{
    const int no;
    ERRNO() : no(errno) { }
    constexpr ERRNO(int no_) : no(no_) { }
};

inline LogBuffer& operator << (LogBuffer& log, ERRNO e)
{
    auto no = e.no ? e.no : errno;
    return log.printf("errno=", no, '(', strerror(no), ')');
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

#define VALUE(x) make_named_value(#x, x)

template<typename T>
inline LogBuffer& operator << (LogBuffer& log, const NamedValue<T>& v)
{
    return log.printf('[', v.name, '=', v.value, ']');
}

inline LogBuffer& operator << (LogBuffer& log, const NamedValue<ALogString>& v)
{
    return log.printf('[', v.name, '=', '"', v.value, '"', ']');
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
    errno = (new_errno) ? (new_errno) : eno.no;     \
    return retv;                                    \
}

