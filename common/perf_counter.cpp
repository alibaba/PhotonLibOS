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

#include "perf_counter.h"
#include <assert.h>
#include <fstream>
#include <thread>
#include <sys/time.h>
#include "string_view.h"
#include "alog.h"
using namespace std;

static ofstream* of = nullptr;
static __perf_counter* pc_begin = nullptr;

static void __register_perf_counter(__perf_counter* pc)
{
    pc->_next = pc_begin;
    pc_begin = pc;
}

template<typename T>
static void output(const char* date, string_view name, T value)
{
    (*of) << date << " " << name.data() << " : " << value << std::endl;
}

static uint64_t current_time(char cur[], int length)
{
    struct timeval t;
    gettimeofday(&t, nullptr);
    struct tm tim;
    ::localtime_r(&t.tv_sec, &tim);
    snprintf(cur, length, "%04d-%02d-%02d %02d:%02d:%02d",
        ((unsigned int)tim.tm_year + 1900) % 10000,
        ((unsigned int)tim.tm_mon + 1) % 100,
        (unsigned int)tim.tm_mday % 100,
        (unsigned int)tim.tm_hour % 100,
        (unsigned int)tim.tm_min % 100,
        (unsigned int)tim.tm_sec % 100);
    return ((uint64_t)t.tv_sec*1000 + t.tv_usec/1000);
}

static void log_perf_counters()
{
    this_thread::sleep_for(std::chrono::seconds(1));
    char date[32];
    uint64_t current = current_time(date, sizeof(date));
    for (auto pc = pc_begin; pc; pc = pc->_next)
    {
        if (current < pc->_last + pc->_interval * 1000)
            continue;

        pc->_last = current;
        string_view name(pc->_name, pc->_name_length);
        if (pc->_type == TOTAL) {
            uint64_t v = pc->_value.load(std::memory_order_relaxed);
            output(date, name, v);
        } else if (pc->_type == ACCUMULATE) {
            uint64_t v = pc->_value.exchange(0, std::memory_order_relaxed);
            output(date, name, v);
        } else if (pc->_type == AVERAGE) {
            uint64_t v = pc->_value.exchange(0, std::memory_order_relaxed);
            uint64_t sum = v >> 24;
            uint64_t count = v & 0xffffffULL;
            double average = (count ? double(sum) / count : 0);
            output(date, name, average);
        } else assert(false);
    }
}

void init_perf_counter(__perf_counter* pc)
{
    __register_perf_counter(pc);
    static bool inited = false;
    if (inited) {
        return;
    }

    inited = true;
    const static char perfLog[] = "./perf.log";
    of = new ofstream(perfLog, std::ios_base::out|std::ios_base::app);
    if (!of->is_open()) {
        LOG_ERROR("failed to open log file ` , errno:`", perfLog, errno);
        delete of;
        of = nullptr;
        return;
    }

    std::thread([&]{
        for (;;) log_perf_counters();
    }).detach();
}

REGISTER_PERF(__example_pc__, TOTAL)
inline int example_pc()
{
    REPORT_PERF(__example_pc__, 1)
    return 0;
}
