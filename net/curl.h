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
#include <cassert>
#include <curl/curl.h>

#include <initializer_list>
#include <string>
#include <unordered_map>

#include <photon/common/alog-functionptr.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog.h>
#include <photon/common/estring.h>
#include <photon/common/iovector.h>

namespace photon {
namespace net {

int libcurl_init(long flags = 3 /*CURL_GLOBAL_DEFAULT*/, long pipelining = 0,
                 long maxconn = 32);

int libcurl_set_pipelining(long val);

int libcurl_set_maxconnects(long val);

int curl_perform(CURL* curl, uint64_t timeout);

void libcurl_fini();

std::string url_escape(const char*);
std::string url_unescape(const char*);

inline void convert(const std::string& v, uint64_t& value) {
    value = std::atoll(v.c_str());
}

inline void convert(const std::string& v, ssize_t& value) {
    value = std::atoll(v.c_str());
}

inline void convert(const std::string& v, char* value, size_t size) {
    strncpy(value, v.data(), size - 1);
    value[size - 1] = 0;
}

template <size_t N>
inline void convert(const std::string& v, char (&value)[N]) {
    convert(v, value, N);
}

class HeaderMap : public std::unordered_map<std::string, std::string> {
protected:
    std::string&& str_lower(std::string&& str) {
        for (auto& ch : str) {
            ch = std::tolower(ch);
        }
        return std::move(str);
    }

public:
    size_t write(const void* ptr, size_t n) {
        estring_view st((const char*)ptr, n);
        auto pos = st.find_first_of(':');
        if (pos != estring::npos) {
            // somehow http header is defined case insensitive
            (*this)[str_lower(st.substr(0, pos).trim())] =
                st.substr(pos + 1).trim();
        }
        return n;
    }

    bool exists(std::initializer_list<const char*> keys) {
        for (auto& k : keys)
            if (count(k) == 0) return false;
        return true;
    }

    template <typename T>
    int try_get(const char* key, T& value) {
        auto it = find(str_lower(key));
        if (it == end()) return -1;

        convert(it->second, value);
        return 0;
    }
};

class BufWriter {
public:
    BufWriter(void* buf_, size_t capacity_)
        : m_capacity(capacity_), m_buf(buf_) {}
    size_t write(const void* ptr, size_t n) {
        auto free = m_capacity - m_size;
        if (free < n) n = free;
        memcpy((char*)m_buf + m_size, ptr, n);
        m_size += n;
        return n;
    }
    size_t size() { return m_size; }
    size_t capacity() { return m_capacity; }
    void add_null_terminator() {
        auto i = m_size;
        if (i >= m_capacity) i--;
        static_cast<char*>(m_buf)[i] = '\0';
    }
    ALogString alog_string() { return ALogString((const char*)m_buf, m_size); }

protected:
    size_t m_size = 0, m_capacity;
    void* m_buf;
};

template <size_t N>
class BufferWriter : public BufWriter {
public:
    char buffer[N];
    BufferWriter() : BufWriter(buffer, N) {}
};

class StringWriter {
public:
    std::string string;
    size_t write(const void* buf, size_t size) {
        string.append((const char*)buf, size);
        return size;
    }
    ALogString alog_string() { return ALogString(string.c_str(), string.length()); }
};

struct IOVWriter : public IOVector {
    using IOVector::IOVector;
    size_t drop = 0;
    size_t written = 0;
    size_t write(const void* buf, size_t size) {
        auto actual_count = memcpy_from(
            buf, size);  // copy from (buf, size) to iovec[] in *this,
        written += actual_count;
        extract_front(actual_count);  // and extract the portion just copied
        if (actual_count < size)      // means full
        {
            drop += size - actual_count;
        }
        return size;
    }
};

class BufReader {
public:
    BufReader(const void* buf_, size_t capacity_)
        : m_size(capacity_),
          m_capacity(capacity_),
          m_buf((char*)buf_ + capacity_) {}
    size_t read(void* ptr, size_t n) {
        if (n > m_size) n = m_size;
        memcpy(ptr, (const char*)m_buf - m_size, n);
        m_size -= n;
        return n;
    }
    size_t size() { return m_size; }
    size_t capacity() { return m_capacity; }

protected:
    size_t m_size, m_capacity;
    const void* m_buf;
};

template <size_t N>
class BufferReader : public BufReader {
public:
    char buffer[N];
    BufferReader() : BufReader(buffer, N) {}
};

// struct VOID
struct DummyReaderWriter {
    size_t read(void*, size_t n) { return n; }
    size_t write(const void*, size_t n) { return n; }
};

class URL_Buf {
public:
    URL_Buf(uint32_t capacity) : m_capacity(capacity) {}
    uint32_t size() { return m_size; }
    uint32_t capacity() { return m_capacity; }
    bool overflow() { return m_size > m_capacity - 1; }
    operator const char*() { return m_url; }
    template <typename... Ts>
    void snp_append(const char* fmt, const Ts&... xs) {
        if (!overflow())
            m_size += snprintf((char*)m_url + m_size, m_capacity - m_size, fmt,
                               xs...);
    }

protected:
    uint32_t m_size = 0, m_capacity;
    char m_url[0];
};

template <size_t N>
class URLBuffer : public URL_Buf {
public:
    URLBuffer() : URL_Buf(N) {}

protected:
    char m_url[N];
};

class cURL {
public:
    // possible flags: CURL_GLOBAL_ALL, CURL_GLOBAL_SSL, CURL_GLOBAL_WIN32
    // CURL_GLOBAL_NOTHING, CURL_GLOBAL_DEFAULT, CURL_GLOBAL_ACK_EINTR
    static int init(long flags = CURL_GLOBAL_ALL, long pipelining = 0,
                    long maxconn = 32) {
        return net::libcurl_init(flags, pipelining, maxconn);
    }
    static int fini() {
        net::libcurl_fini();
        return 0;
    }
    cURL() {
        m_curl = curl_easy_init();
        if (!m_curl) {
            ret = CURLcode::CURLE_INTERFACE_FAILED;
            strcpy(m_errmsg, "failed to create curl object");
            return;
        }
        setopt(CURLOPT_ERRORBUFFER, m_errmsg);
        setopt(CURLOPT_DNS_USE_GLOBAL_CACHE, 0L);
        setopt(CURLOPT_NOSIGNAL, 1L);
        setopt(CURLOPT_TCP_NODELAY, 1L);
        m_errmsg[0] = '\0';
    }
    ~cURL() { curl_easy_cleanup(m_curl); }
    void reset_error() { ret = CURLE_OK; }
    cURL& reset() {
        curl_easy_reset(m_curl);
        return *this;
    }
    cURL& setopt(CURLoption option, long arg) { return _setopt(option, arg); }
    cURL& setopt(CURLoption option, void* arg) { return _setopt(option, arg); }
    cURL& setopt(CURLoption option, const char* arg) {
        return _setopt(option, (void*)arg);
    }
    template <typename Ret, typename... Args>
    cURL& setopt(CURLoption option, Ret (*arg)(Args...)) {
        return _setopt(option, arg);
    }
    cURL& set_user_passwd(const char* user, const char* passwd) {
        return setopt(CURLOPT_USERNAME, user).setopt(CURLOPT_PASSWORD, passwd);
    }
    cURL& set_nobody() { return setopt(CURLOPT_NOBODY, 1); }
    // TODO: https://curl.haxx.se/libcurl/c/CURLOPT_HTTPHEADER.html
    //   When this option is passed to curl_easy_setopt, libcurl will not
    //   copy the entire list so you must keep it around until you no longer
    //   use this handle for a transfer before you call curl_slist_free_all
    //   on the list.
    //  this set_headers construct a slist and then destroy it, just after
    //  setopt, but before calling perform. That cause SEGFAULT
    // template<typename...Ts> // xs can be either char* or std::string
    // cURL& set_headers(const Ts&...xs)
    // {
    //     return setopt(CURLOPT_HTTPHEADER, slist(xs...).list);
    // }
    cURL& set_range(uint64_t start, uint64_t end) {
        char range[128];
        // seems like llu refer long long unsigned int, uint64_t refers to long
        // unsigned int, so %lu should be good without warning
        snprintf(range, sizeof(range), "%llu-%llu", (long long unsigned)start,
                 (long long unsigned)end);   // for compatiblity on macOS
        return setopt(CURLOPT_RANGE, range);
    }
    cURL& set_cafile(const char* cafile) {
        long v = (bool)cafile;
        return setopt(CURLOPT_CAINFO, cafile),
               setopt(CURLOPT_SSL_VERIFYPEER, v);
    }
    cURL& set_redirect(uint32_t max_redir) {
        long v = (bool)max_redir;
        return setopt(CURLOPT_FOLLOWLOCATION, v),
               setopt(CURLOPT_MAXREDIRS, max_redir);
    }
    cURL& set_proxy(const char* proxy) {
        return setopt(CURLOPT_PROXY, proxy);
    }
    cURL& set_noproxy(const char* proxy) {
        return setopt(CURLOPT_NOPROXY, proxy);
    }
    template <typename T>
    cURL& set_header_container(T* stream) {
        return setopt(CURLOPT_HEADERDATA, (void*)stream),
               setopt(CURLOPT_HEADERFUNCTION, &writer<T>);
    }
    // Turn on/off verbose, log to ALOG
    // ATTENTION: during verbose mode on, make sure ALOG configured.
    // modify ALOG configurations during verbose on may cause
    // unpredictable troubles.
    cURL& set_verbose(bool on = true) {
        if (on) {
            auto fd = get_log_file_fd();
            if (fd < 0) return *this;
            auto file = fdopen(fd, "a");
            return setopt(CURLOPT_STDERR, (void*)file),
                   setopt(CURLOPT_VERBOSE, 1L);
        } else {
            return setopt(CURLOPT_VERBOSE, 0L), setopt(CURLOPT_STDERR, stderr);
        }
    }
    template <typename T>
    T getinfo(CURLINFO info) {
        // what the asserts for ? expect to fail?
        // assert(ret != CURLE_OK);
        T value;
        ret = curl_easy_getinfo(m_curl, info, &value);
        // assert(ret != CURLE_OK);
        return value;
    }
    long get_response_code() {
        if (ret == CURLE_OK)
            return getinfo<long>(CURLINFO_RESPONSE_CODE);
        errno = EIO;
        return -1;
    }
    // the stream of T* could be anything that has write(ptr, size)
    template <typename T>  // method, like std::string*
    long GET(const char* url, T* stream, uint64_t timeout = -1) {
        setopt(CURLOPT_HTTPGET, 1L);
        setopt(CURLOPT_URL, url);
        setopt(CURLOPT_HTTPHEADER, headers.list);
#if LIBCURL_VERSION_MAJOR > 7 || LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 37
        setopt(CURLOPT_PROXYHEADER, proxy_headers.list);
#endif
        set_write_stream(stream);
        ret = (CURLcode)net::curl_perform(m_curl, timeout);
        return get_response_code();
    }
    long GET(const char* url, uint64_t timeout = -1) {
        set_nobody();
        setopt(CURLOPT_HTTPGET, 1L);
        setopt(CURLOPT_URL, url);
        setopt(CURLOPT_HTTPHEADER, headers.list);
#if LIBCURL_VERSION_MAJOR > 7 || LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 37
        setopt(CURLOPT_PROXYHEADER, proxy_headers.list);
#endif
        ret = (CURLcode)net::curl_perform(m_curl, timeout);
        return get_response_code();
    }
    template <typename T>  // method, like std::string*
    long HEAD(const char* url, T* stream, uint64_t timeout = -1) {
        set_nobody();
        setopt(CURLOPT_URL, url);
        setopt(CURLOPT_CUSTOMREQUEST, "HEAD");
        setopt(CURLOPT_HTTPHEADER, headers.list);
#if LIBCURL_VERSION_MAJOR > 7 || LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 37
        setopt(CURLOPT_PROXYHEADER, proxy_headers.list);
#endif
        set_write_stream(stream);
        ret = (CURLcode)net::curl_perform(m_curl, timeout);
        return get_response_code();
    }
    long HEAD(const char* url, uint64_t timeout = -1) {
        set_nobody();
        setopt(CURLOPT_URL, url);
        setopt(CURLOPT_CUSTOMREQUEST, "HEAD");
        setopt(CURLOPT_HTTPHEADER, headers.list);
        ret = (CURLcode)net::curl_perform(m_curl, timeout);
        return get_response_code();
    }
    template <typename W>
    long POST(const char* url, W* wstream, uint64_t timeout = -1) {
        return POST(url, (DummyReaderWriter*)nullptr, wstream, timeout);
    }
    template <typename W>
    long POST(const char* url, const char* post_fields, W* wstream,
              uint64_t timeout = -1) {
        setopt(CURLOPT_HTTPHEADER, headers.list);
#if LIBCURL_VERSION_MAJOR > 7 || LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 37
        setopt(CURLOPT_PROXYHEADER, proxy_headers.list);
#endif
        setopt(CURLOPT_POSTFIELDS, post_fields);
        return POST(url, wstream, timeout);
    }
    template <typename R, typename W>
    long POST(const char* url, R* rstream, W* wstream, uint64_t timeout = -1) {
        set_read_stream(rstream);
        set_write_stream(wstream);
        setopt(CURLOPT_URL, url);
        setopt(CURLOPT_POST, 1L);
        setopt(CURLOPT_HTTPHEADER, headers.list);
#if LIBCURL_VERSION_MAJOR > 7 || LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 37
        setopt(CURLOPT_PROXYHEADER, proxy_headers.list);
#endif
        ret = (CURLcode)net::curl_perform(m_curl, timeout);
        return get_response_code();
    }
    template <typename R = DummyReaderWriter, typename W = DummyReaderWriter>
    long PUT(const char* url, R* rstream = (DummyReaderWriter*)nullptr,
             W* wstream = (DummyReaderWriter*)nullptr, uint64_t timeout = -1) {
        set_read_stream(rstream);
        set_write_stream(wstream);
        setopt(CURLOPT_UPLOAD, 1L);
        setopt(CURLOPT_PUT, 1L);
        setopt(CURLOPT_URL, url);
        setopt(CURLOPT_HTTPHEADER, headers.list);
#if LIBCURL_VERSION_MAJOR > 7 || LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 37
        setopt(CURLOPT_PROXYHEADER, proxy_headers.list);
#endif
        // setopt(CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_info.st_size);
        ret = (CURLcode)net::curl_perform(m_curl, timeout);
        return get_response_code();
    }
    long DELETE(const char* url, uint64_t timeout = -1) {
        setopt(CURLOPT_URL, url);
        setopt(CURLOPT_CUSTOMREQUEST, "DELETE");
        setopt(CURLOPT_HTTPHEADER, headers.list);
#if LIBCURL_VERSION_MAJOR > 7 || LIBCURL_VERSION_MAJOR == 7 && LIBCURL_VERSION_MINOR >= 37
        setopt(CURLOPT_PROXYHEADER, proxy_headers.list);
#endif
        ret = (CURLcode)net::curl_perform(m_curl, timeout);
        return get_response_code();
    }
    template <typename W>
    long DELETE(const char* url, W* wstream, uint64_t timeout = -1) {
        set_write_stream(wstream);
        return DELETE(url, timeout);
    }

    const char* error_message() { return m_errmsg; }

    cURL& append_header(const std::string& key, const std::string& val) {
        headers.append(key + std::string(": ") + val);
        return *this;
    }

    cURL& clear_header() {
        headers.clear();
        return *this;
    }

    cURL& append_proxy_header(const std::string& key, const std::string& val) {
        proxy_headers.append(key + std::string(": ") + val);
        return *this;
    }

    cURL& clear_proxy_header() {
        proxy_headers.clear();
        return *this;
    }

    struct slist {
        struct curl_slist* list = nullptr;
        slist() = default;
        template <typename... Ts>
        slist(const Ts&... xs) {
            append(xs...);
        }
        ~slist() { clear(); }

        template <typename T, typename... Ts>
        void append(const T& x, const Ts&... xs) {
            append(x);
            append(xs...);
        }
        void append(const char* h) { list = curl_slist_append(list, h); }
        void append(const std::string& h) { append(h.c_str()); }
        void clear() {
            curl_slist_free_all(list);
            list = nullptr;
        }
    };

protected:
    CURL* m_curl;
    char m_errmsg[CURL_ERROR_SIZE];
    CURLcode ret = CURLE_OK;
    slist headers, proxy_headers;
    template <typename T>
    cURL& _setopt(CURLoption option, T arg) {
        if (ret != CURLE_OK) return *this;
        ret = curl_easy_setopt(m_curl, option, arg);
        if (ret != CURLE_OK)
            LOG_WARN("failed to curl_easy_setopt(m_curl, option=`, arg=`)",
                     option, arg);
        return *this;
    }
    template <typename T>
    static size_t writer(void* buf, size_t size, size_t num, void* file) {
        return static_cast<T*>(file)->write(buf, size * num);
    }
    template <typename W>
    void set_write_stream(W* stream) {
        if (!stream) return;
        setopt(CURLOPT_WRITEFUNCTION, &writer<W>);
        setopt(CURLOPT_WRITEDATA, stream);
    }
    template <typename T>
    static size_t reader(void* buf, size_t size, size_t num, void* file) {
        return static_cast<T*>(file)->read(buf, size * num);
    }
    template <typename R>
    void set_read_stream(R* stream) {
        if (!stream) return;
        setopt(CURLOPT_READFUNCTION, &reader<R>);
        setopt(CURLOPT_READDATA, stream);
    }
};
}  // namespace net
}
