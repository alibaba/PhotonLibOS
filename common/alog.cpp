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

#ifdef _WIN64
#define _POSIX_C_SOURCE 1
#endif
#include "alog.h"
#include "lockfree_queue.h"
#include "photon/thread/thread.h"
#include "photon/io/fd-events.h"
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
#include <photon/common/iovector.h>
#include <sys/uio.h>
#include <vector>
using namespace std;

static uint32_t now0;
static struct tm alog_time = {0};

class BaseLogOutput : public ILogOutput {
public:
    uint64_t throttle = -1UL;
    uint64_t count = 0;
    uint32_t ts = 0;
    int log_file_fd;
    constexpr BaseLogOutput(int fd = 0) : log_file_fd(fd) { }
    unsigned char level_color[ALOG_AUDIT + 1] = { ALOG_COLOR_DARKGRAY,
           ALOG_COLOR_NOTHING, ALOG_COLOR_YELLOW, ALOG_COLOR_RED,
           ALOG_COLOR_MAGENTA, ALOG_COLOR_CYAN,   ALOG_COLOR_GREEN};
    void clear_color() { memset(level_color, 0, sizeof(level_color)); }
    uint16_t decode(uint16_t c) {
        c = ((c & 0xf) << 8) | (c >> 4);
        return c + *(uint16_t*)"00";
    }
    uint16_t get_color(unsigned int level) {
        if (level < LEN(level_color))
            if (auto c = level_color[level])
                return decode(c);
        return 0;
    }
    int set_level_color(int level, unsigned char code) override {
        if ((uint32_t)level > ALOG_AUDIT)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid level ", level);
        auto dx = decode(code);
        if ((dx < decode(ALOG_COLOR_BLACK) &&
             dx > decode(ALOG_COLOR_LIGHTGRAY)) /* not in 3x range */
            && (dx < decode(ALOG_COLOR_DARKGRAY) &&
                dx > decode(ALOG_COLOR_LIGHTWHITE)) /* not in 9x range */
            && (dx != decode(ALOG_COLOR_NOTHING))) {
            LOG_ERROR_RETURN(EINVAL, -1, "invalid color code ", HEX(code));
        }
        level_color[level] = code;
        return 0;
    }

    struct LineIOV {
        struct iovec iov[3];
        size_t total_length;
        uint8_t iovst, iovcnt;
        char color_prefix[6] = "\033[00m";
        LineIOV(uint16_t color, const char* begin, const char* end) {
            total_length = end - begin;
            iov[1] = {(void*)begin, total_length};
            if (!color) {
                iovst = iovcnt = 1;
            } else {
                *(uint16_t*)&color_prefix[2] = color;
                iov[0] = {color_prefix, LEN(color_prefix) - 1};
                constexpr static char color_suffix[] = "\033[0m";
                iov[2] = {(void*)color_suffix, LEN(color_suffix) - 1};
                total_length += iov[0].iov_len + iov[2].iov_len;
                iovst = 0; iovcnt = 3;
            }
        }
        iovec* start() { return iov + iovst; }
        int count() { return iovcnt; }
    };
    void write(int level, const char* begin, const char* end) override {
        LineIOV iov(get_color(level), begin, end);
        std::ignore = ::writev(log_file_fd, iov.start(), iov.count());
        throttle_block();
    }
    void throttle_block() {
        if (throttle == -1UL) return;
        if (ts != now0) { ts = now0; count = 0; }
        if (++count > throttle) { ::sleep(1); }
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

#ifndef DEFAULT_LOG_LEVEL
#define DEFAULT_LOG_LEVEL ALOG_DEBUG
#endif
ALogLogger default_logger {log_output_stdout, DEFAULT_LOG_LEVEL};
ALogLogger default_audit_logger {log_output_null, ALOG_AUDIT};

uint32_t& log_output_level = default_logger.log_level;
ILogOutput* &log_output = default_logger.log_output;

void LogFormatter::put(ALogBuffer& buf, FP x)
{
    char _fmt[64];
    ALogBuffer fmt {_fmt, sizeof(_fmt), 0};
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
    if (!x.is_signed() || x.svalue() >= 0)
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

__attribute__((constructor)) static void __initial_timezone() { tzset(); }
static struct tm* alog_update_time(uint32_t now0) {
    static uint32_t dayid = 0, minuteid = 0;
    static time_t tsdelta = 0;
    ::now0 = now0;

    auto now = now0 + tsdelta;
    int sec = now % 60;    now /= 60;
    if (unlikely(now != minuteid)) {    // calibrate wall time every minute
        now = time(0) - timezone;
        tsdelta = now - now0;
        sec = now % 60; now /= 60;
        minuteid = now;
    }
    int min = now % 60;    now /= 60;
    int hor = now % 24;    now /= 24;
    if (now != dayid) {
        dayid = now;
        auto now_ = now0 + tsdelta;
        gmtime_r(&now_, &alog_time);
        alog_time.tm_year+=1900;
        alog_time.tm_mon++;
    } else {
        alog_time.tm_sec = sec;
        alog_time.tm_min = min;
        alog_time.tm_hour = hor;
    }
    return &alog_time;
}

class LogOutputFile final : public BaseLogOutput {
public:
    uint64_t log_file_size_limit = 0;
    char* log_file_name = nullptr;
    atomic<uint64_t> log_file_size{0};
    unsigned int log_file_max_cnt = 10;

    LogOutputFile() {
        // no colors by default when log into files
        BaseLogOutput::clear_color();
    }

    virtual void destruct() override {
        log_output_file_close();
        delete this;
    }

    int fopen(const char* fn) {
        auto mode   =   S_IRUSR | S_IWUSR  | S_IRGRP | S_IROTH;
        return open(fn, O_CREAT | O_WRONLY | O_APPEND, mode);
    }

    void write(int level, const char* begin, const char* end) override {
        if (log_file_fd < 0) return;
        uint64_t length = end - begin;
        // iovec iov{(void*)begin, length};
        BaseLogOutput::write(level, begin, end);
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
            std::ignore = ::write(log_file_fd, msg, sizeof(msg) - 1);
            if (log_file_name)
                std::ignore = ::write(log_file_fd, log_file_name, strlen(log_file_name));
            std::ignore = ::write(log_file_fd, "\n", 1);
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

static const uint64_t SPSC_CAPACITY     = 1024 * 1024UL;
static const int      MIN_NUM_OF_QUEUES = 1;
static const int      MAX_NUM_OF_QUEUES = 128;
static const uint32_t MAX_YIELD_TURNS   = 1024;

class AsyncLogOutput final : public BaseLogOutput {
public:
    ILogOutput* log_output;
    photon::semaphore sem;
    std::thread background;
    typedef LockfreeSPSCRingQueue<char, SPSC_CAPACITY> spsc;
    std::vector<std::unique_ptr<spsc>> buf;
    std::vector<std::unique_ptr<photon::spinlock>> lock;
    int num_of_queues;
    bool stopped = false;

    AsyncLogOutput(ILogOutput* output, int num) : log_output(output), num_of_queues(num) {
        // no colors by default when log into files
        BaseLogOutput::clear_color();
        if (num_of_queues < MIN_NUM_OF_QUEUES) num_of_queues = MIN_NUM_OF_QUEUES;
        if (num_of_queues > MAX_NUM_OF_QUEUES) num_of_queues = MAX_NUM_OF_QUEUES;
        num_of_queues = 1 << (31 - __builtin_clz(num_of_queues));  // Aligned to 2^n
        buf.reserve(num_of_queues);
        lock.reserve(num_of_queues);
        for (int i = 0; i < num_of_queues; ++i) {
            buf.emplace_back(new spsc());
            lock.emplace_back(new photon::spinlock());
        }
        background = std::thread(&AsyncLogOutput::worker, this);
    }

    void worker() {
        photon::vcpu_init();
        photon::fd_events_init(photon::INIT_EVENT_EPOLL);
        uint32_t yield_turn = 0;
        while (!stopped) {
            if (writeback() == 0) {
                if (yield_turn < MAX_YIELD_TURNS) {
                    photon::thread_yield();
                    ++yield_turn;
                } else {
                    // wait for 100ms
                    sem.wait(1, 100UL * 1000);
                }
            } else {
                yield_turn = 0;
            }
        }
        photon::fd_events_fini();
        photon::vcpu_fini();
        (void)writeback();
    }

    uint64_t writeback() {
        uint64_t cc = 0;
        for (int i = 0; i < num_of_queues; ++i) {
            cc += buf[i]->consume_pop_batch(UINT32_MAX, [&](const char* p1, size_t n1,
                                                            const char* p2, size_t n2) {
                // no level and coloring again, by passing -1
                log_output->write(-1, p1, p1 + n1);
                if (n2) log_output->write(-1, p2, p2 + n2);
            });
        }
        return cc;
    }

    void write(int level, const char* begin, const char* end) override {
        static thread_local uint64_t index = 0;
        auto current = (++index) & (num_of_queues - 1);
        size_t ra;
        LineIOV iov(get_color(level), begin, end); {
            SCOPED_LOCK(lock[current].get());
            ra = buf[current]->read_available();
            (void)buf[current]->produce_push_batch_fully(iov.total_length,
                [&](char* p1, size_t n1, char* p2, size_t n2) {
                    iovec d[2] = {{p1, n1}, {p2, n2}};
                    iovector_view dest(d, 2), src(iov.start(), iov.count());
                    dest.memcpy_from(&src, iov.total_length);
                });
        }
        if (ra + iov.total_length > spsc::SLOTS_NUM / 2 && ra <= spsc::SLOTS_NUM / 2) { sem.signal(1); }
    }
    virtual int get_log_file_fd() override { return log_output->get_log_file_fd(); }
    virtual uint64_t set_throttle(uint64_t t = -1UL) override { return log_output->set_throttle(t); }
    virtual uint64_t get_throttle() override { return log_output->get_throttle(); }
    virtual void destruct() override {
        if (!stopped) {
            stopped = true;
            sem.signal(1);
            if (background.joinable()) background.join();
        }
        delete this;
    }
};

ILogOutput* new_log_output_file(const char* fn, uint64_t rotate_limit,
                                int max_log_files, uint64_t throttle, bool rotate_on_start) {
    auto ret = new LogOutputFile();
    if (ret->log_output_file_setting(fn, rotate_limit, max_log_files) < 0) {
        delete ret;
        LOG_ERROR_RETURN(0, nullptr, "Failed to open log file ", fn);
        return nullptr;
    }
    ret->set_throttle(throttle);

    // when init the new log output file, rotate the log files that last program created
    if (rotate_on_start && ret->log_file_size != 0) {
        ret->log_file_rotate();
        ret->reopen_log_output_file();
    }

    return ret;
}

ILogOutput* new_log_output_file(int fd, uint64_t throttle) {
    auto ret = new LogOutputFile();
    ret->log_output_file_setting(fd);
    ret->set_throttle(throttle);
    return ret;
}

ILogOutput* new_async_log_output(ILogOutput* output, int num_of_queues) {
    return output ? new AsyncLogOutput(output, num_of_queues) : nullptr;
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

LogBuffer& operator << (LogBuffer& log, const Prologue& pro)
{
#ifdef LOG_BENCHMARK
    auto t = &alog_time;
#else
    auto ts = photon::__update_now();
    auto t = alog_update_time(ts.sec());
#endif
#define DEC_W2P0(x) DEC(x).width(2).padding('0')
    log.printf(t->tm_year, '/');
    log.printf(DEC_W2P0(t->tm_mon),  '/');
    log.printf(DEC_W2P0(t->tm_mday), ' ');
    log.printf(DEC_W2P0(t->tm_hour), ':');
    log.printf(DEC_W2P0(t->tm_min),  ':');
    log.printf(DEC_W2P0(t->tm_sec), '.');
    log.printf(DEC(ts.usec()).width(6).padding('0'));

    static const char levels[] = "|DEBUG|th=|INFO |th=|WARN |th=|ERROR|th=|FATAL|th=|TEMP |th=|AUDIT|th=";
    log.level = pro.level;
    log.printf(ALogString(&levels[pro.level * 10], 10));
    log.printf(photon::CURRENT, '|');
    if (pro.level != ALOG_AUDIT) {
        log.printf(ALogString(pro.addr_file, pro.len_file), ':');
        log.printf(pro.line, '|');
        log.printf(ALogString(pro.addr_func, pro.len_func), ':');
    }
    return log;
}

LogBuffer& operator << (LogBuffer& log, ERRNO e) {
    auto no = e.no ? e.no : errno;
    return log.printf("errno=", no, '(', strerror(no), ')');
}
