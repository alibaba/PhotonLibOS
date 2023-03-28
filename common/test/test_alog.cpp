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

#include "photon/common/alog.h"
#include <gtest/gtest.h>
#include "photon/thread/thread.h"
#include <chrono>
#include <vector>

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

    auto start = std::chrono::steady_clock::now();
    // loop for 4.1 secs
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(4100)) {
        // print only 3 logs  every 1 sec
        LOG_FIRST_N_EVERY_T(3, 1, LOG_INFO("LOG 3 logs every 1 second ...", x++));
    }
    // suppose to print and evaluate 15 times
    EXPECT_EQ(15, x);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    int ret = RUN_ALL_TESTS();
    LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
