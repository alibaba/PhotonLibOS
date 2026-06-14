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
// mmap / munmap / mprotect / madvise implemented via Windows VirtualAlloc API

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED   0x1
#define MAP_PRIVATE  0x2
#define MAP_FIXED    0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED   nullptr

#define MADV_DONTNEED 4
#define MADV_NOHUGEPAGE 15

// Implemented in common/win32_compat.cpp
void* mmap(void* addr, size_t length, int prot, int flags, int fd, long long offset);
int   munmap(void* addr, size_t length);
int   mprotect(void* addr, size_t length, int prot);
int   madvise(void* addr, size_t length, int advice);

inline int shm_open(const char*, int, mode_t) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return -1;
}
inline int shm_unlink(const char*) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return -1;
}
#else
#include_next <sys/mman.h>
#endif
