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

#include <unordered_map>

#include <photon/common/alog.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/thread/timer.h>
#include <photon/net/basic_socket.h>

#include "base_socket.h"

namespace photon {
namespace net {

class TCPSocketPool;

class PooledTCPSocketStream : public ForwardSocketStream {
public:
    TCPSocketPool* pool;
    EndPoint ep;
    bool drop;

    PooledTCPSocketStream(ISocketStream* stream, TCPSocketPool* pool, const EndPoint& ep)
            : ForwardSocketStream(stream, false), pool(pool), ep(ep), drop(false) {}
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
    EndPoint key;
    std::unique_ptr<ISocketStream> stream;
    int fd;
    Timeout timeout;

<<<<<<< HEAD
    StreamListNode(const EndPoint& key, ISocketStream* stream, int fd, uint64_t TTL_us)
        : key(key), stream(stream), fd(fd), timeout(TTL_us) {
=======
struct StreamListHead : public StreamListNode {
    StreamListHead* _key_next = this;
    // References from idle/checked-out streams, connectors and collector pins.
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
>>>>>>> 7a5a1ea (fix(net): reclaim failed pool routes safely across reentrant callbacks (#1681))
    }
};

class TCPSocketPool : public ForwardSocketClient {
protected:
    CascadingEventEngine* ev;
    photon::thread* collector;
    std::unordered_map<EndPoint, intrusive_list<StreamListNode>> fdmap;
    uint64_t TTL_us;
    photon::Timer timer;

    // all fd < 0 treated as socket not based on fd
    // and always reuseable. Using such socket needs user
    // to check if connected socket is still usable.
    // if there still have unread bytes in strema, it should be closed.
    bool stream_reusable(int fd) {
        return (fd < 0) || (wait_for_fd_readable(fd, 0) != 0);
    }

    void add_watch(StreamListNode* node) {
        if (node->fd >= 0) ev->add_interest({node->fd, EVENT_READ, node});
    }

    void rm_watch(StreamListNode* node) {
        if (node->fd >= 0) ev->rm_interest({node->fd, EVENT_READ, node});
    }

    ISocketStream* get_from_pool(const EndPoint& ep) {
        auto it = fdmap.find(ep);
        if (it == fdmap.end()) return nullptr;
        assert(it != fdmap.end());
        auto node = it->second.pop_front();
        rm_watch(node);
        DEFER(delete node);
        if (it->second.empty()) fdmap.erase(it);
        return node->stream.release();
    }

    void push_into_pool(StreamListNode* node) {
        fdmap[node->key].push_back(node);
        add_watch(node);
    }

    void drop_from_pool(StreamListNode* node) {
        // or node have no record
        auto it = fdmap.find(node->key);
        auto& list = it->second;
        list.erase(node);
        if (list.empty()) fdmap.erase(it);
        rm_watch(node);
    }

public:
    TCPSocketPool(ISocketClient* client, uint64_t TTL_us,
                  bool client_ownership = false)
        : ForwardSocketClient(client, client_ownership),
          ev(photon::new_default_cascading_engine()),
          TTL_us(TTL_us),
          timer(TTL_us, {this, &TCPSocketPool::evict}) {
        collector = (photon::thread*)photon::thread_enable_join(
            photon::thread_create11(&TCPSocketPool::collect, this));
    }

    ~TCPSocketPool() override {
        timer.stop();
        auto th = collector;
        collector = nullptr;
        photon::thread_interrupt((photon::thread*)th);
        photon::thread_join((photon::join_handle*)th);
        for (auto& l : fdmap) {
            l.second.delete_all();
        }
        delete ev;
    }

    ISocketStream* connect(const char* path, size_t count) override {
        LOG_ERROR_RETURN(ENOSYS, nullptr,
                         "Socket pool supports TCP-like socket only");
    }

    ISocketStream* connect(const EndPoint& remote,
                           const EndPoint* local) override {
    again:
        auto stream = get_from_pool(remote);
        if (!stream) {
            stream = m_underlay->connect(remote, local);
            if (!stream) return nullptr;
        } else if (!stream_reusable(stream->get_underlay_fd())) {
            delete stream;
            goto again;
        }
<<<<<<< HEAD
        return new PooledTCPSocketStream(stream, this, remote);
    }
    uint64_t evict() {
        // remove empty entry in fdmap
        intrusive_list<StreamListNode> freelist;
        uint64_t near_expire = TTL_us + now;
        for (auto it = fdmap.begin(); it != fdmap.end();) {
            auto& list = it->second;
            uint64_t exp;
            while (!list.empty() && now >=
                   (exp = list.front()->timeout.expiration())) {
                freelist.push_back(list.pop_front());
            }
            if (list.empty()) {
                it = fdmap.erase(it);
            } else {
                near_expire = std::min(near_expire, exp);
                it++;
=======
        ISocketStream* stream;
        if (head->single()) {
            head->_refcnt++;
            stream = connector();
            if (!stream) {
                if (--head->_refcnt == 0) {
                    // Connectors may yield and other calls can prepend keys or
                    // pin this head. Unlink only after its final reference drops.
                    ERRNO err;
                    auto link = &_key_head;
                    while (*link != head) link = &(*link)->_key_next;
                    *link = head->_key_next;
                    // Erase by key: a rehash during connector() invalidates
                    // iterators. This builds a temporary key, as emplace() did.
                    sockmap.erase(head->key());
                    errno = err.no;
                }
                return nullptr;
>>>>>>> 7a5a1ea (fix(net): reclaim failed pool routes safely across reentrant callbacks (#1681))
            }
        }
        for (auto node : freelist) {
            rm_watch(node);
        }
        freelist.delete_all();
        assert(near_expire > now);
        return sat_sub(near_expire, now);
    }

    bool release(const EndPoint& ep, ISocketStream* stream) {
        auto fd = stream->get_underlay_fd();
        ERRNO err;
        if (!stream_reusable(fd)) return false;
        auto node = new StreamListNode(ep, stream, fd, TTL_us);
        push_into_pool(node);
        errno = err.no;
<<<<<<< HEAD
        return true;
=======
    }

    Timeout check_expire_heartbeat() {
        Timeout near_expire(args.expiration);
        for (auto h = _key_head; h; h = h->_key_next) {
            // Stream destructors and heartbeats can yield or reenter connect().
            // Pin the head and always restart expiration from its first node,
            // so a destructor may remove another node without invalidating a
            // retained traversal pointer.
            ++h->_refcnt;
            while (!h->single()) {
                auto ptr = h->next();
                if (now < ptr->timeout.expiration()) {
                    near_expire = std::min(near_expire, ptr->timeout);
                    break;
                }
                ptr->remove_from_list();
                rm_watch(ptr);
                assert(h->_refcnt > 1); // the node and the pin
                h->_refcnt--;
                delete ptr;
            }
            if (args.heartbeater) {
                // Take one node at a time and keep only the pinned head and a
                // counter across the callback, so the remaining nodes stay
                // reusable by connect(). Heartbeated nodes go to the tail; the
                // budget bounds the round.
                uint32_t budget = 0;
                for (auto p = h->next(); p != h; p = p->next()) ++budget;
                while (budget-- && !h->single()) {
                    auto ptr = h->next();
                    ptr->remove_from_list();
                    rm_watch(ptr);
                    near_expire = std::min(near_expire, ptr->timeout);
                    if (args.heartbeater(ptr->stream.get()) == 0) {
                        h->insert_before(ptr);
                        add_watch(ptr->stream->get_underlay_fd(), ptr);
                        continue;
                    }
                    assert(h->_refcnt > 1); // the node and the pin
                    h->_refcnt--;
                    delete ptr;
                }
            }
            --h->_refcnt;
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
>>>>>>> 7a5a1ea (fix(net): reclaim failed pool routes safely across reentrant callbacks (#1681))
    }

    void collect() {
        StreamListNode* nodes[16];
        while (collector) {
            auto ret = ev->wait_for_events((void**)nodes, 16, -1UL);
            for (int i = 0; i < ret; i++) {
                // since destructed socket should never become readable before
                // it have been acquired again
                // if it is readable or RDHUP, both condition should treat as
                // socket shutdown
                drop_from_pool(nodes[i]);
            }
            for (int i = 0; i < ret; i++) delete nodes[i];
        }
    }
};

PooledTCPSocketStream::~PooledTCPSocketStream() {
    if (drop || !pool->release(ep, m_underlay)) {
        delete m_underlay;
    }
}

extern "C" ISocketClient* new_tcp_socket_pool(ISocketClient* client, uint64_t TTL_us, bool client_ownership) {
    return new TCPSocketPool(client, TTL_us, client_ownership);
}

}
}
