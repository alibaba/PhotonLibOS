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
// Pull in the real MinGW <fcntl.h> first, then add stubs below.  Use
// #include_next so the search skips this shim and continues past it to the
// toolchain's copy (MinGW's fcntl.h is in the system include path, after
// -I include/).
#include_next <fcntl.h>
#include <io.h>

// Windows/MinGW lacks F_GETFL/F_SETFL/O_NONBLOCK.
// Actual non-blocking setup for sockets uses ioctlsocket(FIONBIO), but the
// constants are referenced by generic code paths (set_fd_nonblocking, etc.)
// so we need them to exist.
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif

// Windows/MinGW lacks posix_fadvise() and its POSIX_FADV_* constants.  The
// fadvise() override on Windows is a no-op, so any integer value is safe;
// using 0 for all of them keeps the shim minimal and call sites unchanged.
#ifndef POSIX_FADV_NORMAL
#define POSIX_FADV_NORMAL     0
#endif
#ifndef POSIX_FADV_RANDOM
#define POSIX_FADV_RANDOM     0
#endif
#ifndef POSIX_FADV_SEQUENTIAL
#define POSIX_FADV_SEQUENTIAL 0
#endif
#ifndef POSIX_FADV_WILLNEED
#define POSIX_FADV_WILLNEED   0
#endif
#ifndef POSIX_FADV_DONTNEED
#define POSIX_FADV_DONTNEED   0
#endif
#ifndef POSIX_FADV_NOREUSE
#define POSIX_FADV_NOREUSE    0
#endif

#else
// Non-Windows: forward to the real libc <fcntl.h>.
#include_next <fcntl.h>
#endif
