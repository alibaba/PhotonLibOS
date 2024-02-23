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

#include "../alog.h"
#include "../alog-stdstring.h"
#include "../alog-functionptr.h"
#include "../alog-audit.h"
#include <gtest/gtest.h>
#include <photon/thread/thread.h>
#include <photon/net/socket.h>
#include <photon/net/utils-stdstring.h>
#include <chrono>
#include <vector>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

class LogOutputTest : public ILogOutput {
public:
    size_t _log_len;
    char _log_buf[4096];
    void write(int, const char* begin, const char* end) override
    {
        _log_len = end - begin;
        EXPECT_TRUE(_log_len < sizeof(_log_buf));
        memcpy(_log_buf, begin, _log_len);
        _log_buf[ --_log_len ] = '\0';
    }
    const char* log_start() const {
        auto ls = _log_buf;
        for (int i = 0; i < 4; i++)
            ls = strchr(ls, '|') + 1;
        ls = strchr(ls, ':') + 1;
        return ls;
    }
    int get_log_file_fd() override {
        return -1;
    }

    uint64_t get_throttle() override {
        return -1UL;
    }

    uint64_t set_throttle(uint64_t) override {
        return -1UL;
    }

    void destruct() override {}
} log_output_test;

auto &_log_buf=log_output_test._log_buf;
auto &_log_len=log_output_test._log_len;

class Recall
{
public:
    int gc_recall(int code)
    {
        LOG_INFO(code);
        return 0;
    }
};

int gc_recall(void* obj, int code)
{
    LOG_INFO(code);
    return 0;
}

TEST(alog, example) {
    log_output = &log_output_test;
    DEFER(log_output = log_output_stdout);
    LOG_INFO("WTF"
    " is `"
    " this `", "exactly", "stuff");
    EXPECT_STREQ("WTF\" \" is exactly\" \" this stuff", log_output_test.log_start());

    LOG_INFO("WTF is ` this `", 1, 2);
    EXPECT_STREQ("WTF is 1 this 2", log_output_test.log_start());
    LOG_INFO("WTF is \n means ` may be work `", "??", "!!");
    EXPECT_STREQ("WTF is \\n means ?? may be work !!", log_output_test.log_start());
    LOG_INFO("WTF is \n means ` may be work `", "??", "!!");
    EXPECT_STREQ("WTF is \\n means ?? may be work !!", log_output_test.log_start());
    const char foobar[] = "n";
    LOG_INFO(foobar);
    // #S len == 6 > 2 + sizeof(yeah) 4, will fit the length condition
    // but miss the leading/tailing character condition;
    EXPECT_STREQ("n", log_output_test.log_start());
    LOG_INFO(VALUE(foobar));
    EXPECT_STREQ("[foobar=n]", log_output_test.log_start());
}

struct BeforeAndAfter {
    BeforeAndAfter() {
        LOG_INFO("Before global");
    }

    ~BeforeAndAfter() {
        LOG_INFO("After global");
    }
} baa;

using namespace std;

TEST(ALog, DEC) {
    log_output = &log_output_test;
    DEFER(log_output = log_output_stdout);
    // LOG_DEBUG(' ');
    // auto log_start = _log_buf + _log_len - 1;
    LOG_DEBUG(DEC(16));
    puts(_log_buf);
    EXPECT_EQ(string("16"), log_output_test.log_start());
    LOG_DEBUG(HEX(16));
    puts(_log_buf);
    EXPECT_EQ(string("10"), log_output_test.log_start());
}

TEST(ALog, DoubleLogger) {
    LogOutputTest lo2;
    ALogLogger l2{0, &lo2};
    log_output = &log_output_test;
    DEFER(log_output = log_output_stdout);
    // LOG_DEBUG(' ');
    // l2 << LOG_DEBUG(' ');
    // auto ls2 = lo2._log_buf + lo2._log_len - 1;
    // auto log_start = _log_buf + _log_len - 1;
    LOG_DEBUG(DEC(16));
    puts(_log_buf);
    EXPECT_EQ(string("16"), log_output_test.log_start());
    l2 << LOG_DEBUG(DEC(32));
    EXPECT_EQ(string("32"), lo2.log_start());
    puts(lo2._log_buf);
    LOG_DEBUG(HEX(16));
    l2 << LOG_DEBUG(HEX(32));
    puts(_log_buf);
    puts(lo2._log_buf);
    EXPECT_EQ(string("10"), log_output_test.log_start());
    EXPECT_EQ(string("20"), lo2.log_start());
}

TEST(ALog, log_to_file) {
    // create empty file
    int fd = ::open("/tmp/logfile", O_RDWR | O_TRUNC | O_CREAT, 0666);
    ::close(fd);
    // set global file
    auto ret = log_output_file("/tmp/logfile");
    EXPECT_EQ(0, ret);
    // if trying create another abnormal log file then
    ret = log_output_file("/");
    EXPECT_EQ(-1, ret);
    const auto HELLO = "Hello log to file";
    LOG_DEBUG(HELLO);// this should put into file
    ret = log_output_file_close();
    EXPECT_EQ(0, ret);
    char buffer[8192];
    fd = ::open("/tmp/logfile", O_RDONLY);
    int length = ::read(fd, &buffer, sizeof(buffer));
    EXPECT_GT(length, 0);
    // compare, buffer will followed tailing enter in the end of line
    EXPECT_EQ(0, strncmp(HELLO, &buffer[length - strlen(HELLO) - 1], strlen(HELLO)));
    ::close(fd);
}

TEST(ALog, float_point)
{
    log_output = &log_output_test;
    LOG_DEBUG(' ');
    // auto log_start = _log_buf + _log_len - 1;
    auto fp = 5203.14159265352L;
    LOG_DEBUG(FP(fp).width(10).precision(3));
    puts(_log_buf);
    EXPECT_EQ(log_output_test.log_start(), string("  5203.142"));
    LOG_DEBUG(FP(fp).width(8).precision(3).scientific(true));
    puts(_log_buf);
    EXPECT_EQ(log_output_test.log_start(), string("5.203e+03"));
    LOG_DEBUG(FP(fp).precision(3).scientific(false));
    puts(_log_buf);
    EXPECT_EQ(log_output_test.log_start(), string("5203.142"));
    log_output = log_output_stdout;
}

void log_format()
{
    LOG_DEBUG("aksdjfj `:` ` ` ` ` `", 234, "^%$#@", 341234, "  hahah `:jksld",
              884, HEX(2345678), "::::::::::::::::::::::::::::::::::::::::::::::::::::");
}

TEST(ALog, fmt_perf_1m)
{
    log_output = log_output_null;
    for (int i=0; i<1000*1000; i++)
        log_format();

    log_output = log_output_stdout;
    log_format();
}

void log_print_()
{
    LOG_DEBUG("aksdjfj ", 234, ':', "^%$#@", ' ', 341234, ' ', "  hahah `:jksld", ' ',
              884, ' ', HEX(2345678), ' ', "::::::::::::::::::::::::::::::::::::::::::::::::::::");
}

TEST(ALog, print_perf_1m)
{
    log_output = log_output_null;
    for (int i=0; i<1000*1000; i++)
        log_print_();

    log_output = log_output_stdout;
    log_print_();
}

int DevNull(void* x, int);
TEST(ALog, snprintf_perf_1m)
{
    char buf[1024];
    static char levels[][6] = {"DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
    static int th = 2423423;
    for (int i=0; i<1000*1000; i++)
    {
        snprintf(buf, sizeof(buf), "%d/%02d/%02d %02d:%02d:%02d|%s|th=%016X|%s:%d|%s:aksdjfj %d:%s%d  hahah `:jksld%d%X%s",
                 9102,03,04, 05,06,78, levels[0], th, __FILE__, __LINE__, __func__,
                 234, "^%$#@", i, 884, 2345678, "::::::::::::::::::::::::::::::::::::::::::::::::::::");
        DevNull(buf, i);
    }
    puts(buf);
}


char str[] = "2018/01/05 21:53:28|DEBUG| 2423423|test.cpp:254|virtual void LOGPerf_1M_memcpy_Test::TestBody():aksdjfj 234:^%$#@341234  hahah `:jksld88423CACE::::::::::::::::::::::::::::::::::::::::::::::::::::";

class foobarasdf
{
public:
    int x = 0x12345678;
    int event1(ssize_t code)
    {
        LOG_FATAL("OK with code=`, x=0x`", code, HEX(x));
        return 0;
    }
    int event2(void* ptr)
    {
        LOG_FATAL("OK with ptr=`, x=0x`", ptr, HEX(x));
        return 0;
    }
    double asdf() const { return 0; }
};

LogBuffer& operator << (LogBuffer& log, const foobarasdf& fb)
{
    return log << "(0x" << HEX(fb.x) << ')';
}

void* test_LOG_ERROR_RETURN()
{
    LOG_ERROR_RETURN(EBUSY, nullptr, "This is a test.");
}

int test_LOG_ERRNO_RETURN()
{
    errno = ENOSYS;
    LOG_ERRNO_RETURN(EMFILE, -1, "This is a test.");
}

__attribute__((noinline))
void test_log(int x)
{
    const char* xs = " a char* string! ";
    auto vxs = VALUE(xs);
    LOG_DEBUG(234, "laskdjf", vxs);//VALUE(xs));
//    EXPECT_EQ(log_start, string("234laskdjf[xs=\"\"]"));
//    puts(_log_buf);
//    LOG_DEBUG("asdf:`, jkl:`, and: ", 1,2,3,4,5);
}

__attribute__((noinline))
void test_log2(int x)
{
    LOG_DEBUG(ALogStringL("asdf:"), 1, ", jkl:", 2, ", and: ", 3,4,5);
}

/*
TEST(ALog, static_parser)
{
#define TEST_PARSER(str, ...) {     \
    PARSE_FMTSTR(str, sequence);    \
    static_assert(is_same<sequence, alog_format::seq< __VA_ARGS__ >>::value, "..."); }

    TEST_PARSER("`", 0);
    TEST_PARSER("`NNN``NN`", 0,-5,8);
    TEST_PARSER("`NNNNN`", 0,6);
    TEST_PARSER("`NN````NNN`", 0,-4,-6,10);
}*/

TEST(ALog, ALog)
{
    test_log(234);
    test_log2(234);
    test_LOG_ERROR_RETURN();
    test_LOG_ERRNO_RETURN();

    log_output = &log_output_test;
    // LOG_DEBUG(' ');
    // auto log_start = _log_buf + _log_len - 1;

    char buf[100];
    memset(buf, '?', sizeof(buf));
    strcpy(buf, "char buf[100]");
    LOG_DEBUG(buf);
    puts(_log_buf);
    EXPECT_TRUE(strcmp(log_output_test.log_start(), buf) == 0);

    LOG_DEBUG("as`df``jkl`as`df``jkl`", 1, 2, 3, 4, 5);
    EXPECT_EQ(log_output_test.log_start(), string("as1df`jkl2as3df`jkl45"));
    puts(_log_buf);

    LOG_DEBUG(2, buf, "asdf");
    EXPECT_EQ(log_output_test.log_start(), string("2") + std::string(buf) + "asdf");
    puts(_log_buf);

    enum { ENUM = 32 };
    LOG_DEBUG("NNNNN`", 1999);
    EXPECT_EQ(log_output_test.log_start(), string("NNNNN1999"));
    puts(_log_buf);

    LOG_DEBUG("Negative: ", -1, foobarasdf(), ERRNO(24));
    EXPECT_EQ(log_output_test.log_start(), string("Negative: -1(0x12345678)errno=24(Too many open files)"));
    puts(_log_buf);

    LOG_DEBUG("My name is `, and my nickname is `.", "Huiba Li", "Lu7", " This is a test of standard formatting.");
    EXPECT_EQ(log_output_test.log_start(), string("My name is Huiba Li, and my nickname is Lu7. This is a test of standard formatting."));
    puts(_log_buf);

    const char* xs = " a char* string! ";
    // auto vxs = VALUE(xs);
    LOG_DEBUG(234, "laskdjf", VALUE(xs));
    EXPECT_EQ(log_output_test.log_start(), string("234laskdjf[xs= a char* string! ]"));
    puts(_log_buf);

    LOG_DEBUG(DEC(298345723731234).comma(true), std::string(" asdf"), xs);
    EXPECT_EQ(log_output_test.log_start(), string("298,345,723,731,234 asdf a char* string! "));
    puts(_log_buf);

    int v = 255;
    LOG_DEBUG("asdf:`", 255);
    EXPECT_EQ(log_output_test.log_start(), string("asdf:255"));
    puts(_log_buf);

    LOG_DEBUG('a', v);
    EXPECT_EQ(log_output_test.log_start(), string("a255"));
    puts(_log_buf);

    LOG_DEBUG(32, "   ",
              DEC(2345678).comma(true).width(10),
              DEC(678).comma(true).width(10),
              DEC(8).comma(true).width(10),
              DEC(5678).comma(true).width(10));
    EXPECT_EQ(log_output_test.log_start(), string("32    2,345,678       678         8     5,678"));
    puts(_log_buf);
}

#define test_type(x, T, len) {          \
    auto xx = alog_forwarding(x);       \
    static_assert(std::is_same<decltype(xx), T>::value, "..."); \
    EXPECT_EQ(xx.size, len); }

TEST(ALog, forwarding)
{
    test_type("as\0df", ALogStringL, 5);

    const char* a = "as\0df";
    test_type(a, ALogStringPChar, 2);

    char* b = (char*)"as\0df";
    test_type(b, ALogStringPChar, 2);

    char c[100] = "as\0df";
    test_type(c, ALogStringPChar, 2);

    const char d[] = "as\0df";
    test_type(d, ALogStringL, 5);
}

void test_defer()
{
    int a = 0;
    DEFER(puts("deferred puts(\"asdf\")"); a++;);
    DEFER(puts("deferred later puts(\"asdf\")"); a++;);
    puts("puts(\"asdf\")");
    printf("a=%d\n", a);
}

TEST(test, test)
{
    char asdf[20];
//  int qwer[LEN(asdf)];
//    vector<int> uio;      // should not compile! to avoid misuse
//    auto len = LEN(uio);

    test_defer();

    foobarasdf fb;
    Callback<void*> done2(&fb, &foobarasdf::event2);
    done2(nullptr);

    auto p = new Recall;
    Callback<int> done(p, &Recall::gc_recall);
    Callback<int> done3(nullptr, &::gc_recall);
    done3.bind(nullptr, &::gc_recall);
    done(12);
    done(13);
}

TEST(ALog, function_pointer)
{
    LOG_DEBUG(&log_output_test);
    LOG_DEBUG(&foobarasdf::event1);
    LOG_DEBUG(&foobarasdf::asdf);
}

struct LevelOutput : public ILogOutput {
    int level;

    void write(int l, const char*, const char*) override {
        level = l;
    }

    int get_log_file_fd() override { return 0; }

    uint64_t set_throttle(uint64_t t = -1UL) override { return 0; }

    uint64_t get_throttle() override { return 0; };

    void destruct() override {}
};

TEST(ALog, level_in_output) {
    auto olo = default_logger.log_output;
    LevelOutput lo;
    default_logger.log_output = &lo;
    default_logger.log_level = ALOG_DEBUG;
    LOG_DEBUG("WTF");
    EXPECT_EQ(ALOG_DEBUG, lo.level);
    LOG_WARN(3.14);
    EXPECT_EQ(ALOG_WARN, lo.level);
    LOG_ERROR(123);
    EXPECT_EQ(ALOG_ERROR, lo.level);
    LOG_FATAL("WTF ` ", 123);
    EXPECT_EQ(ALOG_FATAL, lo.level);
    LOG_DEBUG(" ` ` ", "123");
    EXPECT_EQ(ALOG_DEBUG, lo.level);
    LOG_WARN(VALUE(lo.level));
    EXPECT_EQ(ALOG_WARN, lo.level);
    LOG_ERROR('a');
    EXPECT_EQ(ALOG_ERROR, lo.level);
    default_logger.log_output = olo;
}

TEST(ALog, log_audit) {
    auto olo = default_logger.log_output;
    DEFER(default_logger.log_output = olo);

    LevelOutput lo;
    default_logger.log_output = log_output_null;
    default_logger.log_level = ALOG_DEBUG;
    default_audit_logger.log_output = &lo;
    default_audit_logger.log_level = ALOG_AUDIT;
    DEFER(default_audit_logger.log_output = log_output_null);

    LOG_AUDIT("WTF");
    EXPECT_EQ(ALOG_AUDIT, lo.level);
    default_audit_logger.log_output = log_output_stdout;
    LOG_AUDIT("Hello audit");
    SCOPE_AUDIT("SCOPE for 1 sec");
    photon::thread_usleep(1000 * 1000);
}

void testnull_func() {
    const char *pc = nullptr;
    LOG_DEBUG("try print nullptr const char*", pc);
}

void segfault() {
    volatile char *pc = nullptr;
    *pc = 'w'; //this must trigger sigfault
}

TEST(ALog, null_to_pchar) {
    EXPECT_EXIT((segfault(), exit(0)), ::testing::KilledBySignal(SIGSEGV), ".*");
    EXPECT_EXIT((testnull_func(), exit(0)), testing::ExitedWithCode(0),".*");
}

TEST(ALog, throttled_log) {
    //update time
    photon::thread_yield();
    auto t = time(0);
    set_log_output(log_output_stdout);
    default_logger.log_output->set_throttle(10);
    for (int i=0; i< 30;i++) {
        LOG_INFO("fast logging got throttled ...");
    }
    auto e = time(0);
    EXPECT_GT(e - t, 1);
    default_logger.log_output->set_throttle(-1UL);
}

TEST(ALog, LOG_LIMIT) {
    //update time
    set_log_output(log_output_stdout);
    auto x = 0;
    for (int i=0; i< 1000000;i++) {
        // every 60 secs print only once
        LOG_EVERY_T(60, LOG_INFO("LOG once every 60 second ...", x++));
    }
    // suppose to print and evaluate 1 times
    EXPECT_EQ(1, x);
    x = 0;
    for (int i=0; i< 1000000;i++) {
        // every 100`000 times logs only only once
        LOG_EVERY_N(100000, LOG_INFO("LOG once every 100000 logs ...", x++));
    }
    // suppose to print and evaluate 1`000`000 / 100`000 = 10 times
    EXPECT_EQ(10, x);
    x = 0;
    for (int i=0; i< 1000000;i++) {
        // logs only 10 records.
        LOG_FIRST_N(10, LOG_INFO("LOG first 10 logs ...", x++));
    }
    // suppose to print and evaluate 10 times
    EXPECT_EQ(10, x);
    x = 0;

    auto start = time(0);
    auto last = start;
    // loop for 4.1 secs
    do  {
        // print only 3 logs  every 1 sec
        LOG_FIRST_N_EVERY_T(3, 1, LOG_INFO("LOG 3 logs every 1 second ...", x++));
        last = time(0);
    } while (last - start < 4);
    // suppose to print and evaluate 15 times
    auto suppose = (last - start) * 3;
    EXPECT_GE(x, suppose - 3);
    EXPECT_LE(x, suppose + 3);
}

TEST(ALOG, IPAddr) {
    log_output = &log_output_test;
    DEFER(log_output = log_output_stdout);

    photon::net::IPAddr ip("192.168.12.34");
    EXPECT_STREQ(photon::net::to_string(ip).c_str(), "192.168.12.34");
    LOG_INFO(ip);
    EXPECT_STREQ("192.168.12.34", log_output_test.log_start());

    ip = photon::net::IPAddr("abcd:1111:222:33:4:5:6:7");
    EXPECT_STREQ(photon::net::to_string(ip).c_str(), "abcd:1111:222:33:4:5:6:7");
    LOG_INFO(ip);
    EXPECT_STREQ("abcd:1111:222:33:4:5:6:7", log_output_test.log_start());
}

int main(int argc, char **argv)
{
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    ::testing::InitGoogleTest(&argc, argv);
    int ret = RUN_ALL_TESTS();
    LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
