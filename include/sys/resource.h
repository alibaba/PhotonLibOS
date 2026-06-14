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
#ifdef _WIN32
#include <sys/time.h>
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
};
#define RUSAGE_SELF 0
inline int getrusage(int, struct rusage*) { return -1; }

struct rlimit {
    unsigned long long rlim_cur;
    unsigned long long rlim_max;
};
#define RLIMIT_NOFILE 7
inline int getrlimit(int, struct rlimit*) { return -1; }
inline int setrlimit(int, const struct rlimit*) { return -1; }
#else
#include_next <sys/resource.h>
#endif
