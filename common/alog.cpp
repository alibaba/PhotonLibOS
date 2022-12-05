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

#include "alog.h"
#include "lockfree_queue.h"
#include "photon/thread/thread.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <algorithm>
#include <thread>
#include <chrono>
#include <sys/uio.h>
using namespace std;

class BaseLogOutput : public ILogOutput {
public:
    uint64_t throttle = -1UL;
    uint64_t count = 0;
    time_t ts = 0;
    int log_file_fd;

    constexpr BaseLogOutput(int fd = 0) : log_file_fd(fd) { }

    void write(int, const char* begin, const char* end) override {
        ::write(log_file_fd, begin, end - begin);
        throttle_block();
    }
    void throttle_block() {
        if (throttle == -1UL) return;
        time_t t = time(0);
        if (t > ts) {
            ts = t;
            count = 0;
        }
        if (++count > throttle) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    virtual int get_log_file_fd() override { return log_file_fd; }
    virtual uint64_t set_throttle(uint64_t t = -1) override { return throttle = t; }
    virtual uint64_t get_throttle() override { return throttle; }
    virtual void destruct() override { }
};

static BaseLogOutput _log_output_stdout(1);
static BaseLogOutput _log_output_stderr(2);
ILogOutput* const log_output_stdout = &_log_output_stdout;
ILogOutput* const log_output_stderr = &_log_output_stderr;

class LogOutputNull : public BaseLogOutput {
public:
    void write(int, const char* , const char* ) override { throttle_block(); }
};

static LogOutputNull _log_output_null;
ILogOutput* const log_output_null = &_log_output_null;

ALogLogger default_logger {ALOG_DEBUG, log_output_stdout};
ALogLogger default_audit_logger {ALOG_AUDIT, log_output_null};

int &log_output_level = default_logger.log_level;
ILogOutput* &log_output = default_logger.log_output;

void LogFormatter::put(ALogBuffer& buf, FP x)
{
    char _fmt[64];
    ALogBuffer fmt {0, _fmt, sizeof(_fmt)};
    put(fmt, '%');
    if (x.width() >= 0)
    {
        put(fmt, (uint64_t)x.width());
    }
    // precision and width should be independent. like %.2f
    if (x.precision() >= 0)
    {
        put(fmt, '.');
        put(fmt, (uint64_t)x.precision());
    }
    put(fmt, x.scientific() ? 'e' : 'f');
    put(fmt, '\0');
    // this snprintf is a method of LogFormatter obj, not the one in stdio

    snprintf(buf, _fmt, x.value());
}

static inline void put_uint64(LogFormatter* log, ALogBuffer& buf, uint64_t x)
{
    do { log->put(buf, (char)('0' + x % 10));
    } while((x /= 10) > 0);
}

void LogFormatter::put_integer(ALogBuffer& buf, uint64_t x)
{
    auto begin = buf.ptr;
    put_uint64(this, buf, x);
    std::reverse(begin, buf.ptr);
}

static void move_and_fill(char* begin, char*& ptr, uint64_t width, char padding)
{
    auto end = begin + width;
    if (end > ptr)
    {
        auto len = ptr - begin;
        auto padding_len = end - ptr;
        memmove(begin + padding_len, begin, len);
        memset(begin, padding, padding_len);
        ptr = end;
    }
}

void LogFormatter::put_integer_hbo(ALogBuffer& buf, ALogInteger X)
{
    // print (in reversed order)
    auto x = X.uvalue();
    auto shift = X.shift();
    unsigned char mask = (1UL << shift) - 1;
    auto begin = buf.ptr;
    do { put(buf, "0123456789ABCDEFH" [x & mask]);
    } while (x >>= shift);

    std::reverse(begin, buf.ptr);
    auto ptr = buf.ptr;
    move_and_fill(begin, ptr, X.width(), X.padding());
    if (ptr > buf.ptr) {
        buf.size -= (ptr - buf.ptr);
        buf.ptr = ptr;
    }
}

//static inline void insert_comma(char* begin, char*& ptr, uint64_t width, char padding)
static void insert_commas(char*& digits_end, uint64_t ndigits)
{
    if (ndigits <= 0) return;
    auto ncomma = (ndigits - 1) / 3;
    auto psrc = digits_end;
    digits_end += ncomma;
    auto pdest = digits_end;

    struct temp
    {
        char data[4];
        temp(const char* psrc)
        {
            *(uint32_t*)data = *(uint32_t*)(psrc-1);
            data[0] = ',';
        }
    };

    while (pdest != psrc)
    {
        psrc -= 3; pdest -= 4;
        *(temp*)pdest = temp(psrc);
    }
}

void LogFormatter::put_integer_dec(ALogBuffer& buf, ALogInteger x)
{
    uint64_t ndigits;
    auto begin = buf.ptr;
    // print (in reversed order)
    if (!x.is_signed() || x.svalue() > 0)
    {
        put_uint64(this, buf, x.uvalue());
        ndigits = buf.ptr - begin;
    }
    else
    {
        put_uint64(this, buf, -x.svalue());
        ndigits = buf.ptr - begin;
        put(buf, '-');
    }

    std::reverse(begin, buf.ptr);
    auto ptr = buf.ptr;
    if (x.comma()) insert_commas(ptr, ndigits);
    move_and_fill(begin, ptr, x.width(), x.padding());
    if (ptr > buf.ptr) {
        buf.size -= (ptr - buf.ptr);
        buf.ptr = ptr;
    }
}

static time_t dayid = 0;
static struct tm alog_time = {0};
struct tm* alog_update_time(time_t now)
{
    auto now0 = now;
    int sec = now % 60;    now /= 60;
    int min = now % 60;    now /= 60;
    int hor = now % 24;    now /= 24;
    if (now != dayid)
    {
        dayid = now;
        gmtime_r(&now0, &alog_time);
        alog_time.tm_year+=1900;
        alog_time.tm_mon++;
    }
    else
    {
        alog_time.tm_sec = sec;
        alog_time.tm_min = min;
        alog_time.tm_hour = hor;
    }
    return &alog_time;
}

static struct tm* alog_update_time()
{
    return alog_update_time(time(0) + 8 * 60 * 60);
}

class LogOutputFile final : public BaseLogOutput {
public:
    uint64_t log_file_size_limit = 0;
    char* log_file_name = nullptr;
    atomic<uint64_t> log_file_size{0};
    unsigned int log_file_max_cnt = 10;

    virtual void destruct() override {
        log_output_file_close();
        delete this;
    }

    int fopen(const char* fn) {
        auto mode   =   S_IRUSR | S_IWUSR  | S_IRGRP | S_IROTH;
        return open(fn, O_CREAT | O_WRONLY | O_APPEND, mode);
    }

    void write(int, const char* begin, const char* end) override {
        if (log_file_fd < 0) return;
        uint64_t length = end - begin;
        iovec iov{(void*)begin, length};
        ::writev(log_file_fd, &iov, 1); // writev() is atomic, whereas write() is not
        throttle_block();
        if (log_file_name && log_file_size_limit) {
            log_file_size += length;
            if (log_file_size > log_file_size_limit) {
                static mutex log_file_lock;
                lock_guard<mutex> guard(log_file_lock);
                if (log_file_size > log_file_size_limit) {
                    log_file_rotate();
                    reopen_log_output_file();
                }
            }
        }
    }

    static inline void add_generation(char* buf, int size,
                                      unsigned int generation) {
        if (generation == 0) {
            buf[0] = '\0';
        } else {
            snprintf(buf, size, ".%u", generation);
        }
    }

    void log_output_file_setting(int fd) {
        if (fd < 0)
            return;
        if (log_file_fd > 2 && log_file_fd != fd)
            close(log_file_fd);

        log_file_fd = fd;
        log_file_size.store(lseek(fd, 0, SEEK_END));
        free(log_file_name);
        log_file_name = nullptr;
        log_file_size_limit = 0;
    }

    int log_output_file_setting(const char* fn, uint64_t rotate_limit,
                                int max_log_files) {
        int fd = fopen(fn);
        if (fd < 0) return -1;

        log_output_file_setting(fd);
        free(log_file_name);
        log_file_name = strdup(fn);
        log_file_size_limit = max(rotate_limit, (uint64_t)(1024 * 1024));
        log_file_max_cnt = min(max_log_files, 30);
        return 0;
    }

    void reopen_log_output_file() {
        int fd = fopen(log_file_name);
        if (fd < 0) {
            static char msg[] = "failed to open log output file: ";
            ::write(log_file_fd, msg, sizeof(msg) - 1);
            if (log_file_name)
                ::write(log_file_fd, log_file_name, strlen(log_file_name));
            ::write(log_file_fd, "\n", 1);
            return;
        }

        log_file_size = 0;
        dup2(fd, log_file_fd);  // to make sure log_file_fd
        close(fd);              // doesn't change
    }

    void log_file_rotate() {
        if (!log_file_name || access(log_file_name, F_OK) != 0) return;

        int fn_length = (int)strlen(log_file_name);
        char fn0[PATH_MAX], fn1[PATH_MAX];
        strcpy(fn0, log_file_name);
        strcpy(fn1, log_file_name);

        unsigned int last_generation = 1;  // not include
        while (true) {
            add_generation(fn0 + fn_length, sizeof(fn0) - fn_length,
                           last_generation);
            if (0 != access(fn0, F_OK)) break;
            last_generation++;
        }

        while (last_generation >= 1) {
            add_generation(fn0 + fn_length, sizeof(fn0) - fn_length,
                           last_generation - 1);
            add_generation(fn1 + fn_length, sizeof(fn1) - fn_length,
                           last_generation);

            if (last_generation >= log_file_max_cnt) {
                unlink(fn0);
            } else {
                rename(fn0, fn1);
            }
            last_generation--;
        }
    }

    int log_output_file_close() {
        if (log_file_fd < 0) {
            errno = EALREADY;
            return -1;
        }
        close(log_file_fd);
        log_file_fd = -1;
        free(log_file_name);
        log_file_name = nullptr;
        return 0;
    }
};

class AsyncLogOutput final : public ILogOutput {
public:
    ILogOutput* log_output;
    std::mutex mt;
    std::condition_variable cv;
    std::thread background;
    LockfreeSPSCRingQueue<iovec, 16384> pending;
    photon::spinlock lock;
    bool stopped = false;

    AsyncLogOutput(ILogOutput* output) : log_output(output) {
        background = std::thread([&] {
            auto log_file_fd = log_output->get_log_file_fd();
            auto wb = [this, log_file_fd] {
                iovec iov;
                while (pending.pop(iov)) {
                    log_output->write(log_file_fd, (char*)iov.iov_base, (char*)iov.iov_base + iov.iov_len);
                    delete[] (char*)iov.iov_base;
                }
            };
            while (!stopped) {
                uint64_t interval = 1000UL;
                if (!pending.empty()) {
                    interval = 100UL;
                    wb();
                }
                std::unique_lock<std::mutex> l(mt);
                if (!stopped) cv.wait_for(l, std::chrono::milliseconds(interval));
            }
            if (!pending.empty()) wb();
        });
    }

    void write(int, const char* begin, const char* end) override {
        uint64_t length = end - begin;
        auto buf = new char[length];
        memcpy(buf, begin, length);
        iovec iov{buf, length};
        bool pushed = ({
            SCOPED_LOCK(lock);
            pending.push(iov);
        });
        if (!pushed) {
            delete[] buf;
            cv.notify_all();
        }
    }
    virtual int get_log_file_fd() override { return log_output->get_log_file_fd(); }
    virtual uint64_t set_throttle(uint64_t t = -1UL) override { return log_output->set_throttle(t); }
    virtual uint64_t get_throttle() override { return log_output->get_throttle(); }
    virtual void destruct() override {
        if (!stopped) {
            {
                std::unique_lock<std::mutex> l(mt);
                stopped = true;
                cv.notify_all();
            }
            background.join();
        }
        delete this;
    }
};

ILogOutput* new_log_output_file(const char* fn, uint64_t rotate_limit,
                                int max_log_files, uint64_t throttle) {
    auto ret = new LogOutputFile();
    if (ret->log_output_file_setting(fn, rotate_limit, max_log_files) < 0) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to open log file ", fn);
        return nullptr;
    }
    ret->set_throttle(throttle);
    return ret;
}

ILogOutput* new_log_output_file(int fd, uint64_t throttle) {
    auto ret = new LogOutputFile();
    ret->log_output_file_setting(fd);
    ret->set_throttle(throttle);
    return ret;
}

ILogOutput* new_async_log_output(ILogOutput* output) {
    return output ? new AsyncLogOutput(output) : nullptr;
}

// default_log_file is not defined in header
// so that user can not operate it unless using
// log_output_file(...) and log_output_file_close()
static LogOutputFile default_log_output_file;

int log_output_file(int fd, uint64_t throttle) {
    default_log_output_file.log_output_file_setting(fd);
    default_log_output_file.set_throttle(throttle);
    default_logger.log_output = &default_log_output_file;
    return 0;
}

int log_output_file(const char* fn, uint64_t rotate_limit, int max_log_files, uint64_t throttle) {
    int ret = default_log_output_file.log_output_file_setting(
        fn, rotate_limit, max_log_files);
    if (ret < 0)
        LOG_ERROR_RETURN(0, -1, "Fail to open log file ", fn);
    default_log_output_file.set_throttle(throttle);
    default_logger.log_output = &default_log_output_file;
    return 0;
}

int log_output_file_close() {
    default_log_output_file.log_output_file_close();
    if (default_logger.log_output == &default_log_output_file)
        default_logger.log_output = log_output_stdout;
    return 0;
}

namespace photon
{
    struct thread;
    extern __thread thread* CURRENT;
}

static inline ALogInteger DEC_W2P0(uint64_t x)
{
    return DEC(x).width(2).padding('0');
}

LogBuffer& operator << (LogBuffer& log, const Prologue& pro)
{
#ifdef LOG_BENCHMARK
    auto t = &alog_time;
#else
    auto t = alog_update_time();
#endif
    log.printf(t->tm_year, '/');
    log.printf(DEC_W2P0(t->tm_mon),  '/');
    log.printf(DEC_W2P0(t->tm_mday), ' ');
    log.printf(DEC_W2P0(t->tm_hour), ':');
    log.printf(DEC_W2P0(t->tm_min),  ':');
    log.printf(DEC_W2P0(t->tm_sec));

    static const char levels[] = "|DEBUG|th=|INFO |th=|WARN |th=|ERROR|th=|FATAL|th=|TEMP |th=|AUDIT|th=";
    log.reserved = pro.level;
    log.printf(ALogString(&levels[pro.level * 10], 10));
    log.printf(photon::CURRENT, '|');
    if (pro.level != ALOG_AUDIT) {
        log.printf(ALogString((char*)pro.addr_file, pro.len_file), ':');
        log.printf(pro.line, '|');
        log.printf(ALogString((char*)pro.addr_func, pro.len_func), ':');
    }
    return log;
}
