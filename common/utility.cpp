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

#include <sys/utsname.h>
#include <execinfo.h>
#include <random>
#include "utility.h"
#include "estring.h"
#include "alog.h"

using namespace std;

int version_compare(string_view a, string_view b, int& result) {
    auto sa = ((estring_view&)a).split('.');
    auto sb = ((estring_view&)b).split('.');

    for (auto ita = sa.begin(), itb = sb.begin();
            ita != sa.end() && itb != sb.end();
            ++ita, ++itb) {
        if (!ita->all_digits() || !itb->all_digits()) {
            return -1;
        }
        if (ita->size() == itb->size()) {
            result = strncmp(ita->data(), itb->data(), ita->size());
            if (result != 0) {
                return 0;
            }
        } else {
            result = (int) (ita->size() - itb->size());
            return 0;
        }
    }
    result = (int) (a.size() - b.size());
    return 0;
}

int kernel_version_compare(std::string_view dst, int& result) {
    utsname buf = {};
    uname(&buf);
    estring kernel_release(buf.release);
    estring_view kernel_version = kernel_release.split("-")[0];
    int ret = version_compare(kernel_version, dst, result);
    if (ret != 0) {
        LOG_ERRNO_RETURN(0, -1, "Unable to detect kernel version, `", kernel_release.c_str());
    }
    return 0;
}

void print_stacktrace() {
    int size = 16;
    void * array[16];
    int stack_num = backtrace(array, size);
    char ** stacktrace = backtrace_symbols(array, stack_num);
    for (int i = 0; i < stack_num; ++i)
    {
        LOG_DEBUG(stacktrace[i]);
    }
    free(stacktrace);
}

namespace photon {

static std::random_device rd;
thread_local std::mt19937 gen32(rd());
thread_local std::mt19937_64 gen64(rd());

uint32_t rand32() {
    return gen32();
}

uint32_t rand32_distribution(uint32_t min, uint32_t max) {
    std::uniform_int_distribution<uint32_t> dist(min, max);
    return dist(gen32);
}

uint64_t rand64() {
    return gen64();
}

uint64_t rand64_distribution(uint64_t min, uint64_t max) {
    std::uniform_int_distribution<uint64_t> dist(min, max);
    return dist(gen64);
}

}