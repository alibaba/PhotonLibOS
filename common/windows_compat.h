#pragma once
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN

// Must be defined before any stdlib.h includes
#define _CRT_RAND_S

#include <sys/uio.h>

#include <cstdint>

// POSIX compatibility definitions for Windows/MinGW

// struct iovec, writev, readv are now defined in include/sys/uio.h

// posix_memalign
#include <malloc.h>
inline int posix_memalign(void** memptr, size_t alignment, size_t size) {
    *memptr = _aligned_malloc(size, alignment);
    return *memptr ? 0 : ENOMEM;
}

// malloc_usable_size fallback
inline size_t malloc_usable_size(void*) { return 0; }

// ESHUTDOWN
#ifndef ESHUTDOWN
#define ESHUTDOWN 108
#endif

// POSIX types not defined on Windows
#include <sys/types.h>
#ifndef uid_t
typedef int uid_t;
#endif
#ifndef gid_t
typedef int gid_t;
#endif

// timezone and gmtime_r compatibility
#include <ctime>
#define timezone _timezone
#define gmtime_r(t, r) gmtime_s(r, t)

// rand_s for random numbers
#include <stdlib.h>
#include <string.h>

// fcntl flags not available on MinGW
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
inline int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }

// S_ISSOCK not available on Windows
#ifndef S_ISSOCK
#define S_ISSOCK(mode) 0
#endif

// getpagesize
#include <windows.h>
inline int getpagesize() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwPageSize;
}

// usleep via NtDelayExecution (100ns precision)
int usleep(uint64_t usec);

// strndup fallback
inline char* strndup(const char* s, size_t n) {
    size_t len = strnlen(s, n);
    char* p = (char*)malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = 0; }
    return p;
}

// Signal set types and functions (MinGW lacks full POSIX signal support)
#include <signal.h>
typedef _sigset_t sigset_t;
inline int sigemptyset(sigset_t*) { return 0; }
inline int sigfillset(sigset_t*)  { return 0; }
inline int sigaddset(sigset_t*, int)  { return 0; }
inline int sigdelset(sigset_t*, int)  { return 0; }
inline int sigismember(const sigset_t*, int) { return 0; }
inline int sigprocmask(int, const sigset_t*, sigset_t*) { return 0; }
#define SFD_CLOEXEC   0
#define SFD_NONBLOCK  0

// sigaction and related types (not available on MinGW)
typedef void (*sighandler_t)(int);
struct sigaction {
    union {
        sighandler_t sa_handler;
        void      (*sa_sigaction)(int, void*, void*);
    };
    sigset_t sa_mask;
    int      sa_flags;
};
#define SA_RESTART  0
#define SA_SIGINFO  0

#ifndef SIGTSTP
#define SIGTSTP 20
#endif

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 0
#endif

#define bzero(b, len) memset((b), 0, (len))

#ifndef makedev
#define makedev(major, minor) (((major) << 8) | (minor))
#endif

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int si_pid;
    int si_uid;
    int si_status;
    int si_utime;
    int si_stime;
    int si_int;
    void* si_ptr;
    int si_overrun;
    void* si_addr;
    int si_band;
    int si_fd;
} siginfo_t;

// strptime (POSIX, not available on Windows)
#include <ctime>
inline char* strptime(const char* s, const char* fmt, struct tm* tm) {
    (void)s; (void)fmt; (void)tm;
    return nullptr;
}

#endif // _WIN32
