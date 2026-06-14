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
// statfs not available on Windows; provide stub struct
#include <cstdint>
struct statfs {
    long    f_type;
    long    f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    long    f_namelen;
};
inline int statfs(const char*, struct statfs*) { return -1; }
inline int fstatfs(int, struct statfs*)           { return -1; }
#elif defined(__APPLE__)
#include <sys/mount.h>
#else
#include_next <sys/vfs.h>
#endif
