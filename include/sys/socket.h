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
#include <cstdint>
#include <cstring>

// Windows (MinGW): include native winsock headers.  These define in6_addr,
// sockaddr_in6, in_addr, etc.  MinGW's in6_addr has u.Word (u_short[8]) for
// 16-bit word access but no s6_addr32 for 32-bit word access — the
// _in_addr_word(addr, i) macro below provides uniform 32-bit access across
// all platforms.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

// Winsock's getsockname / getpeername / accept / recvfrom all take `int*` for
// the address-length parameter, while POSIX uses `socklen_t*`.  MinGW
// typedefs socklen_t as int (ws2tcpip.h), so the bit-pattern (int*)socklen_t*
// casts below are safe.  The static_assert fails loudly if a future toolchain
// ever ships socklen_t as something wider (size_t / long), which would make
// the cast write through a mis-sized pointer and corrupt the caller's length.
static_assert(sizeof(socklen_t) == sizeof(int),
              "MinGW socklen_t must be int-sized for (int*)socklen_t* casts");

// Winsock has no native equivalents for these POSIX flags.  Synthesize
// non-zero sentinels; the socket() / accept4() shims below read these bits
// and apply the right platform-specific setup (ioctlsocket FIONBIO, etc.).
// Non-zero lets call sites pass the same flags across platforms.
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x80000000
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x40000000
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef SO_REUSEPORT
#define SO_REUSEPORT 0
#endif

// POSIX cmsghdr for ancillary data (may already be in mswsock.h)
#ifndef CMSG_FIRSTHDR
struct cmsghdr {
    socklen_t cmsg_len;
    int       cmsg_level;
    int       cmsg_type;
};
#define CMSG_FIRSTHDR(m)  ((m)->msg_controllen >= sizeof(cmsghdr) ? (cmsghdr*)(m)->msg_control : nullptr)
#define CMSG_DATA(cmsg)   ((unsigned char*)(cmsg) + sizeof(cmsghdr))
#endif

// POSIX-compatible msghdr
struct msghdr {
    void*         msg_name;
    socklen_t     msg_namelen;
    struct iovec* msg_iov;
    int           msg_iovlen;
    void*         msg_control;
    socklen_t     msg_controllen;
    int           msg_flags;
};

// Implemented in common/win32_compat.cpp
int sendmsg(int s, const struct msghdr* msg, int flags);
int recvmsg(int s, struct msghdr* msg, int flags);

inline int inet_aton(const char* cp, struct in_addr* inp) {
    return inet_pton(AF_INET, cp, inp);
}

// LPFN_WSARECVMSG resolved at runtime via WSAIoctl
typedef int (WSAAPI *LPFN_WSARECVMSG)(
    SOCKET, LPWSAMSG, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
LPFN_WSARECVMSG _get_wsarecvmsg(SOCKET s);

#else  // POSIX (Linux / macOS / BSDs)
// Load the real libc <sys/socket.h> FIRST so native flag definitions
// (SOCK_NONBLOCK, SOCK_CLOEXEC, etc.) are visible before any shim code
// below references them.  Then define fallback sentinels only for flags
// the libc genuinely lacks (niche / older POSIX platforms).
#include_next <sys/socket.h>

#ifndef SOCK_NONBLOCK
// macOS's libc <sys/socket.h> does NOT define SOCK_NONBLOCK (unlike Linux).
// Use 0x20000000 (BSD convention) as the shim's internal sentinel.  libc
// socket() rejects this value (EINVAL), so the shim strips it before the
// syscall; apply_flags_after_create's `requested_flags & SOCK_NONBLOCK`
// check then correctly detects the intent and applies O_NONBLOCK via fcntl.
# if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  define SOCK_NONBLOCK 0x20000000
# else
#  define SOCK_NONBLOCK 0x80000000
# endif
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef SO_REUSEPORT
#define SO_REUSEPORT 0
#endif

// macOS / BSDs: <fcntl.h> + <unistd.h> for fcntl / F_GETFL / F_SETFL /
// F_SETFD / FD_CLOEXEC / O_NONBLOCK / close, needed by the socket() and
// accept4() shims below.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <fcntl.h>
#include <unistd.h>
#endif

#endif  // _WIN32 vs POSIX

// ---------------------------------------------------------------------------
// _in_addr_word(addr, i): 32-bit word accessor for in6_addr.
//   Linux:   expands to (addr).s6_addr32[i] (native uint32_t[4])
//   macOS/BSD: expands to (addr).__u6_addr.__u6_addr32[i] (native uint32_t[4])
//   Windows: convert to uint32_t[i]
#ifdef _WIN32
#define _in_addr_word(a, i) (((uint32_t*)&(a))[i])
#else
#ifdef __linux__
#define _in_addr_word(a, i) ((a).s6_addr32[(i)])
#else
#define _in_addr_word(a, i) ((a).__u6_addr.__u6_addr32[(i)])
#endif
#endif

// ---------------------------------------------------------------------------
// POSIX-signature shims, in namespace photon::net.
//
// Call sites inside photon::net do unqualified lookup for names like
// `socket`, `accept4`, `send`, `recv`, etc. — they find these shims first,
// bypassing libc's / Winsock's global-scope declarations.  Inside shim
// bodies, libc / Winsock functions are called with :: qualification to
// avoid recursing back into the shims (e.g. `::socket(...)` hits libc /
// Winsock, not photon::net::socket).
//
// socket() and accept4() share a small amount of post-creation flag setup
// (set non-blocking via fcntl / ioctlsocket; set cloexec on BSD; set
// SO_NOSIGPIPE on macOS).  That shared logic is factored into
// apply_flags_after_create().
// ---------------------------------------------------------------------------
namespace photon {
namespace net {

namespace _internal {
// Common post-creation flag setup, shared by socket() and accept4() on
// non-Linux platforms.  Linux's libc socket() / accept4() accept
// SOCK_NONBLOCK / SOCK_CLOEXEC atomically, so this helper is unused there.
inline int apply_flags_after_create(int fd, int requested_flags) {
    (void)requested_flags;
#if !defined(__linux__)
    if (requested_flags & SOCK_NONBLOCK) {
# ifdef _WIN32
        u_long nonblock = 1;
        if (::ioctlsocket((SOCKET)(intptr_t)fd, FIONBIO, &nonblock) != 0) {
            ::closesocket((SOCKET)(intptr_t)fd);
            return -1;
        }
# else
        int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
            ::close(fd);
            return -1;
        }
# endif
    }
# if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (requested_flags & SOCK_CLOEXEC) {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            ::close(fd);
            return -1;
        }
    }
# endif
# ifdef __APPLE__
    // macOS: disable SIGPIPE on broken connections (no per-send MSG_NOSIGNAL).
    // SO_NOSIGPIPE is native to macOS (0x1022 in <sys/socket.h>).
    int _nosig = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&_nosig, sizeof(_nosig));
# endif
#endif  // !__linux__
    return fd;
}
}  // namespace _internal

// socket() shim.
//   Linux:   libc socket() accepts SOCK_NONBLOCK / SOCK_CLOEXEC atomically.
//   macOS:   libc socket() does NOT accept these flags (they're Linux-only);
//            strip them, create, then apply via fcntl(O_NONBLOCK) /
//            fcntl(FD_CLOEXEC) and setsockopt(SO_NOSIGPIPE).
//   Windows: Winsock socket() does NOT accept these flags; strip, create,
//            then apply via ioctlsocket(FIONBIO); cloexec no-op.
inline int socket(int domain, int type, int protocol) {
    // Force SOCK_NONBLOCK: photon's coroutine I/O requires non-blocking sockets
    // (blocking syscalls would stall the entire vCPU).  Callers that truly need
    // a blocking socket must clear O_NONBLOCK via fcntl after creation.
    int effective_type = type | SOCK_NONBLOCK;
#ifdef __linux__
    return ::socket(domain, effective_type, protocol);
#else
    int real_type = effective_type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
# ifdef _WIN32
    SOCKET s = ::socket(domain, real_type, protocol);
    if (s == INVALID_SOCKET) return -1;
    return _internal::apply_flags_after_create((int)s, effective_type);
# else
    int s = ::socket(domain, real_type, protocol);
    if (s < 0) return -1;
    return _internal::apply_flags_after_create(s, effective_type);
# endif
#endif
}

// accept4() shim: Linux provides accept4 natively; macOS and Windows do not.
// Accept the connection, then share the same flag-setup helper as socket().
inline int accept4(int sockfd, struct sockaddr* addr, socklen_t* addrlen, int flags) {
#ifdef __linux__
    return ::accept4(sockfd, addr, addrlen, flags);
#elif defined(_WIN32)
    SOCKET s = ::accept((SOCKET)(intptr_t)sockfd, addr, (int*)addrlen);
    if (s == INVALID_SOCKET) return -1;
    return _internal::apply_flags_after_create((int)s, flags);
#else
    int s = ::accept(sockfd, addr, addrlen);
    if (s < 0) return -1;
    return _internal::apply_flags_after_create(s, flags);
#endif
}

// POSIX-signature wrappers for Winsock APIs that differ from POSIX in the
// fd type (SOCKET vs int), option buffer type (char* vs void*), or socklen_t
// (int vs socklen_t).  On Linux / macOS these forward to libc directly; on
// Windows they cast appropriately and call Winsock.
inline int getsockname(int fd, struct sockaddr* addr, socklen_t* len) {
#ifdef _WIN32
    return ::getsockname((SOCKET)(intptr_t)fd, addr, (int*)len);
#else
    return ::getsockname(fd, addr, len);
#endif
}
inline int getpeername(int fd, struct sockaddr* addr, socklen_t* len) {
#ifdef _WIN32
    return ::getpeername((SOCKET)(intptr_t)fd, addr, (int*)len);
#else
    return ::getpeername(fd, addr, len);
#endif
}
inline int setsockopt(int fd, int level, int optname,
                      const void* optval, socklen_t optlen) {
#ifdef _WIN32
    return ::setsockopt((SOCKET)(intptr_t)fd, level, optname,
                        (const char*)optval, (int)optlen);
#else
    return ::setsockopt(fd, level, optname, optval, optlen);
#endif
}
inline int getsockopt(int fd, int level, int optname,
                      void* optval, socklen_t* optlen) {
#ifdef _WIN32
    return ::getsockopt((SOCKET)(intptr_t)fd, level, optname,
                        (char*)optval, (int*)optlen);
#else
    return ::getsockopt(fd, level, optname, optval, optlen);
#endif
}
}  // namespace net
}  // namespace photon

// ---------------------------------------------------------------------------
// Raw POSIX-signature shims (no Timeout), in global namespace net_shim.
// Named net_shim (not net) to avoid ambiguity with ::photon::net, which
// many call sites refer to as plain `net::xxx` via parent-namespace lookup.
// The Timeout-wrapped versions live in ::photon::net (net/basic_socket.h).
//
// Callers use qualified ::net_shim::send(...) to reach these shims.
// ---------------------------------------------------------------------------
namespace net_shim {
inline ssize_t send(int fd, const void* buf, size_t len, int flags) {
#ifdef _WIN32
    return (ssize_t)::send((SOCKET)(intptr_t)fd, (const char*)buf,
                           (int)len, flags);
#else
    return ::send(fd, buf, len, flags);
#endif
}
inline ssize_t recv(int fd, void* buf, size_t len, int flags) {
#ifdef _WIN32
    return (ssize_t)::recv((SOCKET)(intptr_t)fd, (char*)buf,
                           (int)len, flags);
#else
    return ::recv(fd, buf, len, flags);
#endif
}
inline ssize_t sendto(int fd, const void* buf, size_t len, int flags,
                      const struct sockaddr* to, socklen_t tolen) {
#ifdef _WIN32
    return (ssize_t)::sendto((SOCKET)(intptr_t)fd, (const char*)buf,
                             (int)len, flags, to, (int)tolen);
#else
    return ::sendto(fd, buf, len, flags, to, tolen);
#endif
}
inline ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                        struct sockaddr* from, socklen_t* fromlen) {
#ifdef _WIN32
    return (ssize_t)::recvfrom((SOCKET)(intptr_t)fd, (char*)buf,
                               (int)len, flags, from, (int*)fromlen);
#else
    return ::recvfrom(fd, buf, len, flags, from, fromlen);
#endif
}
inline ssize_t sendmsg(int fd, const struct msghdr* msg, int flags) {
    return ::sendmsg(fd, msg, flags);
}
inline ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
    return ::recvmsg(fd, msg, flags);
}
}  // namespace net_shim
