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
#include <cstdint>

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    uint64_t     f_blocks;
    uint64_t     f_bfree;
    uint64_t     f_bavail;
    uint64_t     f_files;
    uint64_t     f_ffree;
    uint64_t     f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

// Implemented in common/win32_compat.cpp
int statvfs(const char* path, struct statvfs* buf);
inline int fstatvfs(int, struct statvfs*) { return -1; }
#else
#include_next <sys/statvfs.h>
#endif
