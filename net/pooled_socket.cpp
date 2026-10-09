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

#include "socket.h"
#include <netinet/tcp.h>
#include <vector>
#include <unordered_set>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/string_view.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/thread/timer.h>
#include <photon/net/basic_socket.h>
#include <photon/net/utils.h>
#include "base_socket.h"

namespace photon {
namespace net {

class TCPSocketPool;
struct StreamListHead;

class PooledTCPSocketStream : public ForwardSocketStream {
public:
    TCPSocketPool* pool;
    StreamListHead* head;
    bool drop = false;

    PooledTCPSocketStream(ISocketStream* stream, TCPSocketPool* pool, StreamListHead* head)
            : ForwardSocketStream(stream, false), pool(pool), head(head) { }
    // release socket back to pool when dtor
    ~PooledTCPSocketStream() override;
    // forwarding all actions
    int shutdown(ShutdownHow how) override {
        drop = true;
        return m_underlay->shutdown(how);
    }

#define FORWARD_SOCK_ACT(pred, action, count) \
    if (count == 0) return 0;                 \
    auto ret = m_underlay->action;            \
    drop = std::pred<ssize_t>()(ret, 0);      \
    return ret

    int close() override {
        drop = true;
        m_underlay->close();
        return 0;
    }
    ssize_t read(void* buf, size_t count) override {
        FORWARD_SOCK_ACT(less_equal, read(buf, count), count);
    }
    ssize_t write(const void* buf, size_t count) override {
        FORWARD_SOCK_ACT(less, write(buf, count), count);
    }
    ssize_t readv(const struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(less_equal, readv(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t readv_mutable(struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(less_equal, readv_mutable(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t writev(const struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(less, writev(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t writev_mutable(struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(less, writev_mutable(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t recv(void* buf, size_t count, int flags = 0) override {
        FORWARD_SOCK_ACT(less_equal, recv(buf, count, flags), count);
    }
    ssize_t recv(const struct iovec* iov, int iovcnt, int flags = 0) override {
        FORWARD_SOCK_ACT(less_equal, recv(iov, iovcnt, flags),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t send(const void* buf, size_t count, int flags = 0) override {
        FORWARD_SOCK_ACT(less, send(buf, count, flags), count);
    }
    ssize_t send(const struct iovec* iov, int iovcnt, int flags = 0) override {
        FORWARD_SOCK_ACT(less, send(iov, iovcnt, flags),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        FORWARD_SOCK_ACT(less, sendfile(in_fd, offset, count), count);
    }

#undef FORWARD_SOCK_ACT
};

struct StreamListNode : public intrusive_list_node<StreamListNode> {
    std::unique_ptr<ISocketStream> stream;
    StreamListHead* head;
    Timeout timeout;
    StreamListNode(ISocketStream* stream, StreamListHead* head, uint64_t TTL_us) :
        stream(stream), head(head), timeout(TTL_us) { }
};

struct StreamListHead : public StreamListNode {
    StreamListHead* _key_next = this;
    // total # of sockets, including those in use, not including the head
    uint32_t _refcnt = 0;
    uint16_t _key_len;
    char _key[0];

    StreamListHead(std::string_view k) : StreamListNode(0, 0, 0) {
        assert(k.size() < UINT16_MAX);
        memcpy(_key, k.data(), k.size());
        _key[_key_len = k.size()] = '\0';
    }
    std::string_view key() const { return {_key, _key_len}; }
    static StreamListHead* create(std::string_view key) {
        auto buf = malloc(sizeof(StreamListHead) + key.size() + 1);
        return new (buf) StreamListHead(key);
    }
    void destroy() {
        this->~StreamListHead();
        free(this);
    }
};

struct StreamList : intrusive_list<StreamListNode> {
    StreamList(const StreamList&) = delete;
    StreamList(StreamList&& rhs) {
        node = rhs.node;
        rhs.node = nullptr;
    }
    StreamList(std::string_view key)  {
        auto head = StreamListHead::create(key);
        push_back(head);
    }
    ~StreamList() {
        auto h = head();
        assert(h);
        if (!h->single()) {
            auto ptr = h->remove_from_list();
            intrusive_list<StreamListNode> list(ptr);
            h->_refcnt -= list.delete_all();
        }
        if (h->_refcnt)
            LOG_ERROR("there are still ` living socket stream(s) outside the pool!", h->_refcnt);
        h->destroy();
        node = nullptr;
    }
    StreamListHead* head() const {
        return (StreamListHead*)node;
    }
    void operator=(const StreamList&) = delete;
    void operator=(StreamList&& rhs) {
        if (this == &rhs) return;
        if (node) delete_all();
        node = rhs.node;
        rhs.node = nullptr;
    }
};

struct Hash {
    size_t operator()(const StreamList& list) const {
        return std::hash<std::string_view>()(list.head()->key());
    }
};

struct Equal {
    bool operator()(const StreamList& a, const StreamList& b) const {
        return a.head()->key() == b.head()->key();
    }
    bool operator()(const StreamList& a, std::string_view b) const {
        return a.head()->key() == b;
    }
};

class TCPSocketPool : public ISocketPool {
public:
    SocketPoolArgs args;
    std::unordered_set<StreamList, Hash, Equal> sockmap;
    StreamListHead *_key_head = nullptr;
    CascadingEventEngine* ev = new_default_cascading_engine();
    join_handle* collector = thread_enable_join(
        thread_create11(&TCPSocketPool::collect, this));

    TCPSocketPool(const SocketPoolArgs& args) : args(args) { }

    ~TCPSocketPool() override {
        auto th = collector;
        collector = nullptr;
        thread_interrupt((thread*)th);
        thread_join(th);
        delete ev;
        if (args.client_ownership)
            delete args.sockclient;
    }

    __attribute__((noinline))
    static void logerr_no_client() {
        LOG_ERROR_RETURN(ENOSYS, , "a socket client must be provided when socket pool was constructed");
    }
    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        if (!args.sockclient) return logerr_no_client(), -1;
        return args.sockclient->setsockopt(level, option_name, option_value, option_len);
    }
    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        if (!args.sockclient) return logerr_no_client(), -1;
        return args.sockclient->getsockopt(level, option_name, option_value, option_len);
    }
    uint64_t timeout() const override {
        if (!args.sockclient) return logerr_no_client(), -1;
        return args.sockclient->timeout();
    }
    void timeout(uint64_t tm) override {
        if (!args.sockclient) return logerr_no_client();
        args.sockclient->timeout(tm);
    }
    Object* get_underlay_object(uint64_t recursion = 0) override {
        return (recursion == 0) ? args.sockclient :
                                  args.sockclient->get_underlay_object(recursion - 1);
    }

    // all fd < 0 treated as socket not based on fd
    // and always reuseable. Using such socket needs user
    // to check if connected socket is still usable.
    // if there still have unread bytes in stream, it should be closed.
    bool stream_reusable(int fd) {
        return (fd < 0) || (wait_for_fd_readable(fd, 0) != 0);
    }

    void add_watch(int fd, StreamListNode* node) {
        if (fd >= 0) ev->add_interest({fd, EVENT_READ, node});
    }

    int rm_watch(StreamListNode* node) {
        auto fd = node->stream->get_underlay_fd();
        if (fd >= 0) ev->rm_interest({fd, EVENT_READ, node});
        return fd;
    }

    ISocketStream* connect(const char* path, size_t count) override {
        LOG_ERROR_RETURN(ENOSYS, nullptr,
                         "Socket pool supports TCP-like socket only");
    }
    ISocketStream* connect(const EndPoint& remote,
                           const EndPoint* local) override {
        std::string_view key{(char*)&remote, sizeof(remote)};
        return connect(key, remote, local);
    }
    ISocketStream* connect(std::string_view key, const EndPoint& remote, const EndPoint* local = nullptr) override {
        return connect(key, [&]() -> ISocketStream* {
            if (!args.sockclient) return logerr_no_client(), nullptr;
            return args.sockclient->connect(remote, local);
        });
    }
    ISocketStream* connect(std::string_view domain_name, uint16_t port, const EndPoint* local = nullptr) override {
        return connect(domain_name, domain_name, port, local);
    }
    ISocketStream* connect(std::string_view key, std::string_view domain_name, uint16_t port, const EndPoint* local = nullptr) override {
        return connect(key, [&]() -> ISocketStream* {
            if (!args.sockclient) return logerr_no_client(), nullptr;
            std::vector<IPAddr> addrs;
            int ret = gethostbyname_nb(domain_name, addrs);
            if (ret < 0 || addrs.empty())
                LOG_ERRNO_RETURN(0, nullptr, "failed to resolve ", VALUE(domain_name));
            EndPoint ep{addrs[rand() % addrs.size()], port};
            return args.sockclient->connect(ep, local);
        });
    }
    ISocketStream* connect(std::string_view key, TempDelegate<ISocketStream*> connector) override {
        auto ret = sockmap.emplace(key);
        auto head = ret.first->head();
        if (ret.second) { // new entry, prepend to key list
            head->_key_next = _key_head;
            _key_head = head;
        }
        ISocketStream* stream;
        if (head->single()) {
            head->_refcnt++;
            stream = connector();
            if (!stream) {
                head->_refcnt--;
                return nullptr;
            }
            if (args.enable_tcp_keepalive)
                set_keepalive(stream);
        } else {
            auto node = head->next();
            node->remove_from_list();
            rm_watch(node);
            stream = node->stream.release();
            delete node;
        }
        return new PooledTCPSocketStream(stream, this, head);
    }

    void set_keepalive(ISocketStream* stream) {
        stream->setsockopt<int>(SOL_SOCKET, SO_KEEPALIVE, 1);
#ifndef _WIN32
#ifdef __APPLE__
#define TCP_KEEPIDLE  TCP_KEEPALIVE
#define SOL_TCP       IPPROTO_TCP
#endif
        stream->setsockopt<int>(SOL_TCP, TCP_KEEPIDLE, args.tcp_keepalive_time);
        stream->setsockopt<int>(SOL_TCP, TCP_KEEPINTVL, args.tcp_keepalive_intvl);
        stream->setsockopt<int>(SOL_TCP, TCP_KEEPCNT, args.tcp_keepalive_probes);
#else
        struct tcp_keepalive {
            u_long onoff;
            u_long keepalivetime;
            u_long keepaliveinterval;
        } keepalive_params;
        keepalive_params.onoff = 1;
        keepalive_params.keepalivetime = args.tcp_keepalive_time * 1000;
        keepalive_params.keepaliveinterval = args.tcp_keepalive_intvl * 1000;
        DWORD bytes_returned = 0;
        SOCKET s = (SOCKET)(intptr_t)stream->get_underlay_fd();
        WSAIoctl(s, SIO_KEEPALIVE_VALS,
                    &keepalive_params, sizeof(keepalive_params),
                    nullptr, 0, &bytes_returned, nullptr, nullptr);
#endif
    }

    void release(StreamListHead* head, ISocketStream* stream) {
        auto fd = stream->get_underlay_fd();
        ERRNO err;
        if (!stream_reusable(fd)) {
            assert(head->_refcnt > 0);
            head->_refcnt--;
            delete stream;
            return;
        }
        auto node = new StreamListNode(stream, head, args.expiration);
        head->insert_before(node);
        assert(head->prev() == node);
        add_watch(fd, node);
        errno = err.no;
    }

    Timeout check_expire_heartbeat() {
        Timeout near_expire(args.expiration);
        for (auto h = _key_head; h; h = h->_key_next) {
            // Expire timed-out nodes
            auto ptr = h->next();
            while (ptr != h) {
                if (now < ptr->timeout.expiration()) {
                    near_expire = std::min(near_expire, ptr->timeout);
                    break;
                }
                auto next = ptr->remove_from_list();
                rm_watch(ptr);
                assert(h->_refcnt > 0);
                h->_refcnt--;
                delete ptr;
                ptr = next;
            }
            // Heartbeat remaining nodes
            if (args.heartbeater) {
                while (ptr != h) {
                    // Take node out of the list so connect() cannot grab it
                    // while the heartbeater yields (I/O).
                    auto next = ptr->remove_from_list();
                    rm_watch(ptr);
                    int ret = args.heartbeater(ptr->stream.get());
                    if (ret == 0) {
                        h->insert_before(ptr);
                        add_watch(ptr->stream->get_underlay_fd(), ptr);
                        ptr = next;
                    } else {
                        assert(h->_refcnt > 0);
                        h->_refcnt--;
                        delete ptr;
                        ptr = next;
                    }
                }
            }
        }
        // Erase empty entries, unlinking from key list as we go
        auto *prev_next = &_key_head;
        for (auto h = _key_head; h; ) {
            if (h->_refcnt) {
                prev_next = &h->_key_next;
                h = h->_key_next;
            } else {
                auto next = h->_key_next;
                *prev_next = next;
                sockmap.erase(h->key()); // frees h via StreamList destructor
                h = next;
            }
        }
        assert(near_expire.expiration() > now);
        return near_expire;
    }

    void check_fd_state(Timeout timeout) {
    again:
        StreamListNode* nodes[16];
        auto ret = ev->wait_for_events((void**)nodes, 16, timeout);
        for (int i = 0; i < ret; i++) {
            // since destructed socket should never become readable before
            // it have been acquired again
            // if it is readable or RDHUP, both condition should treat as
            // socket shutdown
            assert(!nodes[i]->single());
            assert(nodes[i] != nodes[i]->head);
            assert(nodes[i]->head->_refcnt > 0);
            nodes[i]->head->_refcnt--;
            nodes[i]->remove_from_list();
            rm_watch(nodes[i]);
        }
        for (int i = 0; i < ret; i++) delete nodes[i];
        if (ret == (int)LEN(nodes)) goto again; // may have more
    }

    void collect() {
        auto interval = std::min(args.heartbeat_interval,
                                 args.expiration);
        Timeout timeout(interval);
        while (collector) {
            check_fd_state(timeout);
            auto next = check_expire_heartbeat();
            timeout.timeout(interval);
            if (timeout > next)
                timeout = next;
        }
    }
};

inline PooledTCPSocketStream::~PooledTCPSocketStream() {
    if (drop) {
        delete m_underlay;
        head->_refcnt--;
    } else {
        pool->release(head, m_underlay);
    }
}

extern "C" ISocketPool* new_tcp_socket_pool(const SocketPoolArgs& args) {
    return new TCPSocketPool(args);
}

}
}
