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
    Timeout expire;

    StreamListNode(EndPoint key, ISocketStream* stream, int fd, uint64_t expire)
        : key(key), stream(stream), fd(fd), expire(expire) {
    }
};

class TCPSocketPool : public ForwardSocketClient {
protected:
    CascadingEventEngine* ev;
    photon::thread* collector;
    std::unordered_map<EndPoint, intrusive_list<StreamListNode>> fdmap;
    uint64_t expiration;
    photon::Timer timer;

    // all fd < 0 treated as socket not based on fd
    // and always alive. Using such socket needs user
    // to check if connected socket is still usable.
    bool stream_alive(int fd) {
        return (fd < 0) || (wait_for_fd_readable(fd, 0) != 0);
    }

    void add_watch(StreamListNode* node) {
        if (node->fd >= 0) ev->add_interest({node->fd, EVENT_READ, node});
    }

    void rm_watch(StreamListNode* node) {
        if (node->fd >= 0) ev->rm_interest({node->fd, EVENT_READ, node});
    }

    ISocketStream* get_from_pool(EndPoint ep) {
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
        delete node;
    }

public:
    TCPSocketPool(ISocketClient* client, uint64_t expiration,
                  bool client_ownership = false)
        : ForwardSocketClient(client, client_ownership),
          ev(photon::new_default_cascading_engine()),
          expiration(expiration),
          timer(expiration, {this, &TCPSocketPool::evict}) {
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

    ISocketStream* connect(EndPoint remote,
                           EndPoint local = EndPoint()) override {
    again:
        auto stream = get_from_pool(remote);
        if (!stream) {
            stream = m_underlay->connect(remote, local);
            if (!stream) return nullptr;
        } else if (!stream_alive(stream->get_underlay_fd())) {
            delete stream;
            goto again;
        }
        return new PooledTCPSocketStream(stream, this, remote);
    }
    uint64_t evict() {
        // remove empty entry in fdmap
        intrusive_list<StreamListNode> freelist;
        uint64_t near_expire = expiration;
        for (auto it = fdmap.begin(); it != fdmap.end();) {
            auto& list = it->second;
            while (!list.empty() &&
                   list.front()->expire.expire() < photon::now) {
                freelist.push_back(list.pop_front());
            }
            if (it->second.empty()) {
                it = fdmap.erase(it);
            } else {
                near_expire =
                    std::min(near_expire, it->second.front()->expire.timeout());
                it++;
            }
        }
        for (auto node : freelist) {
            rm_watch(node);
        }
        freelist.delete_all();
        return near_expire;
    }

    bool release(EndPoint ep, ISocketStream* stream) {
        auto fd = stream->get_underlay_fd();
        if (!stream_alive(fd)) return false;
        auto node = new StreamListNode(ep, stream, fd, expiration);
        push_into_pool(node);
        return true;
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
        }
    }
};

PooledTCPSocketStream::~PooledTCPSocketStream() {
    if (drop || !pool->release(ep, m_underlay)) {
        delete m_underlay;
    }
}

extern "C" ISocketClient* new_tcp_socket_pool(ISocketClient* client, uint64_t expire, bool client_ownership) {
    return new TCPSocketPool(client, expire, client_ownership);
}

}
}
