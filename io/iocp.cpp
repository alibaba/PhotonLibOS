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

// AFD + IOCP event engine.
//
// Uses the Windows Ancillary Function Driver (AFD) poll API via IOCTL_AFD_POLL
// to get socket readiness notifications delivered through an IOCP.
//
// This mirrors the epoll model: AFD provides kernel-side readiness tracking
// (push, not poll), and IOCP serves as the completion queue for waking
// GetQueuedCompletionStatus(). The combination avoids the O(N) copy cost of
// WSAPoll, scales to many fds, and gives us true event-driven behaviour.
//
// A TCP loopback socket pair is kept solely for cascading wait_for_events
// wakeup, matching the epoll engine's self-fd trick.
//
// Type note — int vs HANDLE vs SOCKET:
// Photon uses int for all file descriptors and sockets.  On Windows HANDLE
// and SOCKET are pointer-sized (UINT_PTR).  This int representation is safe
// specifically for CRT file descriptors and Winsock SOCKET values, because
// both the CRT and Winsock allocate them as small table indices (well below
// 2^31) rather than real kernel pointers.  Raw kernel HANDLEs (CreateFile,
// etc.) do NOT share this property and cannot be represented as int.
//
// In the AFD poll path below, the Handle field is typed as HANDLE per the
// Windows SDK definition.  The value we store in it is a socket fd (int).
// This is safe because fd values from the CRT and Winsock are small table
// indices, not real kernel pointers; see type note above.

#include <errno.h>
#include <vector>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <winternl.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>   // struct msghdr, WSAMSG
#include <malloc.h>       // _malloca / _freea
#endif

#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>
#include <photon/io/fd-events.h>
#include <photon/net/basic_socket.h>
#include "events_map.h"
#include "reset_handle.h"
#include "iocp.h"

namespace photon {

using ::msghdr;  // bring global msghdr (from sys/socket.h) into photon namespace

// ---------------------------------------------------------------------------
// AFD poll constants (undocumented Windows internal API, stable since Vista)
// ---------------------------------------------------------------------------

#ifndef IOCTL_AFD_POLL
#define IOCTL_AFD_POLL 0x00012024
#endif

#ifndef AFD_POLL_RECEIVE
#define AFD_POLL_RECEIVE           0x0001
#define AFD_POLL_RECEIVE_EXPEDITED 0x0002
#define AFD_POLL_SEND              0x0004
#define AFD_POLL_DISCONNECT        0x0010
#define AFD_POLL_ABORT             0x0040
#define AFD_POLL_LOCAL_CLOSE       0x0080
#endif

#pragma pack(push, 1)
typedef struct _AFD_POLL_HANDLE_INFO {
    HANDLE   Handle;
    ULONG    Events;
    NTSTATUS Status;
} AFD_POLL_HANDLE_INFO;

typedef struct _AFD_POLL_INFO {
    LARGE_INTEGER        Timeout;
    ULONG                NumberOfHandles;
    ULONG                Exclusive;
    AFD_POLL_HANDLE_INFO Handles[1];
} AFD_POLL_INFO;
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Per-operation context for overlapped I/O
// ---------------------------------------------------------------------------

// Stored in OVERLAPPED::hEvent (unused by IOCP; the field is free for user data).
// The struct must outlive the I/O operation (typically stack-allocated in the
// calling wrapper function, which sleeps until the operation completes).
struct iocp_io_op {
    OVERLAPPED     ov;
    photon::thread* th;         // sleeping thread to wake on completion
    ssize_t        result;      // bytes transferred, or -errno on failure
};

// ---------------------------------------------------------------------------
// AFD → Photon event translation
// ---------------------------------------------------------------------------

// Photon EVENT_READ  → AFD_POLL_RECEIVE | DISCONNECT | ABORT
// Photon EVENT_WRITE → AFD_POLL_SEND    | ABORT
// Photon EVENT_ERROR → AFD_POLL_DISCONNECT | ABORT
static constexpr ULONG AFD_READ  = AFD_POLL_RECEIVE | AFD_POLL_DISCONNECT | AFD_POLL_ABORT;
static constexpr ULONG AFD_WRITE = AFD_POLL_SEND    | AFD_POLL_ABORT;
static constexpr ULONG AFD_ERROR = AFD_POLL_DISCONNECT | AFD_POLL_ABORT;

const static uint32_t EVENT_RWEO = EVENT_RWE | ONE_SHOT;

// ---------------------------------------------------------------------------
// InFlightEvent: per-fd tracking (same as epoll)
// ---------------------------------------------------------------------------

struct InFlightEvent {
    uint32_t interests = 0;
    void* reader_data = nullptr;
    void* writer_data = nullptr;
    void* error_data  = nullptr;
};

// ---------------------------------------------------------------------------
// TCP loopback socket pair — used only for cascading wait_for_events wakeup
// ---------------------------------------------------------------------------

const static ::SOCKET INVALID_SOCK = (::SOCKET)(~0);

static int create_wakeup_pair(::SOCKET socks[2]) {
    socks[0] = socks[1] = INVALID_SOCK;

    ::SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCK)
        LOG_ERRNO_RETURN(0, -1, "failed to create listener for wakeup pair");

    DEFER(if (listener != INVALID_SOCK) closesocket(listener));

    ::sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, (::sockaddr*)&addr, sizeof(addr)) != 0)
        LOG_ERRNO_RETURN(0, -1, "failed to bind wakeup listener");

    int addrlen = sizeof(addr);
    if (getsockname(listener, (::sockaddr*)&addr, &addrlen) != 0)
        LOG_ERRNO_RETURN(0, -1, "failed to getsockname wakeup listener");

    if (::listen(listener, 1) != 0)
        LOG_ERRNO_RETURN(0, -1, "failed to listen wakeup listener");

    ::SOCKET writer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (writer == INVALID_SOCK)
        LOG_ERRNO_RETURN(0, -1, "failed to create wakeup writer");
    DEFER(if (writer != INVALID_SOCK && socks[1] == INVALID_SOCK) closesocket(writer));

    if (::connect(writer, (::sockaddr*)&addr, sizeof(addr)) != 0)
        LOG_ERRNO_RETURN(0, -1, "failed to connect wakeup writer");

    ::SOCKET reader = accept(listener, nullptr, nullptr);
    if (reader == INVALID_SOCK)
        LOG_ERRNO_RETURN(0, -1, "failed to accept wakeup reader");

    u_long mode = 1;
    ::ioctlsocket(reader, FIONBIO, &mode);
    ::ioctlsocket(writer, FIONBIO, &mode);

    socks[0] = reader;
    socks[1] = writer;
    return 0;
}

// ---------------------------------------------------------------------------
// EventEngineIOCP
// ---------------------------------------------------------------------------

class EventEngineIOCP : public MasterEventEngine,
                        public CascadingEventEngine,
                        public ResetHandle {
public:
    // ---- IOCP & AFD handles ----
    HANDLE  _iocp = nullptr;
    HANDLE  _afd  = nullptr;

    // ---- Wakeup socket pair (cascading wait only) ----
    ::SOCKET _wakeup_reader = INVALID_SOCK;
    ::SOCKET _wakeup_writer = INVALID_SOCK;

    // ---- Per-fd tracking ----
    std::vector<InFlightEvent> _inflight_events;

    // ---- AFD poll state ----
    OVERLAPPED _ov = {};
    bool       _ov_pending = false;
    bool       _needs_resubmit = true;
    uint64_t   _ov_seq = 0;              // bumped on every submit
    std::vector<uint8_t> _poll_buf;   // AFD_POLL_INFO buffer

    // ---- Completion key constants ----
    static constexpr ULONG_PTR IOCP_KEY_AFD   = 1;  // AFD poll completion
    static constexpr ULONG_PTR IOCP_KEY_WAKE  = 2;  // cancel_wait
    static constexpr ULONG_PTR IOCP_KEY_IO    = 3;  // overlapped I/O completion

    // ---- Lifetime ----

    int init() {
        // 1. Open AFD device
        _afd = ::CreateFileW(
            L"\\\\.\\Afd",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (_afd == INVALID_HANDLE_VALUE) {
            LOG_ERRNO_RETURN(0, -1, "failed to open AFD device");
        }

        // 2. Create IOCP
        _iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (!_iocp) {
            LOG_ERRNO_RETURN(0, -1, "failed to create IOCP");
        }

        // 3. Associate AFD handle with the IOCP
        if (!::CreateIoCompletionPort(_afd, _iocp, IOCP_KEY_AFD, 0)) {
            LOG_ERRNO_RETURN(0, -1, "failed to associate AFD with IOCP");
        }

        // 4. Create wakeup socket pair
        ::SOCKET pair[2];
        if (create_wakeup_pair(pair) < 0)
            return -1;
        _wakeup_reader = pair[0];
        _wakeup_writer = pair[1];

        return 0;
    }

    int reset() override {
        if (_afd) {
            ::CancelIo(_afd);  // cancel any pending I/O
            ::CloseHandle(_afd);
            _afd = nullptr;
        }
        if (_iocp) {
            ::CloseHandle(_iocp);
            _iocp = nullptr;
        }
        if (_wakeup_reader != INVALID_SOCK) {
            closesocket(_wakeup_reader);
            _wakeup_reader = INVALID_SOCK;
        }
        if (_wakeup_writer != INVALID_SOCK) {
            closesocket(_wakeup_writer);
            _wakeup_writer = INVALID_SOCK;
        }
        _inflight_events.clear();
        _poll_buf.clear();
        memset(&_ov, 0, sizeof(_ov));
        _ov_pending = false;
        _ov_seq = 0;
        _needs_resubmit = true;
        return init();
    }

    virtual ~EventEngineIOCP() override {
        LOG_INFO("Finish event engine: iocp");
        if (_afd) {
            ::CancelIo(_afd);
            ::CloseHandle(_afd);
        }
        if (_iocp) ::CloseHandle(_iocp);
        if (_wakeup_reader != INVALID_SOCK) closesocket(_wakeup_reader);
        if (_wakeup_writer != INVALID_SOCK) closesocket(_wakeup_writer);
    }

    // ---- AFD poll helpers ----

    // Cancel any in-flight AFD poll and clear pending state.
    void cancel_afd_poll() {
        if (!_ov_pending) return;
        ::CancelIoEx(_afd, &_ov);
        // Drain the cancelled completion from the IOCP queue.
        // We don't care about the result because we're about to resubmit.
        //
        // NOTE: CancelIoEx is asynchronous — the cancellation completion may
        // not have been posted to the IOCP by the time we enter this loop.
        // A timeout=0 drain therefore sometimes misses the ghost, which would
        // then show up in the main event loop AFTER a fresh resubmit.  We tag
        // every OVERLAPPED with a monotonic sequence (_ov.Pointer = _ov_seq)
        // and have the main loop discard completions whose tag doesn't match
        // the current sequence.
        DWORD bytes;
        ULONG_PTR key;
        OVERLAPPED* ov;
        while (::GetQueuedCompletionStatus(_iocp, &bytes, &key, &ov, 0) &&
               key == IOCP_KEY_AFD && ov == &_ov) {
            // drained
        }
        memset(&_ov, 0, sizeof(_ov));
        _ov_pending = false;
    }

    // Build AFD_POLL_INFO from current _inflight_events and submit it.
    int submit_afd_poll() {
        cancel_afd_poll();

        // Count active fds
        size_t nhandles = 0;
        for (size_t fd = 0; fd < _inflight_events.size(); fd++) {
            if (_inflight_events[fd].interests & EVENT_RWE)
                nhandles++;
        }
        // +1 for the wakeup reader socket
        nhandles++;

        // Allocate / resize buffer
        size_t buf_size = sizeof(AFD_POLL_INFO) + (nhandles - 1) * sizeof(AFD_POLL_HANDLE_INFO);
        if (_poll_buf.size() < buf_size)
            _poll_buf.resize(buf_size);
        memset(_poll_buf.data(), 0, buf_size);

        auto* info = (AFD_POLL_INFO*)_poll_buf.data();
        info->Timeout.QuadPart = -1;  // infinite: AFD will wait until event
        info->NumberOfHandles = (ULONG)nhandles;
        info->Exclusive = 0;

        // Populate handle entries.  Index 0 is the wakeup reader.
        info->Handles[0].Handle = (HANDLE)(intptr_t)_wakeup_reader;
        info->Handles[0].Events = AFD_POLL_RECEIVE;

        ULONG idx = 1;
        for (size_t fd = 0; fd < _inflight_events.size(); fd++) {
            auto& entry = _inflight_events[fd];
            if (!(entry.interests & EVENT_RWE)) continue;

            ULONG afd_events = 0;
            if (entry.interests & EVENT_READ)  afd_events |= AFD_READ;
            if (entry.interests & EVENT_WRITE) afd_events |= AFD_WRITE;
            if (entry.interests & EVENT_ERROR) afd_events |= AFD_ERROR;

            info->Handles[idx].Handle = (HANDLE)(intptr_t)fd;
            info->Handles[idx].Events = afd_events;
            idx++;
        }

        // Submit.  The OVERLAPPED must be zeroed.
        memset(&_ov, 0, sizeof(_ov));
        ++_ov_seq;
        _ov.Pointer = (PVOID)(uintptr_t)_ov_seq;
        DWORD bytes;
        BOOL ok = ::DeviceIoControl(
            _afd, IOCTL_AFD_POLL,
            _poll_buf.data(), (DWORD)buf_size,
            _poll_buf.data(), (DWORD)buf_size,
            &bytes, &_ov);

        if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
            LOG_ERRNO_RETURN(0, -1, "AFD poll DeviceIoControl failed");
        }
        _ov_pending = true;
        _needs_resubmit = false;
        return 0;
    }

    // ---- CascadingEventEngine ----

    virtual int add_interest(Event e) override {
        if (e.fd < 0)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid file descriptor ", e.fd);
        if (unlikely(!e.interests))
            return 0;
        if (unlikely((size_t)e.fd >= _inflight_events.size()))
            _inflight_events.resize(e.fd * 2 + 16);

        e.interests &= EVENT_RWEO;
        auto& entry = _inflight_events[e.fd];
        auto eint = entry.interests & EVENT_RWEO;

        if (eint) {
            if ((eint ^ e.interests) & ONE_SHOT)
                LOG_ERROR_RETURN(EALREADY, -1, "conflicted ONE_SHOT flag");
            auto intersection = e.interests & eint;
            auto data = (entry.reader_data != e.data) * EVENT_READ  |
                        (entry.writer_data != e.data) * EVENT_WRITE |
                        (entry.error_data  != e.data) * EVENT_ERROR;
            if (intersection & data)
                LOG_ERROR_RETURN(EALREADY, -1, "conflicted interest(s)");
        }

        eint |= e.interests;
        entry.interests |= eint;
        if (e.interests & EVENT_READ)  entry.reader_data = e.data;
        if (e.interests & EVENT_WRITE) entry.writer_data = e.data;
        if (e.interests & EVENT_ERROR) entry.error_data  = e.data;

        _needs_resubmit = true;
        return 0;
    }

    virtual int rm_interest(Event e) override {
        if (e.fd < 0 || (size_t)e.fd >= _inflight_events.size())
            LOG_ERROR_RETURN(EINVAL, -1, "invalid file descriptor ", e.fd);
        if (unlikely(e.interests == 0)) return 0;

        auto& entry = _inflight_events[e.fd];
        auto eint = entry.interests & EVENT_RWEO;
        auto intersection = e.interests & eint;
        if (intersection == 0) return 0;

        entry.interests ^= intersection;
        if (intersection & EVENT_READ)  entry.reader_data = nullptr;
        if (intersection & EVENT_WRITE) entry.writer_data = nullptr;
        if (intersection & EVENT_ERROR) entry.error_data  = nullptr;

        _needs_resubmit = true;
        return 0;
    }

    // ---- MasterEventEngine ----

    // Drain wakeup bytes from the loopback reader socket.
    void drain_wakeup() {
        char buf[64];
        while (::net_shim::recv((int)_wakeup_reader, buf, sizeof(buf), 0) > 0) {}
    }

    // Drain any spurious IOCP wake completions (from PostQueuedCompletionStatus).
    void drain_iocp_wake() {
        DWORD bytes;
        ULONG_PTR key;
        OVERLAPPED* ov;
        while (::GetQueuedCompletionStatus(_iocp, &bytes, &key, &ov, 0) &&
               key == IOCP_KEY_WAKE) {
            // drained
        }
    }

    virtual ssize_t wait_and_fire_events(uint64_t timeout) override {
        // Resubmit if interests changed
        if (_needs_resubmit || !_ov_pending) {
            if (submit_afd_poll() < 0)
                return -1;
        }

        // Wait on IOCP
        DWORD  ms = (timeout == (uint64_t)-1) ? INFINITE : (DWORD)(timeout / 1000);
        DWORD  bytes;
        ULONG_PTR   key;
        OVERLAPPED* ov;

        BOOL ok = ::GetQueuedCompletionStatus(_iocp, &bytes, &key, &ov, ms);
        if (!ok && !ov) {
            // Timeout or error with no OVERLAPPED
            if (::GetLastError() == WAIT_TIMEOUT)
                return 0;
            LOG_WARN("GetQueuedCompletionStatus failed, err=", ::GetLastError());
            return -1;
        }

        if (key == IOCP_KEY_WAKE) {
            // cancel_wait() woke us
            drain_iocp_wake();
            _needs_resubmit = true;
            return 0;
        }

        if (key == IOCP_KEY_IO) {
            // Overlapped I/O completion.  The OVERLAPPED::hEvent field
            // carries our iocp_io_op pointer (set by pread/pwrite/etc.).
            auto* op = (iocp_io_op*)ov->hEvent;
            op->result = ok ? (ssize_t)bytes : -(ssize_t)::GetLastError();
            thread_interrupt(op->th, EOK);
            return 1;
        }

        if (key != IOCP_KEY_AFD || ov != &_ov) {
            LOG_WARN("unexpected IOCP completion, key=", key);
            _needs_resubmit = true;
            return 0;
        }

        // Stale ghost completion from a previous cancel.  See comment in
        // cancel_afd_poll().  Drop it; the real completion for the current
        // submit is still in flight.
        if ((uint64_t)(uintptr_t)ov->Pointer != _ov_seq) {
            _needs_resubmit = true;
            return 0;
        }

        _ov_pending = false;

        // AFD poll completed.  Check the Status field for each handle.
        ssize_t fired = 0;
        auto* info = (AFD_POLL_INFO*)_poll_buf.data();
        for (ULONG i = 0; i < info->NumberOfHandles; i++) {
            auto& h = info->Handles[i];
            if (h.Status != 0)  // STATUS_SUCCESS == 0 means event occurred
                continue;       // NTSTATUS non-zero: no event

            int fd = (int)(intptr_t)h.Handle;
            if (fd == (int)_wakeup_reader) {
                drain_wakeup();
                continue;
            }

            if ((size_t)fd >= _inflight_events.size())
                continue;

            auto& entry = _inflight_events[fd];
            uint32_t events = 0;

            if ((h.Events & AFD_POLL_RECEIVE) && (entry.interests & EVENT_READ)) {
                events |= EVENT_READ;
                thread_interrupt((thread*)entry.reader_data, EOK);
                fired++;
            }
            if ((h.Events & AFD_POLL_SEND) && (entry.interests & EVENT_WRITE)) {
                events |= EVENT_WRITE;
                thread_interrupt((thread*)entry.writer_data, EOK);
                fired++;
            }
            if ((h.Events & (AFD_POLL_DISCONNECT | AFD_POLL_ABORT)) && (entry.interests & EVENT_ERROR)) {
                events |= EVENT_ERROR;
                thread_interrupt((thread*)entry.error_data, EOK);
                fired++;
            }

            if (events && (entry.interests & ONE_SHOT)) {
                rm_interest({fd, events, nullptr});
            }
        }

        _needs_resubmit = true;
        return fired;
    }

    virtual int cancel_wait() override {
        // Post a completion packet to the IOCP to wake GetQueuedCompletionStatus.
        // This is callable from any vCPU.
        if (!::PostQueuedCompletionStatus(_iocp, 0, IOCP_KEY_WAKE, nullptr))
            return -1;
        return 0;
    }

    virtual int wait_for_fd(int fd, uint32_t interest, Timeout timeout) override {
        if (fd < 0)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid fd");
        if (interest & (interest - 1))
            LOG_ERROR_RETURN(EINVAL, -1, "can not wait for multiple interests");
        if (unlikely(interest == 0))
            return rm_interest({fd, EVENT_RWE | ONE_SHOT, 0});

        int ret = add_interest({fd, interest | ONE_SHOT, CURRENT});
        if (ret < 0)
            LOG_ERROR_RETURN(0, -1, "failed to add event interest");

        // Wake up the poll loop so it picks up our new interest.
        // Signal the wakeup writer; drain_wakeup() in wait_and_fire_events
        // will consume it and the loop will resubmit.
        char byte = 0;
        ::net_shim::send((int)_wakeup_writer, &byte, 1, 0);

        SCOPED_PAUSE_WORK_STEALING;
        ret = thread_usleep(timeout);
        ERRNO err;
        if (ret == -1 && err.no == EOK) {
            return 0;
        }
        rm_interest({fd, interest, 0});
        errno = (ret == 0) ? ETIMEDOUT : err.no;
        return -1;
    }

    // ---- CascadingEventEngine (advanced multi-fd wait) ----

    virtual ssize_t wait_for_events(void** data, size_t count, Timeout timeout) override {
        // Cascading wait: use the master engine to sleep until the wakeup
        // reader becomes readable (meaning an event fired or cancel_wait
        // was called), then drain events with 0 timeout.
        int ret = ::photon::wait_for_fd_readable((int)_wakeup_reader, timeout);
        if (ret < 0) {
            return errno == ETIMEDOUT ? 0 : -1;
        }

        // Drain without blocking
        auto ptr = data;
        auto end = data + count;

        if (_needs_resubmit || !_ov_pending) {
            if (submit_afd_poll() < 0) return -1;
        }

        // Peek the IOCP queue with 0 timeout
        DWORD bytes;
        ULONG_PTR key;
        OVERLAPPED* ov;
        while ((end - ptr) >= 3) {
            BOOL ok = ::GetQueuedCompletionStatus(_iocp, &bytes, &key, &ov, 0);
            if (!ok && !ov) break;

            if (key == IOCP_KEY_WAKE) {
                drain_iocp_wake();
                continue;
            }
            if (key != IOCP_KEY_AFD || ov != &_ov) break;

            // Stale ghost from a previous cancel — drop and keep peeking.
            if ((uint64_t)(uintptr_t)ov->Pointer != _ov_seq) {
                _needs_resubmit = true;
                continue;
            }

            _ov_pending = false;
            auto* info = (AFD_POLL_INFO*)_poll_buf.data();
            for (ULONG i = 0; i < info->NumberOfHandles; i++) {
                if (info->Handles[i].Status != 0) continue;
                int fd = (int)(intptr_t)info->Handles[i].Handle;
                if (fd == (int)_wakeup_reader) { drain_wakeup(); continue; }
                if ((size_t)fd >= _inflight_events.size()) continue;
                auto& entry = _inflight_events[fd];
                if (entry.reader_data) { *ptr++ = entry.reader_data; }
                if (entry.writer_data) { *ptr++ = entry.writer_data; }
                if (entry.error_data)  { *ptr++ = entry.error_data; }
            }
        }

        _needs_resubmit = true;
        if (ptr == data) return 0;
        return ptr - data;
    }

    // ---- Async I/O wrappers (iouring-equivalent) ----

    // Associate a file/socket with the IOCP for overlapped I/O.
    // Returns the underlying HANDLE for direct use with ReadFile/WriteFile.
    // On failure, returns INVALID_HANDLE_VALUE and sets errno.
    HANDLE _get_iocp_handle(int fd) {
        auto h = (HANDLE)(intptr_t)_get_osfhandle(fd);
        if (h == INVALID_HANDLE_VALUE || !h) {
            errno = EBADF;
            return INVALID_HANDLE_VALUE;
        }
        if (!::CreateIoCompletionPort(h, _iocp, IOCP_KEY_IO, 0)) {
            errno = EIO;
            return INVALID_HANDLE_VALUE;
        }
        return h;
    }

    // Common async I/O loop: submit an overlapped operation, sleep until
    // completion or timeout, cancel on timeout.
    ssize_t _wait_io(HANDLE h, iocp_io_op& op, Timeout timeout) {
        auto ret = thread_usleep(timeout);
        if (ret == -1) {
            ERRNO err;
            if (err.no == EOK) {
                // I/O completed: result was stored by the IOCP handler
                return op.result;
            }
            // Interrupted externally; cancel the I/O
            ::CancelIoEx(h, &op.ov);
            // Drain the cancellation completion
            DWORD bytes;
            ULONG_PTR key;
            OVERLAPPED* ov;
            ::GetQueuedCompletionStatus(_iocp, &bytes, &key, &ov, INFINITE);
            errno = err.no;
            return -1;
        }
        // Timeout
        ::CancelIoEx(h, &op.ov);
        DWORD bytes;
        ULONG_PTR key;
        OVERLAPPED* ov;
        ::GetQueuedCompletionStatus(_iocp, &bytes, &key, &ov, INFINITE);
        errno = ETIMEDOUT;
        return -1;
    }

public:
    ssize_t pread(int fd, void* buf, size_t count, off_t offset, Timeout timeout) {
        auto h = _get_iocp_handle(fd);
        if (h == INVALID_HANDLE_VALUE) return -1;

        iocp_io_op op = {};
        op.th = CURRENT;
        op.ov.hEvent = (HANDLE)&op;
        op.ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
        op.ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);

        BOOL ok = ::ReadFile(h, buf, (DWORD)count, nullptr, &op.ov);
        if (ok) {
            // Completed synchronously
            return (ssize_t)op.ov.InternalHigh;
        }
        if (::GetLastError() != ERROR_IO_PENDING) {
            errno = EIO;
            return -1;
        }
        return _wait_io(h, op, timeout);
    }

    ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset, Timeout timeout) {
        auto h = _get_iocp_handle(fd);
        if (h == INVALID_HANDLE_VALUE) return -1;

        iocp_io_op op = {};
        op.th = CURRENT;
        op.ov.hEvent = (HANDLE)&op;
        op.ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
        op.ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);

        BOOL ok = ::WriteFile(h, buf, (DWORD)count, nullptr, &op.ov);
        if (ok) return (ssize_t)op.ov.InternalHigh;
        if (::GetLastError() != ERROR_IO_PENDING) {
            errno = EIO;
            return -1;
        }
        return _wait_io(h, op, timeout);
    }

    int fsync(int fd, Timeout timeout) {
        auto h = _get_iocp_handle(fd);
        if (h == INVALID_HANDLE_VALUE) return -1;

        if (!::FlushFileBuffers(h)) {
            errno = EIO;
            return -1;
        }
        return 0;
    }

    int fdatasync(int fd, Timeout timeout) {
        // Windows doesn't distinguish fsync/fdatasync
        return fsync(fd, timeout);
    }

    ssize_t send(int fd, const void* buf, size_t count, int flags, Timeout timeout) {
        auto h = _get_iocp_handle(fd);
        if (h == INVALID_HANDLE_VALUE) return -1;

        iocp_io_op op = {};
        op.th = CURRENT;
        op.ov.hEvent = (HANDLE)&op;

        WSABUF wbuf = { (ULONG)count, (CHAR*)buf };
        DWORD sent = 0;
        int ret = ::WSASend((SOCKET)(intptr_t)fd, &wbuf, 1, &sent, (DWORD)flags,
                            &op.ov, nullptr);
        if (ret == 0) return (ssize_t)sent;
        if (::WSAGetLastError() != WSA_IO_PENDING) {
            errno = EIO;
            return -1;
        }
        return _wait_io(h, op, timeout);
    }

    ssize_t recv(int fd, void* buf, size_t count, int flags, Timeout timeout) {
        auto h = _get_iocp_handle(fd);
        if (h == INVALID_HANDLE_VALUE) return -1;

        iocp_io_op op = {};
        op.th = CURRENT;
        op.ov.hEvent = (HANDLE)&op;

        WSABUF wbuf = { (ULONG)count, (CHAR*)buf };
        DWORD recvd = 0;
        DWORD wflags = (DWORD)flags;
        int ret = ::WSARecv((SOCKET)(intptr_t)fd, &wbuf, 1, &recvd, &wflags,
                            &op.ov, nullptr);
        if (ret == 0) return (ssize_t)recvd;
        if (::WSAGetLastError() != WSA_IO_PENDING) {
            errno = EIO;
            return -1;
        }
        return _wait_io(h, op, timeout);
    }

    ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count,
                     Timeout timeout) {
        // POSIX sendfile semantics: count is a byte count; count == 0 → nothing to do.
        if (count == 0) return 0;

        // Socket handle drives the overlapped I/O; register the socket with IOCP.
        auto sock = _get_iocp_handle(out_fd);
        if (sock == INVALID_HANDLE_VALUE) return -1;
        // Retrieve the file HANDLE from the CRT fd. We don't register the file
        // with IOCP — TransmitFile posts completion to the socket's IOCP.
        auto fh = (HANDLE)(intptr_t)_get_osfhandle(in_fd);
        if (fh == INVALID_HANDLE_VALUE || !fh) {
            errno = EBADF;
            return -1;
        }

        iocp_io_op op = {};
        op.th = CURRENT;
        op.ov.hEvent = (HANDLE)&op;
        if (offset) {
            op.ov.Offset     = (DWORD)((uint64_t)*offset & 0xFFFFFFFFu);
            op.ov.OffsetHigh = (DWORD)((uint64_t)*offset >> 32);
        }

        // Single TransmitFile call, clamped to DWORD_MAX. The upper layer
        // (sendfile_n / BufStep) is responsible for looping over the full
        // requested range; here we just attempt one kernel hand-off, same as
        // the Linux / macOS sendfile wrappers do.
        DWORD to_send = (count > (size_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (DWORD)count;

        BOOL ok = ::TransmitFile((SOCKET)(intptr_t)out_fd, fh, to_send, 0,
                                 &op.ov, nullptr, 0);
        ssize_t sent;
        if (ok) {
            sent = (ssize_t)op.ov.InternalHigh;
        } else if (::GetLastError() == ERROR_IO_PENDING) {
            sent = _wait_io(sock, op, timeout);
        } else {
            errno = EIO;
            return -1;
        }
        if (sent > 0 && offset) *offset += sent;
        return sent;
    }

    ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset,
                   Timeout timeout) {
        ssize_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            auto ret = pread(fd, iov[i].iov_base, iov[i].iov_len,
                             offset + (off_t)total, timeout);
            if (ret < 0) return total > 0 ? total : -1;
            total += ret;
            if ((size_t)ret < iov[i].iov_len) break;  // EOF
        }
        return total;
    }

    ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset,
                    Timeout timeout) {
        ssize_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            auto ret = pwrite(fd, iov[i].iov_base, iov[i].iov_len,
                              offset + (off_t)total, timeout);
            if (ret < 0) return total > 0 ? total : -1;
            total += ret;
        }
        return total;
    }

    ssize_t sendmsg(int fd, const struct msghdr* msg, int flags, Timeout timeout) {
        auto h = _get_iocp_handle(fd);
        if (h == INVALID_HANDLE_VALUE) return -1;

        // Convert iovec[] → WSABUF[] (different field order)
        auto n = msg->msg_iovlen;
        auto* wsa = (WSABUF*)_malloca(sizeof(WSABUF) * n);
        if (!wsa && n > 0) { errno = ENOMEM; return -1; }
        for (int i = 0; i < n; i++) {
            wsa[i].buf = (CHAR*)msg->msg_iov[i].iov_base;
            wsa[i].len = (ULONG)msg->msg_iov[i].iov_len;
        }

        WSAMSG wmsg = {};
        wmsg.name       = (LPSOCKADDR)msg->msg_name;
        wmsg.namelen    = msg->msg_namelen;
        wmsg.lpBuffers  = wsa;
        wmsg.dwBufferCount = (DWORD)n;
        wmsg.Control.buf = (CHAR*)msg->msg_control;
        wmsg.Control.len = (ULONG)msg->msg_controllen;

        iocp_io_op op = {};
        op.th = CURRENT;
        op.ov.hEvent = (HANDLE)&op;

        DWORD sent = 0;
        int ret = ::WSASendMsg((SOCKET)(intptr_t)fd, &wmsg, (DWORD)flags,
                               &sent, &op.ov, nullptr);
        if (n > 0) _freea(wsa);
        if (ret == 0) return (ssize_t)sent;
        if (::WSAGetLastError() != WSA_IO_PENDING) {
            errno = EIO;
            return -1;
        }
        return _wait_io(h, op, timeout);
    }

    ssize_t recvmsg(int fd, struct msghdr* msg, int flags, Timeout timeout) {
        auto h = _get_iocp_handle(fd);
        if (h == INVALID_HANDLE_VALUE) return -1;

        auto n = msg->msg_iovlen;
        auto* wsa = (WSABUF*)_malloca(sizeof(WSABUF) * n);
        if (!wsa && n > 0) { errno = ENOMEM; return -1; }
        for (int i = 0; i < n; i++) {
            wsa[i].buf = (CHAR*)msg->msg_iov[i].iov_base;
            wsa[i].len = (ULONG)msg->msg_iov[i].iov_len;
        }

        WSAMSG wmsg = {};
        wmsg.name       = (LPSOCKADDR)msg->msg_name;
        wmsg.namelen    = msg->msg_namelen;
        wmsg.lpBuffers  = wsa;
        wmsg.dwBufferCount = (DWORD)n;
        wmsg.Control.buf = (CHAR*)msg->msg_control;
        wmsg.Control.len = (ULONG)msg->msg_controllen;

        // WSARecvMsg is resolved once (cached static) in sys/socket.h
        LPFN_WSARECVMSG fn = _get_wsarecvmsg((SOCKET)(intptr_t)fd);
        if (!fn) {
            if (n > 0) _freea(wsa);
            errno = ENOSYS;
            return -1;
        }

        iocp_io_op op = {};
        op.th = CURRENT;
        op.ov.hEvent = (HANDLE)&op;

        DWORD recvd = 0;
        int ret = fn((SOCKET)(intptr_t)fd, &wmsg, &recvd, &op.ov, nullptr);
        if (n > 0) _freea(wsa);
        if (ret == 0) {
            msg->msg_namelen = wmsg.namelen;
            msg->msg_flags   = wmsg.dwFlags;
            return (ssize_t)recvd;
        }
        if (::WSAGetLastError() != WSA_IO_PENDING) {
            errno = EIO;
            return -1;
        }
        auto result = _wait_io(h, op, timeout);
        if (result >= 0) {
            msg->msg_namelen = wmsg.namelen;
            msg->msg_flags   = wmsg.dwFlags;
        }
        return result;
    }

    int close(int fd, Timeout timeout) {
        (void)timeout;
        auto h = (HANDLE)(intptr_t)_get_osfhandle(fd);
        if (h == INVALID_HANDLE_VALUE || !h) {
            errno = EBADF;
            return -1;
        }
        // Cancel any pending I/O on this handle
        ::CancelIo(h);
        return ::_close(fd);
    }
};

// ---- Factory functions ----

__attribute__((noinline)) static
EventEngineIOCP* new_iocp_engine(ALogStringL role) {
    LOG_INFO("Init iocp event engine: ", role);
    return NewObj<EventEngineIOCP>()->init();
}

MasterEventEngine* new_iocp_master_engine() {
    return new_iocp_engine("master");
}

CascadingEventEngine* new_iocp_cascading_engine() {
    return new_iocp_engine("cascading");
}

// ---- Free wrapper functions (call into the master engine) ----

static EventEngineIOCP* _get_iocp_engine(CascadingEventEngine* engine = nullptr) {
    if (engine) return (EventEngineIOCP*)engine;
    return (EventEngineIOCP*)get_vcpu()->master_event_engine;
}

ssize_t iocp_pread(int fd, void* buf, size_t count, off_t offset, Timeout timeout,
                   CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->pread(fd, buf, count, offset, timeout);
}

ssize_t iocp_pwrite(int fd, const void* buf, size_t count, off_t offset, Timeout timeout,
                    CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->pwrite(fd, buf, count, offset, timeout);
}

ssize_t iocp_preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset,
                    Timeout timeout, CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->preadv(fd, iov, iovcnt, offset, timeout);
}

ssize_t iocp_pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset,
                     Timeout timeout, CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->pwritev(fd, iov, iovcnt, offset, timeout);
}

int iocp_fsync(int fd, Timeout timeout, CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->fsync(fd, timeout);
}

int iocp_fdatasync(int fd, Timeout timeout, CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->fdatasync(fd, timeout);
}

int iocp_close(int fd, Timeout timeout, CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->close(fd, timeout);
}

ssize_t iocp_send(int fd, const void* buf, size_t count, int flags, Timeout timeout,
                  CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->send(fd, buf, count, flags, timeout);
}

ssize_t iocp_recv(int fd, void* buf, size_t count, int flags, Timeout timeout,
                  CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->recv(fd, buf, count, flags, timeout);
}

ssize_t iocp_sendmsg(int fd, const struct msghdr* msg, int flags, Timeout timeout,
                     CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->sendmsg(fd, msg, flags, timeout);
}

ssize_t iocp_recvmsg(int fd, struct msghdr* msg, int flags, Timeout timeout,
                     CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->recvmsg(fd, msg, flags, timeout);
}

ssize_t iocp_sendfile(int out_fd, int in_fd, off_t* offset, size_t count,
                      Timeout timeout, CascadingEventEngine* engine) {
    return _get_iocp_engine(engine)->sendfile(out_fd, in_fd, offset, count, timeout);
}

int iocp_open(const char* pathname, int flags, mode_t mode) {
    (void)mode;   // Windows has no UNIX permission model; security descriptor ignored here

    DWORD access = 0;
    if ((flags & O_RDWR) == O_RDWR)       access = GENERIC_READ | GENERIC_WRITE;
    else if (flags & O_WRONLY)            access = GENERIC_WRITE;
    else                                  access = GENERIC_READ;

    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    DWORD creation = OPEN_EXISTING;
    if (flags & O_CREAT) {
        creation = (flags & O_EXCL)  ? CREATE_NEW
                 : (flags & O_TRUNC) ? CREATE_ALWAYS
                 :                     OPEN_ALWAYS;
    } else if (flags & O_TRUNC) {
        creation = TRUNCATE_EXISTING;
    }

    DWORD attrs = FILE_FLAG_OVERLAPPED;
    if (flags & _O_TEMPORARY) attrs |= FILE_ATTRIBUTE_TEMPORARY;

    HANDLE h = ::CreateFileA(pathname, access, share, nullptr, creation,
                             attrs, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = ::GetLastError();
        errno = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT
              : (e == ERROR_ACCESS_DENIED)                               ? EACCES
              : (e == ERROR_FILE_EXISTS  || e == ERROR_ALREADY_EXISTS)   ? EEXIST
              :                                                            EIO;
        return -1;
    }

    // O_APPEND: seek to end once. Subsequent writes still need FILE_APPEND_DATA
    // or manual seeks for strict append semantics.
    if (flags & O_APPEND) ::SetFilePointer(h, 0, nullptr, FILE_END);

    int fd = _open_osfhandle((intptr_t)h, 0);
    if (fd < 0) {
        ::CloseHandle(h);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

}  // namespace photon
