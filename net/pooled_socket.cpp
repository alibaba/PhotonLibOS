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

#define FORWARD_SOCK_ACT(how, action, count)                            \
    if (count == 0) return 0;                                           \
    auto ret = m_underlay->action;                                      \
    if (ret < 0 || (ShutdownHow::Read == ShutdownHow::how && ret == 0)) \
        drop = true;                                                    \
    return ret

    int close() override {
        drop = true;
        m_underlay->close();
        return 0;
    }
    ssize_t read(void* buf, size_t count) override {
        FORWARD_SOCK_ACT(Read, read(buf, count), count);
    }
    ssize_t write(const void* buf, size_t count) override {
        FORWARD_SOCK_ACT(Write, write(buf, count), count);
    }
    ssize_t readv(const struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Read, readv(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t readv_mutable(struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Read, readv_mutable(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t writev(const struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Write, writev(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t writev_mutable(struct iovec* iov, int iovcnt) override {
        FORWARD_SOCK_ACT(Write, writev_mutable(iov, iovcnt),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t recv(void* buf, size_t count, int flags = 0) override {
        FORWARD_SOCK_ACT(Read, recv(buf, count, flags), count);
    }
    ssize_t recv(const struct iovec* iov, int iovcnt, int flags = 0) override {
        FORWARD_SOCK_ACT(Read, recv(iov, iovcnt, flags),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t send(const void* buf, size_t count, int flags = 0) override {
        FORWARD_SOCK_ACT(Write, send(buf, count, flags), count);
    }
    ssize_t send(const struct iovec* iov, int iovcnt, int flags = 0) override {
        FORWARD_SOCK_ACT(Write, send(iov, iovcnt, flags),
                         iovector_view((struct iovec*)iov, iovcnt).sum());
    }
    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        FORWARD_SOCK_ACT(Write, sendfile(in_fd, offset, count), count);
    }

#undef FORWARD_SOCK_ACT
};

struct StreamListNode : public intrusive_list_node<StreamListNode> {
    EndPoint key;
    std::unique_ptr<ISocketStream> stream;
    Timeout expire;

    StreamListNode() : expire(0) {}
    StreamListNode(EndPoint key, ISocketStream* stream, uint64_t expire)
            : key(key), stream(stream), expire(expire) {}
};

class TCPSocketPool : public ForwardSocketClient {
protected:
    CascadingEventEngine* ev;
    photon::thread* collector;
    std::unordered_multimap<EndPoint, StreamListNode*> fdmap;
    std::unordered_map<int, EndPoint> fdep;
    intrusive_list<StreamListNode> lru;
    uint64_t expiration;
    photon::Timer timer;

public:
    TCPSocketPool(ISocketClient* client, uint64_t expiration, bool client_ownership = false)
            : ForwardSocketClient(client, client_ownership),
              ev(photon::new_default_cascading_engine()),
              expiration(expiration),
              timer(0, {this, &TCPSocketPool::evict}) {
        collector = (photon::thread*) photon::thread_enable_join(
                photon::thread_create11(&TCPSocketPool::collect, this));
    }

    ~TCPSocketPool() override {
        timer.stop();
        auto th = collector;
        collector = nullptr;
        photon::thread_interrupt((photon::thread*)th);
        photon::thread_join((photon::join_handle*)th);
        for (auto fe : fdep) {
            ev->rm_interest(
                    {(int)fe.first, EVENT_READ, (void*)(uint64_t)fe.first});
        }
        while (!lru.empty()) {
            auto node = lru.pop_front();
            delete node;
        }
        delete ev;
    }

    ISocketStream* connect(const char* path, size_t count) override {
        LOG_ERROR_RETURN(ENOSYS, nullptr,
                         "Socket pool supports TCP-like socket only");
    }

    ISocketStream* connect(EndPoint remote, EndPoint local = EndPoint()) override {
        return do_connect(remote, [&] { return m_underlay->connect(remote, local); });
    }

    void drop_from_pool(int fd) {
        // remove fd interest
        ev->rm_interest({(int)fd, EVENT_READ, (void*)(int64_t)fd});
        // find fdep & fdmap and remove
        auto it = fdep.find(fd);
        if (it != fdep.end()) {
            auto ep = it->second;
            for (auto map_it = fdmap.find(ep);
                 map_it != fdmap.end() && map_it->first == ep; map_it++) {
                if (map_it->second->stream->get_underlay_fd() == fd) {
                    auto node = map_it->second;
                    fdmap.erase(map_it);
                    lru.erase(node);
                    delete node;
                    break;
                }
            }
            fdep.erase(it);
        }
    }

    void drop_from_pool(StreamListNode* node) {
        // remove fd interest
        auto fd = node->stream->get_underlay_fd();
        if (fd >= 0) return drop_from_pool(fd);
        // or node have no record
        for (auto map_it = fdmap.find(node->key);
             map_it != fdmap.end() && map_it->first == node->key; map_it++) {
            if (map_it->second == node) {
                fdmap.erase(map_it);
                lru.erase(node);
                break;
            }
        }
        delete node;
    }

    uint64_t evict() {
        photon::thread_yield_to(photon::CURRENT);
        auto evict_cnt = fdmap.size() >> 1;
        auto cnt = 0;
        while (!lru.empty() && evict_cnt &&
               lru.front()->expire.expire() <= photon::now) {
            auto node = lru.front();
            drop_from_pool(node);
            evict_cnt--;
            cnt++;
        }
        if (lru.empty() || cnt) return expiration;
        return lru.front()->expire.timeout();
    }

    bool release(EndPoint ep, ISocketStream* stream) {
        auto fd = stream->get_underlay_fd();
        if (fd >= 0) {
            // able to fetch fd
            // check by epoll
            if (wait_for_fd_readable(fd, 0) == 0) {
                LOG_TEMP("refuse put in pool");
                return false;
            }
            ev->add_interest({fd, EVENT_READ, (void*)(uint64_t)fd});
            fdep.emplace(fd, ep);
        }
        // stream back to pool
        photon::thread_yield_to(photon::CURRENT);
        auto node = new StreamListNode(ep, stream, expiration);
        fdmap.emplace(ep, node);
        lru.push_back(node);
        return true;
    }

    void collect() {
        int64_t fds[16];
        while (collector) {
            auto ret = ev->wait_for_events((void**)fds, 16, -1UL);
            for (int i = 0; i < ret; i++) {
                // since destructed socket should never become readable before
                // it have been acquired again
                // if it is readable or RDHUP, both condition should treat as
                // socket shutdown
                auto fd = fds[i];
                drop_from_pool(fd);
            }
        }
    }

private:
    template<typename SockCTOR>
    ISocketStream* do_connect(EndPoint key, SockCTOR ctor) {
again:
        auto it = fdmap.find(key);
        if (it == fdmap.end()) {
            ISocketStream* sock = ctor();
            if (sock) {
                return new PooledTCPSocketStream(sock, this, key);
            }
            return nullptr;
        } else {
            auto fd = it->second->stream->get_underlay_fd();
            if (fd >= 0) {
                fdep.erase(fd);
                ev->rm_interest({fd, EVENT_READ, (void*) (uint64_t) fd});
            }
            auto node = it->second;
            fdmap.erase(it);
            lru.erase(node);
            if (fd >= 0 && wait_for_fd_readable(fd, 0) == 0) {
                LOG_TEMP("destruct node");
                delete node;
                goto again;
            }
            auto ret = new PooledTCPSocketStream(node->stream.release(), this, key);
            delete node;
            return ret;
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
