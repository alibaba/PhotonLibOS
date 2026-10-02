/*
Copyright 2026 The Photon Authors

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

#include <memory>
#include <vector>
#include <photon/common/memory-stream/memory-stream.h>
#include <photon/photon.h>
#include "../../test/gtest.h"

// Inspect private key retention and exact connector/collector interleavings
// without adding observability or test hooks to the public socket-pool API.
#include "../pooled_socket.cpp"

using namespace photon;
using namespace photon::net;

TEST(socket_pool_failure, unique_large_failed_keys_are_reclaimed_immediately) {
    TCPSocketPool pool(SocketPoolArgs{});
    std::string key(64000, 'k');
    for (int i = 0; i < 128; ++i) {
        auto prefix = std::to_string(i);
        key.replace(0, prefix.size(), prefix);
        EXPECT_EQ(nullptr, pool.connect(key, []() -> ISocketStream* {
            errno = EHOSTUNREACH;
            return nullptr;
        }));
        EXPECT_EQ(EHOSTUNREACH, errno);
        EXPECT_TRUE(pool.sockmap.empty());
        EXPECT_EQ(nullptr, pool._key_head);
    }
}

TEST(socket_pool_failure, same_key_inflight_connection_keeps_its_head) {
    TCPSocketPool pool(SocketPoolArgs{});
    semaphore started(0), resume(0);
    ISocketStream* first = nullptr;
    auto worker = thread_enable_join(thread_create11([&] {
        first = pool.connect("shared", [&]() -> ISocketStream* {
            started.signal(1);
            resume.wait(1);
            return new_string_socket_stream();
        });
    }));
    DEFER({ if (worker) { resume.signal(1); thread_join(worker); } });
    ASSERT_EQ(0, started.wait(1, 5ULL * 1000 * 1000));
    auto head = pool._key_head;
    EXPECT_EQ(nullptr, pool.connect("shared", []() -> ISocketStream* {
        errno = ECONNREFUSED;
        return nullptr;
    }));
    ASSERT_EQ(1U, pool.sockmap.size());
    EXPECT_EQ(head, pool._key_head);
    EXPECT_EQ(1U, head->_refcnt);
    resume.signal(1);
    thread_join(worker);
    worker = nullptr;
    ASSERT_NE(nullptr, first);
    delete first;
    int connectors = 0;
    std::unique_ptr<ISocketStream> reused(pool.connect("shared", [&]() -> ISocketStream* {
        ++connectors;
        return nullptr;
    }));
    ASSERT_NE(nullptr, reused);
    EXPECT_EQ(0, connectors);
    reused->close();
    reused.reset();
    EXPECT_EQ(nullptr, pool.connect("shared", []() -> ISocketStream* { return nullptr; }));
    EXPECT_TRUE(pool.sockmap.empty());
    EXPECT_EQ(nullptr, pool._key_head);
}

TEST(socket_pool_failure, failed_connector_unlinks_after_rehash_and_other_keys) {
    TCPSocketPool pool(SocketPoolArgs{});
    semaphore started(0), resume(0);
    auto worker = thread_enable_join(thread_create11([&] {
        EXPECT_EQ(nullptr, pool.connect("pending", [&]() -> ISocketStream* {
            started.signal(1);
            resume.wait(1);
            errno = ENOENT;
            return nullptr;
        }));
        EXPECT_EQ(ENOENT, errno);
    }));
    DEFER({ if (worker) { resume.signal(1); thread_join(worker); } });
    ASSERT_EQ(0, started.wait(1, 5ULL * 1000 * 1000));
    std::vector<std::unique_ptr<ISocketStream>> streams;
    for (int i = 0; i < 32; ++i) {
        streams.emplace_back(pool.connect(std::to_string(i), []() -> ISocketStream* {
            return new_string_socket_stream();
        }));
        ASSERT_NE(nullptr, streams.back());
    }
    // The second same-key failure cannot erase the first connector's pin.
    EXPECT_EQ(nullptr, pool.connect("pending", []() -> ISocketStream* { return nullptr; }));
    EXPECT_EQ(33U, pool.sockmap.size());
    resume.signal(1);
    thread_join(worker);
    worker = nullptr;
    EXPECT_EQ(32U, pool.sockmap.size());
    size_t linked = 0;
    for (auto head = pool._key_head; head; head = head->_key_next) ++linked;
    EXPECT_EQ(32U, linked);
    for (int i = 0; i < 32; ++i) {
        streams[i]->close();
        streams[i].reset();
        EXPECT_EQ(nullptr, pool.connect(std::to_string(i), []() -> ISocketStream* { return nullptr; }));
    }
    EXPECT_TRUE(pool.sockmap.empty());
    EXPECT_EQ(nullptr, pool._key_head);
}

class ReentrantStream : public ForwardSocketStream {
public:
    Delegate<void> onDestroy;
    explicit ReentrantStream(Delegate<void> onDestroy)
        : ForwardSocketStream(new_string_socket_stream(), true), onDestroy(onDestroy) {}
    ~ReentrantStream() override { onDestroy(); }
    int close() override { return m_underlay->close(); }
    ssize_t read(void* buf, size_t count) override { return m_underlay->read(buf, count); }
    ssize_t readv(const iovec* iov, int count) override { return m_underlay->readv(iov, count); }
    ssize_t write(const void* buf, size_t count) override { return m_underlay->write(buf, count); }
    ssize_t writev(const iovec* iov, int count) override { return m_underlay->writev(iov, count); }
    ssize_t recv(void* buf, size_t count, int flags = 0) override {
        return m_underlay->recv(buf, count, flags);
    }
    ssize_t recv(const iovec* iov, int count, int flags = 0) override {
        return m_underlay->recv(iov, count, flags);
    }
    ssize_t send(const void* buf, size_t count, int flags = 0) override {
        return m_underlay->send(buf, count, flags);
    }
    ssize_t send(const iovec* iov, int count, int flags = 0) override {
        return m_underlay->send(iov, count, flags);
    }
    ssize_t sendfile(int fd, off_t offset, size_t count) override {
        return m_underlay->sendfile(fd, offset, count);
    }
};

TEST(socket_pool_failure, collector_pins_head_during_reentrant_destruction) {
    SocketPoolArgs args;
    args.expiration = 1000;
    TCPSocketPool pool(args);
    int destroyed = 0;
    auto onDestroy = [&] {
        ++destroyed;
        EXPECT_EQ(nullptr, pool.connect("expiring", []() -> ISocketStream* { return nullptr; }));
        ASSERT_EQ(1U, pool.sockmap.size());
        EXPECT_EQ(1U, pool._key_head->_refcnt);
    };
    auto stream = pool.connect("expiring", [&]() -> ISocketStream* {
        return new ReentrantStream(onDestroy);
    });
    ASSERT_NE(nullptr, stream);
    delete stream;
    pool._key_head->next()->timeout.timeout(0);
    pool.check_expire_heartbeat();
    EXPECT_EQ(1, destroyed);
    EXPECT_TRUE(pool.sockmap.empty());
    EXPECT_EQ(nullptr, pool._key_head);
}

TEST(socket_pool_failure, collector_restarts_after_reentrant_destruction) {
    SocketPoolArgs args;
    args.expiration = 1000;
    TCPSocketPool pool(args);
    int reentries = 0;
    auto onDestroy = [&] {
        ++reentries;
        auto grabbed = pool.connect("expiring", []() -> ISocketStream* {
            return nullptr;
        });
        EXPECT_NE(nullptr, grabbed);
        if (grabbed) {
            grabbed->close();
            delete grabbed;
        }
    };
    auto first = pool.connect("expiring", [&]() -> ISocketStream* {
        return new ReentrantStream(onDestroy);
    });
    auto second = pool.connect("expiring", []() -> ISocketStream* {
        return new_string_socket_stream();
    });
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    delete first;
    delete second;
    ASSERT_EQ(2U, pool._key_head->_refcnt);
    pool._key_head->next()->timeout.timeout(0);
    pool._key_head->next()->next()->timeout.timeout(0);
    pool.check_expire_heartbeat();
    EXPECT_EQ(1, reentries);
    EXPECT_TRUE(pool.sockmap.empty());
    EXPECT_EQ(nullptr, pool._key_head);
}

TEST(socket_pool_failure, collector_detaches_batch_before_reentrant_heartbeat) {
    TCPSocketPool* poolPtr = nullptr;
    int heartbeats = 0;
    int connectors = 0;
    auto heartbeater = [&](ISocketStream*) -> int {
        ++heartbeats;
        if (heartbeats != 1) return 0;
        auto grabbed = poolPtr->connect("heartbeat", [&]() -> ISocketStream* {
            ++connectors;
            return nullptr;
        });
        EXPECT_EQ(nullptr, grabbed);
        if (grabbed) {
            grabbed->close();
            delete grabbed;
        }
        return 0;
    };
    SocketPoolArgs args;
    args.heartbeater = heartbeater;
    TCPSocketPool pool(args);
    poolPtr = &pool;
    auto first = pool.connect("heartbeat", []() -> ISocketStream* {
        return new_string_socket_stream();
    });
    auto second = pool.connect("heartbeat", []() -> ISocketStream* {
        return new_string_socket_stream();
    });
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    delete first;
    delete second;
    pool.check_expire_heartbeat();
    EXPECT_EQ(2, heartbeats);
    EXPECT_EQ(1, connectors);
    ASSERT_EQ(1U, pool.sockmap.size());
    ASSERT_NE(nullptr, pool._key_head);
    EXPECT_EQ(2U, pool._key_head->_refcnt);
    size_t idle = 0;
    for (auto node = pool._key_head->next(); node != pool._key_head;
         node = node->next())
        ++idle;
    EXPECT_EQ(2U, idle);
}

int main(int argc, char** argv) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE)) return 1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
