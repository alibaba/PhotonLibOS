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

#include <photon/net/kcp.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <queue>
#include <random>
#include <unordered_map>
#include <vector>
#include <unistd.h>

#include <ikcp.h>

#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/net/datagram_socket.h>
#include <photon/net/socket.h>
#include "base_socket.h"
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

namespace photon {
namespace net {

static const int KCP_DEFAULT_MTU = 1400;
static const int KCP_DEFAULT_SNDWND = 32;
static const int KCP_DEFAULT_RCVWND = 128;
static const int KCP_DEFAULT_INTERVAL = 100;
static const int KCP_RECV_BUF_SIZE = 65536;
static const int KCP_MSS_MAX = 1400;  // max segment size = MTU - 24 header
static const int KCP_HEADER_SIZE = 24;
static const uint8_t KCP_CMD_PUSH = 81;

// --------------------------------------------------------------------------- //
// KcpSocketStream                                                             //
// --------------------------------------------------------------------------- //

class KcpSocketBase;

class KcpSocketStream : public ISocketStream {
public:
    KcpSocketStream(UDPSocket* dgram, const EndPoint& remote, KcpSocketBase* base)
        : m_dgram(dgram), m_remote(remote), m_base(base) { }

    ~KcpSocketStream() override { close(); }

    bool init(uint32_t conv, SockOptBuffer& opts, uint64_t timeout) {
        m_kcp = ikcp_create(conv, this);
        if (!m_kcp)
            LOG_ERROR_RETURN(ENOMEM, false, "failed to create kcp control block");
        m_kcp->stream = 1;
        ikcp_setoutput(m_kcp, &output_callback);
        ikcp_nodelay(m_kcp, 0, KCP_DEFAULT_INTERVAL, 0, 0);
        ikcp_setmtu(m_kcp, KCP_DEFAULT_MTU);
        ikcp_wndsize(m_kcp, KCP_DEFAULT_SNDWND, KCP_DEFAULT_RCVWND);
        ikcp_update(m_kcp, (uint32_t)(photon::now / 1000));
        for (auto& opt : opts)
            setsockopt(opt.level, opt.opt_name, opt.opt_val, opt.opt_len);
        m_timeout = timeout;
        return true;
    }

    uint32_t conv() const { return m_kcp->conv; }
    const EndPoint& remote() const { return m_remote; }
    bool closed() const { return m_closed.load(std::memory_order_acquire); }

    int input(const char* data, int len) {
        if (m_closed.load(std::memory_order_acquire))
            return 0;
        {
            SCOPED_LOCK(m_mutex);
            if (m_closed.load(std::memory_order_acquire))
                return 0;
            ikcp_input(m_kcp, data, len);
        }
        m_cv.notify_one();
        return 0;
    }

    // ---- ISocketStream / IStream ----

    ssize_t recv(void* buf, size_t count, int flags = 0) override {
        SCOPED_LOCK(m_mutex);
        if (m_recv_buf_off < m_recv_buf_size) {
            uint16_t available = m_recv_buf_size - m_recv_buf_off;
            uint16_t to_copy = std::min((uint16_t)count, available);
            memcpy(buf, m_recv_buf + m_recv_buf_off, to_copy);
            m_recv_buf_off += to_copy;
            if (m_recv_buf_off >= m_recv_buf_size) {
                m_recv_buf_size = 0;
                m_recv_buf_off = 0;
            }
            return (ssize_t)to_copy;
        }
        while (true) {
            if (m_closed.load(std::memory_order_acquire)) {
                errno = ECONNRESET;
                return -1;
            }
            int n = ikcp_recv(m_kcp, m_recv_buf, KCP_MSS_MAX);
            if (n > 0) {
                m_recv_buf_size = (uint16_t)n;
                m_recv_buf_off = 0;
                uint16_t to_copy = std::min((uint16_t)count, (uint16_t)n);
                memcpy(buf, m_recv_buf, to_copy);
                m_recv_buf_off = to_copy;
                return (ssize_t)to_copy;
            }
            if (m_closed.load(std::memory_order_acquire)) {
                errno = ECONNRESET;
                return -1;
            }
            int ret = m_cv.wait(m_mutex, Timeout(m_timeout));
            if (ret < 0 && errno == ETIMEDOUT)
                return -1;
        }
    }

    ssize_t recv(const struct iovec* iov, int iovcnt, int flags) override {
        if (iovcnt <= 0) return 0;
        return recv(iov[0].iov_base, iov[0].iov_len, flags);
    }

    ssize_t send(const void* buf, size_t count, int flags = 0) override {
        SCOPED_LOCK(m_mutex);
        if (m_closed.load(std::memory_order_acquire)) {
            errno = ECONNRESET;
            return -1;
        }
        int ret;
        while ((ret = ikcp_send(m_kcp, (const char*)buf, (int)count)) == -2) {
            m_cv.wait(m_mutex, Timeout(m_timeout));
            if (m_closed.load(std::memory_order_acquire)) {
                errno = ECONNRESET;
                return -1;
            }
        }
        if (ret < 0)
            LOG_ERROR_RETURN(EIO, -1, "ikcp_send failed");
        if (m_flush)
            ikcp_flush(m_kcp);
        return (ssize_t)count;
    }

    ssize_t send(const struct iovec* iov, int iovcnt, int flags) override {
        if (iovcnt <= 0) return 0;
        SCOPED_LOCK(m_mutex);
        if (m_closed.load(std::memory_order_acquire)) {
            errno = ECONNRESET;
            return -1;
        }
        ssize_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            int ret;
            while ((ret = ikcp_send(m_kcp, (const char*)iov[i].iov_base,
                                     (int)iov[i].iov_len)) == -2) {
                if (total > 0) break;
                m_cv.wait(m_mutex, Timeout(m_timeout));
                if (m_closed.load(std::memory_order_acquire)) {
                    errno = ECONNRESET;
                    return total > 0 ? total : -1;
                }
            }
            if (ret == -2)
                break;
            if (ret < 0)
                LOG_ERROR_RETURN(EIO, -1, "ikcp_send failed");
            total += (ssize_t)iov[i].iov_len;
        }
        if (m_flush)
            ikcp_flush(m_kcp);
        return total;
    }

    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        char buf[65536];
        ssize_t total = 0;
        while (count > 0) {
            size_t to_read = sizeof(buf) < count ? sizeof(buf) : count;
            ssize_t n = ::pread(in_fd, buf, to_read, offset);
            if (n <= 0) break;
            ssize_t sent = send(buf, (size_t)n, 0);
            if (sent < 0) return -1;
            offset += n;
            count -= n;
            total += n;
        }
        return total;
    }

    int close() override {
        bool was = m_closed.exchange(true, std::memory_order_acq_rel);
        if (was) return 0;

        m_cv.notify_all();

        notify_base_closed();

        {
            SCOPED_LOCK(m_mutex);
            if (m_kcp) {
                ikcp_update(m_kcp, (uint32_t)(photon::now / 1000));
                ikcp_flush(m_kcp);
                ikcp_release(m_kcp);
                m_kcp = nullptr;
            }
        }
        return 0;
    }

    int shutdown(ShutdownHow /*how*/) override { errno = ENOSYS; return -1; }

    ssize_t read(void* buf, size_t count) override {
        size_t total = 0;
        while (total < count) {
            ssize_t n = recv((char*)buf + total, count - total, 0);
            if (n <= 0)
                return total > 0 ? (ssize_t)total : n;
            total += n;
        }
        return (ssize_t)total;
    }
    ssize_t readv(const struct iovec* iov, int iovcnt) override {
        size_t done = 0;
        for (int i = 0; i < iovcnt; i++) {
            char* base = (char*)iov[i].iov_base;
            size_t need = iov[i].iov_len;
            while (need > 0) {
                ssize_t n = recv(base, need, 0);
                if (n <= 0)
                    return done > 0 ? (ssize_t)done : n;
                base += n;
                need -= (size_t)n;
                done += (size_t)n;
            }
        }
        return (ssize_t)done;
    }
    ssize_t write(const void* buf, size_t count) override {
        size_t total = 0;
        while (total < count) {
            ssize_t n = send((const char*)buf + total, count - total, 0);
            if (n <= 0)
                return total > 0 ? (ssize_t)total : n;
            total += n;
        }
        return (ssize_t)total;
    }
    ssize_t writev(const struct iovec* iov, int iovcnt) override {
        size_t done = 0;
        for (int i = 0; i < iovcnt; i++) {
            const char* base = (const char*)iov[i].iov_base;
            size_t need = iov[i].iov_len;
            while (need > 0) {
                ssize_t n = send(base, need, 0);
                if (n <= 0)
                    return done > 0 ? (ssize_t)done : n;
                base += n;
                need -= (size_t)n;
                done += (size_t)n;
            }
        }
        return (ssize_t)done;
    }

    uint64_t timeout() const override { return m_timeout; }
    void timeout(uint64_t tm) override { m_timeout = tm; }

    // ---- ISocketBase ----

    int setsockopt(int level, int option_name, const void* option_value,
                   socklen_t option_len) override {
        return (level == SOL_KCP) ? set_kcp_opt(option_name, option_value, option_len) :
                     m_dgram->setsockopt(level, option_name, option_value, option_len) ;
    }

    int getsockopt(int level, int option_name, void* option_value,
                   socklen_t* option_len) override {
        return (level == SOL_KCP) ? get_kcp_opt(option_name, option_value, option_len) :
                     m_dgram->getsockopt(level, option_name, option_value, option_len) ;
    }

    Object* get_underlay_object(uint64_t recursion) override {
        if (recursion == 0) return (Object*)m_dgram;
        return m_dgram->get_underlay_object(recursion - 1);
    }

    // ---- ISocketName ----

    int getsockname(EndPoint& addr) override { return m_dgram->getsockname(addr); }
    int getpeername(EndPoint& addr) override { addr = m_remote; return 0; }
    int getsockname(char* /*path*/, size_t /*count*/) override { errno = ENOSYS; return -1; }
    int getpeername(char* /*path*/, size_t /*count*/) override { errno = ENOSYS; return -1; }

    static int output_callback(const char* buf, int len, ikcpcb* /*kcp*/, void* user) {
        auto self = static_cast<KcpSocketStream*>(user);
        ssize_t ret = self->m_dgram->sendto(buf, (size_t)len, self->m_remote);
        return (ret >= 0) ? 0 : -1;
    }

    void notify_base_closed();

    void update(uint32_t current) {
        if (m_closed.load(std::memory_order_acquire)) return;
        SCOPED_LOCK(m_mutex);
        if (m_closed.load(std::memory_order_acquire)) return;
        int old_waitsnd = ikcp_waitsnd(m_kcp);
        auto old_nrcv_que = m_kcp->nrcv_que;
        ikcp_update(m_kcp, current);
        if (m_kcp->state == -1u) {
            m_closed.store(true, std::memory_order_release);
            m_cv.notify_all();
            return;
        }
        if (ikcp_waitsnd(m_kcp) < old_waitsnd ||
            m_kcp->nrcv_que > old_nrcv_que)
            m_cv.notify_all();
    }

    int set_kcp_opt(int option_name, const void* option_value, socklen_t option_len) {
        if (!option_value)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid kcp option value");
        if (!m_kcp) LOG_ERROR_RETURN(ENOTCONN, -1, "kcp not initialized");
        SCOPED_LOCK(m_mutex);
        switch (option_name) {
        case KCP_NODELAY_OPTS: {
            if (option_len < 4 * sizeof(int))
                LOG_ERROR_RETURN(EINVAL, -1, "KCP_NODELAY_OPTS requires int[4]");
            const int* vals = (const int*)option_value;
            ikcp_nodelay(m_kcp, vals[0], vals[1], vals[2], vals[3]);
            return 0;
        }
        case KCP_WND_OPTS: {
            if (option_len < 2 * sizeof(int))
                LOG_ERROR_RETURN(EINVAL, -1, "KCP_WND_OPTS requires int[2]");
            const int* vals = (const int*)option_value;
            ikcp_wndsize(m_kcp, vals[0], vals[1]);
            return 0;
        }
        case KCP_MTU:
            if (option_len < sizeof(int))
                LOG_ERROR_RETURN(EINVAL, -1, "invalid kcp option value");
            ikcp_setmtu(m_kcp, *(const int*)option_value);
            break;
        case KCP_AUTO_FLUSH:
            if (option_len < sizeof(int))
                LOG_ERROR_RETURN(EINVAL, -1, "invalid kcp option value");
            m_flush = !!*(const int*)option_value;
            break;
        default:
            LOG_ERROR_RETURN(EINVAL, -1, "unknown kcp option `", VALUE(option_name));
        }
        return 0;
    }

    int get_kcp_opt(int option_name, void* option_value, socklen_t* option_len) {
        if (!option_value || !option_len)
            LOG_ERROR_RETURN(EINVAL, -1, "invalid kcp option value");
        if (!m_kcp) LOG_ERROR_RETURN(ENOTCONN, -1, "kcp not initialized");
        SCOPED_LOCK(m_mutex);
        switch (option_name) {
        case KCP_NODELAY_OPTS: {
            if (*option_len < 4 * sizeof(int))
                LOG_ERROR_RETURN(EINVAL, -1, "KCP_NODELAY_OPTS requires int[4]");
            int* vals = (int*)option_value;
            vals[0] = m_kcp->nodelay;
            vals[1] = m_kcp->interval;
            vals[2] = m_kcp->fastresend;
            vals[3] = m_kcp->nocwnd;
            *option_len = 4 * sizeof(int);
            return 0;
        }
        case KCP_WND_OPTS: {
            if (*option_len < 2 * sizeof(int))
                LOG_ERROR_RETURN(EINVAL, -1, "KCP_WND_OPTS requires int[2]");
            int* vals = (int*)option_value;
            vals[0] = m_kcp->snd_wnd;
            vals[1] = m_kcp->rcv_wnd;
            *option_len = 2 * sizeof(int);
            return 0;
        }
        case KCP_MTU:
            if (*option_len < sizeof(int))
                LOG_ERROR_RETURN(EINVAL, -1, "invalid kcp option value");
            *(int*)option_value = m_kcp->mtu;
            *option_len = sizeof(int);
            return 0;
        case KCP_AUTO_FLUSH:
            if (*option_len < sizeof(int))
                LOG_ERROR_RETURN(EINVAL, -1, "invalid kcp option value");
            *(int*)option_value = m_flush;
            *option_len = sizeof(int);
            return 0;
        case KCP_CONV:
            if (*option_len < sizeof(uint32_t))
                LOG_ERROR_RETURN(EINVAL, -1, "invalid kcp option value");
            *(uint32_t*)option_value = m_kcp->conv;
            *option_len = sizeof(uint32_t);
            return 0;
        default:
            LOG_ERROR_RETURN(EINVAL, -1, "unknown kcp option `", VALUE(option_name));
        }
    }

    UDPSocket* m_dgram;
    ikcpcb* m_kcp = nullptr;
    uint64_t m_timeout = -1ULL;
    EndPoint m_remote;

    photon::mutex m_mutex;
    photon::condition_variable m_cv;
    std::atomic<bool> m_closed{false};
    bool m_flush = 0;

    char m_recv_buf[KCP_MSS_MAX];
    uint16_t m_recv_buf_size = 0;
    uint16_t m_recv_buf_off = 0;

    KcpSocketBase* const m_base;
};

// --------------------------------------------------------------------------- //
// KcpSocketBase                                                               //
// --------------------------------------------------------------------------- //
class KcpSocketBase {
public:
    KcpSocketBase(UDPSocket* dgram) : m_dgram(dgram) {}

    ~KcpSocketBase() {
        stop_recv_thread();
        close_all_streams();
    }

    void start_recv_thread(Delegate<void> loop) {
        if (m_recv_th) return;
        m_running.store(true, std::memory_order_release);
        m_dgram->timeout((uint64_t)m_update_interval * 1000);
        m_recv_th = thread_create11(loop);
        thread_enable_join(m_recv_th, true);
    }

    void stop_recv_thread() {
        if (!m_recv_th) return;
        m_running.store(false, std::memory_order_release);
        thread_interrupt(m_recv_th, ECANCELED);
        thread_join((join_handle*)m_recv_th);
        m_recv_th = nullptr;
    }

    void update_all_streams() {
        uint32_t current = (uint32_t)(photon::now / 1000);
        if (current - m_last_update < m_update_interval) return;
        m_last_update = current;
        SCOPED_LOCK(m_streams_mutex);
        for (auto& kv : m_streams)
            kv.second->update(current);
    }

    int set_update_interval(const void* option_value, socklen_t option_len) {
        if (!option_value || option_len < sizeof(int))
            LOG_ERROR_RETURN(EINVAL, -1, "invalid kcp option value");
        int v = *(const int*)option_value;
        if (v < 10 || v > 100)
            LOG_ERROR_RETURN(EINVAL, -1, "KCP_UPDATE_INTERVAL must be in [10, 100] ms");
        m_update_interval = v;
        return 0;
    }

    int get_update_interval(void* option_value, socklen_t* option_len) {
        if (!option_value || !option_len || *option_len < sizeof(int))
            LOG_ERROR_RETURN(EINVAL, -1, "invalid kcp option value");
        *(int*)option_value = m_update_interval;
        *option_len = sizeof(int);
        return 0;
    }

    int kcp_setsockopt(SockOptBuffer& opts, int option_name,
                       const void* option_value, socklen_t option_len) {
        if (option_name == KCP_UPDATE_INTERVAL) {
            int ret = set_update_interval(option_value, option_len);
            if (ret == 0 && m_recv_th)
                m_dgram->timeout((uint64_t)m_update_interval * 1000);
            return ret;
        }
        return opts.put_opt(SOL_KCP, option_name, option_value, option_len);
    }

    int kcp_getsockopt(SockOptBuffer& opts, int option_name,
                       void* option_value, socklen_t* option_len) {
        if (option_name == KCP_UPDATE_INTERVAL)
            return get_update_interval(option_value, option_len);
        return opts.get_opt(SOL_KCP, option_name, option_value, option_len);
    }

    int setsockopt(SockOptBuffer& opts, int level, int option_name,
                   const void* option_value, socklen_t option_len) {
        if (level == SOL_KCP)
            return kcp_setsockopt(opts, option_name, option_value, option_len);
        return m_dgram->setsockopt(level, option_name, option_value, option_len);
    }

    int getsockopt(SockOptBuffer& opts, int level, int option_name,
                   void* option_value, socklen_t* option_len) {
        if (level == SOL_KCP)
            return kcp_getsockopt(opts, option_name, option_value, option_len);
        return m_dgram->getsockopt(level, option_name, option_value, option_len);
    }

    void on_stream_closed(KcpSocketStream* stream) {
        erase_stream(stream);
        m_streams_cv.notify_one();
    }

    void close_all_streams() {
        std::vector<KcpSocketStream*> to_close;
        {
            SCOPED_LOCK(m_streams_mutex);
            for (auto& pair : m_streams)
                to_close.push_back(pair.second);
        }
        for (auto* s : to_close) s->close();
        SCOPED_LOCK(m_streams_mutex);
        while (!m_streams.empty())
            m_streams_cv.wait(m_streams_mutex);
    }

    KcpSocketStream* lookup_stream(uint32_t conv, const EndPoint& from) {
        SCOPED_LOCK(m_streams_mutex);
        auto it = m_streams.find({conv, from});
        return (it == m_streams.end()) ? nullptr : it->second;
    }

    void erase_stream(KcpSocketStream* stream) {
        SCOPED_LOCK(m_streams_mutex);
        m_streams.erase({stream->conv(), stream->remote()});
    }

    struct StreamKey {
        uint32_t conv;
        EndPoint remote;
        bool operator==(const StreamKey& o) const {
            return conv == o.conv && remote == o.remote;
        }
    };
    struct StreamKeyHash {
        size_t operator()(const StreamKey& k) const {
            size_t h = std::hash<uint32_t>()(k.conv);
            h ^= std::hash<EndPoint>()(k.remote) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<StreamKey, KcpSocketStream*, StreamKeyHash> m_streams;

    photon::mutex m_streams_mutex;
    photon::condition_variable m_streams_cv;
    photon::thread* m_recv_th = nullptr;
    UDPSocket* m_dgram;
    uint32_t m_update_interval = KCP_DEFAULT_INTERVAL;  // ms, range [10, 100]
    uint32_t m_last_update = 0;
    std::atomic<bool> m_running{false};
};

// --------------------------------------------------------------------------- //
// KcpSocketClient                                                            //
// --------------------------------------------------------------------------- //

class KcpSocketClient : public SocketClientBase, public KcpSocketBase {
public:
    explicit KcpSocketClient(UDPSocket* dgram) : KcpSocketBase(dgram) {
        start_recv_thread({this, &KcpSocketClient::recv_loop});
    }

    ISocketStream* connect(const EndPoint& remote,
                           const EndPoint* local = nullptr) override {
        if (local)
            LOG_ERROR_RETURN(EINVAL, nullptr, "local endpoint not supported for kcp");

        uint32_t conv;
        auto* stream = new KcpSocketStream(m_dgram, remote, this);
        static thread_local std::mt19937 rng(std::random_device{}());
        {
            SCOPED_LOCK(m_streams_mutex);
            do { conv = rng(); }
            while (m_streams.insert({{conv, remote}, stream}).second == false);
        }
        if (!stream->init(conv, m_opts, m_timeout)) {
            erase_stream(stream);
            delete stream;
            LOG_ERROR_RETURN(0, nullptr, "failed to init kcp stream");
        }
        return stream;
    }

    ISocketStream* connect(const char* /*path*/, size_t /*count*/) override {
        errno = ENOSYS;
        return nullptr;
    }

    Object* get_underlay_object(uint64_t recursion) override {
        if (recursion == 0) return (Object*)m_dgram;
        return m_dgram->get_underlay_object(recursion - 1);
    }

    int setsockopt(int level, int option_name, const void* option_value,
                   socklen_t option_len) override {
        return KcpSocketBase::setsockopt(m_opts, level, option_name,
                                         option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value,
                   socklen_t* option_len) override {
        return KcpSocketBase::getsockopt(m_opts, level, option_name,
                                         option_value, option_len);
    }

    void recv_loop() {
        EndPoint from;
        char buf[KCP_RECV_BUF_SIZE];
        while (m_running.load(std::memory_order_acquire)) {
            ssize_t n = m_dgram->recvfrom(buf, sizeof(buf), &from);
            update_all_streams();
            if (n <= 0) {
                if (!m_running.load(std::memory_order_acquire)) break;
                continue;
            }
            uint32_t conv = ikcp_getconv(buf);
            KcpSocketStream* stream = lookup_stream(conv, from);
            if (stream)
                stream->input(buf, (int)n);
        }
    }
};

// --------------------------------------------------------------------------- //
// KcpSocketServer                                                            //
// --------------------------------------------------------------------------- //

class KcpSocketServer : public SocketServerBase, public KcpSocketBase {
public:
    explicit KcpSocketServer(UDPSocket* dgram) : KcpSocketBase(dgram) {}

    ~KcpSocketServer() override {
        terminate();
    }

    int bind(const EndPoint& ep) override {
        return m_dgram->bind(ep);
    }

    int bind(const char* /*path*/, size_t /*count*/) override {
        errno = ENOSYS;
        return -1;
    }

    int listen(int /*backlog*/) override {
        start_recv_thread({this, &KcpSocketServer::recv_loop});
        return 0;
    }

    KcpSocketStream* accept_queue_pop() {
        SCOPED_LOCK(m_queue_mutex);
        if (m_accept_queue.empty())
            return nullptr;
        auto stream = m_accept_queue.front();
        m_accept_queue.pop();
        return stream;
    }

    ISocketStream* accept(EndPoint* remote_endpoint = nullptr) override {
        Timeout timeout(m_timeout);
    again:
        int ret = m_accept_sem.wait(1, timeout);
        if (ret < 0) return nullptr;
        KcpSocketStream* stream = accept_queue_pop();
        if (!stream) goto again;
        if (remote_endpoint)
            stream->getpeername(*remote_endpoint);
        return stream;
    }

    ISocketServer* set_handler(Handler handler) override {
        m_handler = handler;
        return this;
    }

    int start_loop(bool /*block*/ = false) override {
        m_started.store(true, std::memory_order_release);
        return 0;
    }

    void terminate() override {
        if (!m_running.load(std::memory_order_acquire)) return;
        stop_recv_thread();
        m_accept_sem.signal(1);
    }

    int setsockopt(int level, int option_name, const void* option_value,
                   socklen_t option_len) override {
        return KcpSocketBase::setsockopt(m_opts, level, option_name,
                                         option_value, option_len);
    }

    int getsockopt(int level, int option_name, void* option_value,
                   socklen_t* option_len) override {
        return KcpSocketBase::getsockopt(m_opts, level, option_name,
                                         option_value, option_len);
    }

    Object* get_underlay_object(uint64_t recursion) override {
        if (recursion == 0) return (Object*)m_dgram;
        return m_dgram->get_underlay_object(recursion - 1);
    }

    int getsockname(EndPoint& addr) override { return m_dgram->getsockname(addr); }
    int getpeername(EndPoint& /*addr*/) override { errno = ENOTCONN; return -1; }
    int getsockname(char* /*path*/, size_t /*count*/) override { errno = ENOSYS; return -1; }
    int getpeername(char* /*path*/, size_t /*count*/) override { errno = ENOSYS; return -1; }

    bool is_kcp_connect(const char* buf, int len) {
        if (len < KCP_HEADER_SIZE) return false;
        uint8_t cmd = *(const uint8_t*)(buf + 4);
        uint32_t sn = *(const uint32_t*)(buf + 12);
        return cmd == KCP_CMD_PUSH && sn == 0;
    }

    void recv_loop() {
        EndPoint from;
        char buf[KCP_RECV_BUF_SIZE];
        while (m_running.load(std::memory_order_acquire)) {
            ssize_t n = m_dgram->recvfrom(buf, sizeof(buf), &from);
            update_all_streams();
            if (n <= 0) {
                if (!m_running.load(std::memory_order_acquire)) break;
                continue;
            }
            uint32_t conv = ikcp_getconv(buf);
            KcpSocketStream* stream = lookup_stream(conv, from);
            if (stream) {
                if (stream->closed())
                    continue;
            } else {
                if (!is_kcp_connect(buf, (int)n))
                    continue;
                stream = new KcpSocketStream(m_dgram, from, this);
                if (!stream->init(conv, m_opts, m_timeout)) {
                    LOG_ERROR("failed to init kcp stream for conv `", conv);
                    delete stream;
                    continue;
                }
                bool inserted;
                {
                    SCOPED_LOCK(m_streams_mutex);
                    inserted = m_streams.emplace(StreamKey{conv, from}, stream).second;
                }
                if (!inserted) {
                    delete stream;
                    continue;
                }
                if (m_handler && m_started.load(std::memory_order_acquire)) {
                    thread_create11(m_handler, stream);
                } else {
                    SCOPED_LOCK(m_queue_mutex);
                    m_accept_queue.push(stream);
                    m_accept_sem.signal(1);
                }
            }
            assert(stream);
            stream->input(buf, (int)n);
        }
    }

    std::queue<KcpSocketStream*> m_accept_queue;
    photon::mutex m_queue_mutex;
    photon::semaphore m_accept_sem{0};

    Handler m_handler;
    std::atomic<bool> m_started{false};
};

inline void KcpSocketStream::notify_base_closed() {
    m_base->on_stream_closed(this);
}

// --------------------------------------------------------------------------- //
// Factory functions                                                          //
// --------------------------------------------------------------------------- //

extern "C" ISocketClient* new_kcp_socket_client(UDPSocket* datagram_socket) {
    return new KcpSocketClient(datagram_socket);
}

extern "C" ISocketServer* new_kcp_socket_server(UDPSocket* datagram_socket) {
    return new KcpSocketServer(datagram_socket);
}

}  // namespace net
}  // namespace photon
