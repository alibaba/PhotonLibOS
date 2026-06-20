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

#include <string>
#include <unordered_map>

#include <photon/common/alog.h>
#include <photon/common/iovector.h>
#include <photon/common/timeout.h>
#include <photon/io/fd-events.h>
#include <photon/net/socket.h>
#include <photon/thread/list.h>
#include <photon/thread/thread11.h>
#include <photon/thread/timer.h>

#include "base_socket.h"

namespace photon {
namespace net {

template <typename Key, typename Hash = std::hash<Key>>
class StreamPoolBase {
protected:
    struct StreamNode : public intrusive_list_node<StreamNode> {
        Key key;
        std::unique_ptr<ISocketStream> stream;
        int fd;
        Timeout timeout;

        StreamNode(const Key& key, ISocketStream* stream, int fd, uint64_t ttl_us)
            : key(key), stream(stream), fd(fd), timeout(ttl_us) {}
    };

    CascadingEventEngine* m_ev;
    photon::thread* m_collector;
    std::unordered_map<Key, intrusive_list<StreamNode>, Hash> m_pool;
    uint64_t m_ttl_us;
    photon::Timer m_timer;

    bool stream_reusable(int fd) {
        return (fd < 0) || (wait_for_fd_readable(fd, 0) != 0);
    }

    void add_watch(StreamNode* node) {
        if (node->fd >= 0) m_ev->add_interest({node->fd, EVENT_READ, node});
    }

    void rm_watch(StreamNode* node) {
        if (node->fd >= 0) m_ev->rm_interest({node->fd, EVENT_READ, node});
    }

    uint64_t evict() {
        intrusive_list<StreamNode> freelist;
        uint64_t near_expire = m_ttl_us + now;
        for (auto it = m_pool.begin(); it != m_pool.end();) {
            auto& list = it->second;
            uint64_t exp;
            while (!list.empty() && now >=
                   (exp = list.front()->timeout.expiration())) {
                freelist.push_back(list.pop_front());
            }
            if (list.empty()) {
                it = m_pool.erase(it);
            } else {
                near_expire = std::min(near_expire, exp);
                it++;
            }
        }
        for (auto node : freelist)
            rm_watch(node);
        freelist.delete_all();
        assert(near_expire > now);
        return sat_sub(near_expire, now);
    }

    void collect() {
        StreamNode* nodes[16];
        while (m_collector) {
            auto ret = m_ev->wait_for_events((void**)nodes, 16, -1UL);
            for (int i = 0; i < ret; i++) {
                auto it = m_pool.find(nodes[i]->key);
                if (it != m_pool.end()) {
                    it->second.erase(nodes[i]);
                    if (it->second.empty()) m_pool.erase(it);
                }
            }
            for (int i = 0; i < ret; i++) delete nodes[i];
        }
    }

private:
    // Wraps a stream and returns it to the pool on destruction.
    // Subclasses never see this type — acquire() returns ISocketStream*.
    class PooledStream : public ForwardSocketStream {
    public:
        StreamPoolBase* pool;
        Key key;
        bool drop;

        PooledStream(ISocketStream* stream, StreamPoolBase* pool, const Key& key)
            : ForwardSocketStream(stream, false), pool(pool), key(key), drop(false) {}
        ~PooledStream() override {
            if (drop || !pool->release(key, m_underlay))
                delete m_underlay;
        }
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

public:
    StreamPoolBase(uint64_t ttl_us = -1UL)
        : m_ev(photon::new_default_cascading_engine()),
          m_collector(nullptr),
          m_ttl_us(ttl_us),
          m_timer(ttl_us, {this, &StreamPoolBase::evict}) {
        m_collector = (photon::thread*)photon::thread_enable_join(
            photon::thread_create11(&StreamPoolBase::collect, this));
    }

    ~StreamPoolBase() {
        m_timer.stop();
        auto th = m_collector;
        m_collector = nullptr;
        photon::thread_interrupt((photon::thread*)th);
        photon::thread_join((photon::join_handle*)th);
        for (auto& kv : m_pool)
            kv.second.delete_all();
        delete m_ev;
    }

    // Return a stream to the pool.  Called from PooledStream's destructor.
    // Returns true if accepted back, false if the stream is unhealthy
    // (caller must delete it).
    bool release(const Key& key, ISocketStream* stream) {
        auto fd = stream->get_underlay_fd();
        ERRNO err;
        if (!stream_reusable(fd)) return false;
        auto node = new StreamNode(key, stream, fd, m_ttl_us);
        m_pool[key].push_back(node);
        add_watch(node);
        errno = err.no;
        return true;
    }

    // Acquire a pooled stream: cache lookup → health check → factory fallback → wrap.
    // Factory is called when cache misses or all cached entries are stale.
    template <typename Factory>
    ISocketStream* acquire(const Key& key, Factory&& factory) {
        // Try cache: drain stale entries, then use the first reusable one.
        auto it = m_pool.find(key);
        if (it != m_pool.end()) {
            auto& list = it->second;
            while (!list.empty()) {
                auto node = list.pop_front();
                rm_watch(node);
                int fd = node->stream->get_underlay_fd();
                if (stream_reusable(fd)) {
                    auto* s = node->stream.release();
                    if (list.empty()) m_pool.erase(it);
                    delete node;
                    return new PooledStream(s, this, key);
                }
                delete node;
            }
            m_pool.erase(it);
        }
        // Cache miss: create a new stream via the factory.
        auto* stream = factory();
        if (!stream) return nullptr;
        return new PooledStream(stream, this, key);
    }
};

}  // namespace net
}  // namespace photon
