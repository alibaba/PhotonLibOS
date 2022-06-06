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

#include <execinfo.h>
#include "utility.h"
#include "estring.h"
#include "alog.h"

using namespace std;

namespace Utility {

inline bool all_digits(string_view s)
{
    for (auto c: s)
        if (!isdigit(c))
            return false;
    return true;
}

int version_compare(const std::string& a, const std::string& b, int& result) {
    auto sa = ((estring&) a).split('.');
    auto sb = ((estring&) b).split('.');

    for (auto ita = sa.begin(), itb = sb.begin();
            ita != sa.end() && itb != sb.end();
            ++ita, ++itb) {
        if (!all_digits(*ita) || !all_digits(*itb)) {
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

}
