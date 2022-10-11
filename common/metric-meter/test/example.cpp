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

#include <sys/fcntl.h>
#include <time.h>

#include <cstdio>
#include <cstring>
#include <random>

#include <photon/thread/thread.h>
#include "../metrics.h"
#include <photon/thread/timer.h>

class Table {
public:
    Metric::IntervalMaxCounter maxv;
    Metric::AverageLatencyCounter pread_latency;
    Metric::MaxLatencyCounter pread_max;
} table;

constexpr char WHITE[] = "\033[1;37m";
constexpr char NC[] = "\033[0m";
constexpr char CLEARLN[] = "\033[2K\r";

uint64_t print(void*) {
    char buffer[4096];
    auto len =
        snprintf(buffer, 4096, "%s%12ld%12ld%12ld", CLEARLN, table.maxv.val(),
                 table.pread_latency.val(), table.pread_max.val());
    ::write(1, buffer, len);
    return 0;
}

int main() {
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    photon::Timer printer(1UL * 100 * 1000, {print, nullptr});
    table.maxv.interval(1000UL * 1000);
    table.pread_latency.interval(1000UL * 1000);
    table.pread_max.interval(1000UL * 1000);
    printf("%s%12s %12s %12s%s\n", WHITE, "maxv", "pread_latency", "pread_max",
           NC);
    for (int i = 0; i < 5000; i++) {
        SCOPE_LATENCY(table.pread_latency);
        SCOPE_LATENCY(table.pread_max);
        table.maxv.put(photon::now % 10000000 / 1000000);
        photon::thread_usleep(1000 + rand() % 1000);
    }
    photon::thread_usleep(5000 * 1000);
}
