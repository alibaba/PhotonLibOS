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

#include <cstddef>
#include <cstdio>

struct iovec {
    void*  iov_base;
    size_t iov_len;
};

typedef long long ssize_t;

// Implemented in common/win32_compat.cpp
ssize_t writev(int fd, const struct iovec* iov, int iovcnt);
ssize_t readv(int fd, const struct iovec* iov, int iovcnt);

#else
#include_next <sys/uio.h>
#endif
