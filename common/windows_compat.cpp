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

// Windows POSIX-compatibility function implementations.
// These were extracted from include/*.h headers to keep headers clean.
// Only compiled on _WIN32.

#ifdef _WIN32

#include <windows.h>
#include <winioctl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <io.h>
#include <fcntl.h>
#include <malloc.h>
#include <cstring>

#include <sys/socket.h>   // struct msghdr
#include <sys/uio.h>      // struct iovec
#include <sys/statvfs.h>  // struct statvfs
#include <sys/mman.h>     // PROT_*, MAP_*, MADV_* defines

#ifndef WSAENOSYS
#define WSAENOSYS 40
#endif

// ---- windows_compat.h implementations ----

// usleep: POSIX microsecond sleep via NtDelayExecution (100ns precision).
//
// Overflow detection techniques used here:
// 1. (usec | nsec) >> 63 — if either the microsecond value or its ×10
//    100ns equivalent has bit 63 set, the result exceeds INT64_MAX and the
//    NtDelayExecution argument would overflow.  Single bit-test, no division.
// 2. !!(usec % 1000) — ceiling to ms without (usec + 999) which risks
//    UINT64 overflow on its own.  The modulo result is 0..999, ! is 0 iff
//    no remainder, !! is 1 iff there is, avoiding a branch.
// 3. ms > uint64_t((DWORD)-1) — DWORD overflow check for Sleep.  (DWORD)-1
//    is INFINITE (0xFFFFFFFF), the max value Sleep accepts.

int usleep(uint64_t usec) {
    typedef long (NTAPI *NtDelayExecution_t)(BOOL, LARGE_INTEGER*);
    static auto pNtDelay = (NtDelayExecution_t)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDelayExecution");
    if (pNtDelay) {
        LARGE_INTEGER delay;
        auto nsec = usec * 10;
        auto toobig = (usec | nsec) >> 63;
        delay.QuadPart = toobig ? -INT64_MAX  : -(LONGLONG)nsec;
        pNtDelay(FALSE, &delay);
    } else {
        auto ms = (usec / 1000) + !!(usec % 1000);
        auto ms_dw = (ms > uint64_t((DWORD)-1)) ? ((DWORD)-1) : (DWORD)ms;
        Sleep(ms_dw);
    }
    return 0;
}

// ---- signal.h stubs (signal.cpp excluded from Windows build) ----

extern "C" int sync_signal_init()   { return 0; }
extern "C" int block_all_signal()   { return 0; }
extern "C" sighandler_t sync_signal(int, sighandler_t) { return nullptr; }
extern "C" int sync_sigaction(int, const struct sigaction*, struct sigaction*) { return 0; }
extern "C" int sync_signal_fini()   { return 0; }

// ---- easy_weak stubs ----

struct easy_comutex_t { char _[40]; };
extern "C" int v1_easy_comutex_init(easy_comutex_t*) { return -1; }
extern "C" int v1_easy_comutex_cond_wait(easy_comutex_t*) { return -1; }
extern "C" int v1_easy_comutex_cond_timedwait(easy_comutex_t*, int64_t) { return -1; }
extern "C" int v1_easy_comutex_cond_signal(easy_comutex_t*) { return -1; }
extern "C" int v1_easy_comutex_cond_broadcast(easy_comutex_t*) { return -1; }
extern "C" void v1_easy_comutex_lock(easy_comutex_t*) { }
extern "C" void v1_easy_comutex_unlock(easy_comutex_t*) { }

// ---- unistd.h implementations ----

int fsync(int fd) {
    auto h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    return FlushFileBuffers(h) ? 0 : -1;
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    auto h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED ov = {};
    ov.Offset     = (DWORD)offset;
    ov.OffsetHigh = (DWORD)((ULONGLONG)offset >> 32);
    DWORD n = 0;
    if (!ReadFile(h, buf, (DWORD)count, &n, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return 0;
        errno = (e == ERROR_ACCESS_DENIED) ? EACCES : EIO;
        return -1;
    }
    return (ssize_t)n;
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    auto h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED ov = {};
    ov.Offset     = (DWORD)offset;
    ov.OffsetHigh = (DWORD)((ULONGLONG)offset >> 32);
    DWORD n = 0;
    if (!WriteFile(h, buf, (DWORD)count, &n, &ov)) {
        DWORD e = GetLastError();
        errno = (e == ERROR_ACCESS_DENIED) ? EACCES
              : (e == ERROR_DISK_FULL)     ? ENOSPC
              : EIO;
        return -1;
    }
    return (ssize_t)n;
}

ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        auto ret = pread(fd, iov[i].iov_base, iov[i].iov_len, offset + total);
        if (ret < 0) return total > 0 ? total : -1;
        total += ret;
        if ((size_t)ret < iov[i].iov_len) break;
    }
    return total;
}

ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        auto ret = pwrite(fd, iov[i].iov_base, iov[i].iov_len, offset + total);
        if (ret < 0) return total > 0 ? total : -1;
        total += ret;
    }
    return total;
}

ssize_t readlink(const char* path, char* buf, size_t bufsiz) {
    auto h = CreateFileA(path, 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                         nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;

    struct {
        ULONG  ReparseTag;
        USHORT ReparseDataLength;
        USHORT Reserved;
        union {
            struct {
                USHORT SubstituteNameOffset;
                USHORT SubstituteNameLength;
                USHORT PrintNameOffset;
                USHORT PrintNameLength;
                ULONG  Flags;
                WCHAR  PathBuffer[1];
            } SymbolicLinkReparseBuffer;
            struct {
                USHORT SubstituteNameOffset;
                USHORT SubstituteNameLength;
                USHORT PrintNameOffset;
                USHORT PrintNameLength;
                WCHAR  PathBuffer[1];
            } MountPointReparseBuffer;
            struct {
                UCHAR DataBuffer[1];
            } GenericReparseBuffer;
        };
    } rbuf;

    DWORD n;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT,
                              nullptr, 0, &rbuf, sizeof(rbuf), &n, nullptr);
    CloseHandle(h);
    if (!ok) return -1;

    if (rbuf.ReparseTag != IO_REPARSE_TAG_SYMLINK) { errno = EINVAL; return -1; }

    auto& sl = rbuf.SymbolicLinkReparseBuffer;
    auto* src = (WCHAR*)((char*)sl.PathBuffer + sl.SubstituteNameOffset);
    auto  len = sl.SubstituteNameLength / sizeof(WCHAR);

    if (len > 4 && src[0] == L'\\' && src[1] == L'?' && src[2] == L'?' && src[3] == L'\\')
        { src += 4; len -= 4; }

    auto m = WideCharToMultiByte(CP_UTF8, 0, src, (int)len, buf, (int)bufsiz - 1, nullptr, nullptr);
    if (m == 0) return -1;
    buf[m] = '\0';
    return m;
}

int truncate(const char* path, off_t length) {
    int fd = _open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return -1;
    errno_t e = _chsize_s(fd, length);
    _close(fd);
    if (e != 0) { errno = e; return -1; }
    return 0;
}

// ---- sys/uio.h implementations ----

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n = _write(fd, iov[i].iov_base, (unsigned int)iov[i].iov_len);
        if (n < 0) return -1;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    return total;
}

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        errno = 0;
        ssize_t n = _read(fd, iov[i].iov_base, (unsigned int)iov[i].iov_len);
        if (n < 0) {
            // Real error — if we already made progress, return it; otherwise -1.
            if (total > 0) return total;
            if (errno == 0) errno = EIO;
            return -1;
        }
        total += n;
        if (n == 0 || (size_t)n < iov[i].iov_len) break;
    }
    return total;
}

// ---- sys/socket.h implementations ----

// LPFN_WSARECVMSG is a function pointer type obtained via WSAIoctl
typedef int (WSAAPI *LPFN_WSARECVMSG)(
    SOCKET, LPWSAMSG, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

// ---- Shared iovec→WSABUF conversion + WSAMSG fill for sendmsg/recvmsg ----
// Caller provides a pre-allocated WSABUF array (_malloca/_freea pair).

static void fill_wsamsg(WSAMSG& wm, const struct msghdr* msg, WSABUF* wsa) {
    auto n = msg->msg_iovlen;
    for (int i = 0; i < n; i++) {
        wsa[i].buf = (CHAR*)msg->msg_iov[i].iov_base;
        wsa[i].len = (ULONG)msg->msg_iov[i].iov_len;
    }
    wm = {};
    wm.name       = (LPSOCKADDR)msg->msg_name;
    wm.namelen    = msg->msg_namelen;
    wm.lpBuffers  = wsa;
    wm.dwBufferCount = (DWORD)n;
    wm.Control.buf = (CHAR*)msg->msg_control;
    wm.Control.len = (ULONG)msg->msg_controllen;
}

int sendmsg(int s, const struct msghdr* msg, int flags) {
    auto n = msg->msg_iovlen;
    auto* wsa = (WSABUF*)_malloca(sizeof(WSABUF) * n);
    if (!wsa) { WSASetLastError(WSAENOBUFS); return -1; }
    WSAMSG wm;
    fill_wsamsg(wm, msg, wsa);
    DWORD sent = 0;
    int ret = WSASendMsg((SOCKET)s, &wm, (DWORD)flags, &sent, nullptr, nullptr);
    _freea(wsa);
    if (ret != 0) { WSASetLastError(WSAEINVAL); return -1; }
    return (int)sent;
}

LPFN_WSARECVMSG _get_wsarecvmsg(SOCKET s) {
    static LPFN_WSARECVMSG fn = nullptr;
    if (!fn) {
        GUID guid = WSAID_WSARECVMSG;
        DWORD bytes;
        ::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
                   &guid, sizeof(guid), &fn, sizeof(fn), &bytes, nullptr, nullptr);
    }
    return fn;
}

int recvmsg(int s, struct msghdr* msg, int flags) {
    auto fn = _get_wsarecvmsg((SOCKET)s);
    if (!fn) { WSASetLastError(WSAENOSYS); return -1; }

    auto n = msg->msg_iovlen;
    auto* wsa = (WSABUF*)_malloca(sizeof(WSABUF) * n);
    if (!wsa) { WSASetLastError(WSAENOBUFS); return -1; }
    WSAMSG wm;
    fill_wsamsg(wm, msg, wsa);
    DWORD recvd = 0;
    int ret = fn((SOCKET)s, &wm, &recvd, nullptr, nullptr);
    for (int i = 0; i < n; i++)
        msg->msg_iov[i].iov_len = wsa[i].len;
    msg->msg_namelen = wm.namelen;
    msg->msg_flags   = wm.dwFlags;
    _freea(wsa);
    if (ret != 0) { WSASetLastError(WSAEINVAL); return -1; }
    return (int)recvd;
}

// ---- sys/mman.h implementations ----

static DWORD _mman_protect(int prot) {
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    if (prot & PROT_READ)  return PAGE_READONLY;
    if (prot & PROT_EXEC)  return PAGE_EXECUTE_READ;
    return PAGE_NOACCESS;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, long long offset) {
    (void)fd;
    (void)offset;

    if (!(flags & MAP_ANONYMOUS)) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return MAP_FAILED;
    }

    DWORD allocType = MEM_RESERVE | MEM_COMMIT;
    DWORD pageProt  = _mman_protect(prot);

    if (flags & MAP_FIXED) {
        if (addr) VirtualFree(addr, 0, MEM_RELEASE);
        return VirtualAlloc(addr, length, allocType, pageProt);
    }

    return VirtualAlloc(nullptr, length, allocType, pageProt);
}

int munmap(void* addr, size_t length) {
    (void)length;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

int mprotect(void* addr, size_t length, int prot) {
    DWORD old;
    return VirtualProtect(addr, length, _mman_protect(prot), &old) ? 0 : -1;
}

int madvise(void* addr, size_t length, int advice) {
    if (advice == MADV_DONTNEED) {
        VirtualAlloc(addr, length, MEM_RESET, PAGE_READWRITE);
        return 0;
    }
    (void)addr;
    (void)length;
    return 0;
}

// ---- sys/statvfs.h implementations ----

int statvfs(const char* path, struct statvfs* buf) {
    ULARGE_INTEGER free_bytes_available, total_bytes, total_free_bytes;
    if (!GetDiskFreeSpaceExA(path, &free_bytes_available, &total_bytes, &total_free_bytes))
        return -1;
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize  = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = total_bytes.QuadPart / 4096;
    buf->f_bfree  = total_free_bytes.QuadPart / 4096;
    buf->f_bavail = free_bytes_available.QuadPart / 4096;
    buf->f_namemax = 255;
    return 0;
}

#endif // _WIN32
