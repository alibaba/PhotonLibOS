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

#include <atomic>
#include <memory>
#include <thread>
#include <unistd.h>

#include <photon/common/memory-stream/memory-stream.h>
#include <photon/common/utility.h>
#include <photon/net/http/dialer.h>
#include <photon/net/security-context/tls-stream.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include "../../../test/gtest.h"

using namespace photon;
using namespace photon::net;
using namespace photon::net::http;

namespace {

class StreamDialer : public IDialer {
public:
    int calls = 0;
    int* destroyed = nullptr;
    std::string input;
    StringSocketStream* last = nullptr;

    explicit StreamDialer(int* destroyed = nullptr) : destroyed(destroyed) {}
    ~StreamDialer() override {
        if (destroyed) ++*destroyed;
    }

    ISocketStream* dial(const DialTarget&, uint64_t) override {
        ++calls;
        last = new_string_socket_stream();
        last->set_input(input);
        return last;
    }
};

class TrackedStream : public ISocketStream {
public:
    explicit TrackedStream(int* destroyed)
        : inner(new_string_socket_stream()), destroyed(destroyed) {}
    ~TrackedStream() override { ++*destroyed; }

    int close() override { return inner->close(); }
    ssize_t read(void* buf, size_t count) override {
        return inner->read(buf, count);
    }
    ssize_t readv(const iovec* iov, int iovcnt) override {
        return inner->readv(iov, iovcnt);
    }
    ssize_t write(const void* buf, size_t count) override {
        return inner->write(buf, count);
    }
    ssize_t writev(const iovec* iov, int iovcnt) override {
        return inner->writev(iov, iovcnt);
    }
    uint64_t timeout() const override { return timeout_value; }
    void timeout(uint64_t value) override { timeout_value = value; }
    ssize_t recv(void* buf, size_t count, int flags = 0) override {
        return inner->recv(buf, count, flags);
    }
    ssize_t recv(const iovec* iov, int iovcnt, int flags = 0) override {
        return inner->recv(iov, iovcnt, flags);
    }
    ssize_t send(const void* buf, size_t count, int flags = 0) override {
        return inner->send(buf, count, flags);
    }
    ssize_t send(const iovec* iov, int iovcnt, int flags = 0) override {
        return inner->send(iov, iovcnt, flags);
    }
    ssize_t sendfile(int in_fd, off_t offset, size_t count) override {
        return inner->sendfile(in_fd, offset, count);
    }
    Object* get_underlay_object(uint64_t recursion = 0) override {
        return inner->get_underlay_object(recursion);
    }
    int setsockopt(int level, int name, const void* value,
                   socklen_t length) override {
        return inner->setsockopt(level, name, value, length);
    }
    int getsockopt(int level, int name, void* value,
                   socklen_t* length) override {
        return inner->getsockopt(level, name, value, length);
    }
    int getsockname(EndPoint& address) override {
        return inner->getsockname(address);
    }
    int getpeername(EndPoint& address) override {
        return inner->getpeername(address);
    }
    int getsockname(char* path, size_t count) override {
        return inner->getsockname(path, count);
    }
    int getpeername(char* path, size_t count) override {
        return inner->getpeername(path, count);
    }

public:
    std::unique_ptr<StringSocketStream> inner;
    int* destroyed;
    uint64_t timeout_value = -1ULL;
};

class FailingStreamDialer : public IDialer {
public:
    explicit FailingStreamDialer(int* destroyed) : destroyed(destroyed) {}

    ISocketStream* dial(const DialTarget&, uint64_t) override {
        return new TrackedStream(destroyed);
    }

public:
    int* destroyed;
};

struct LocalCounters {
    std::atomic<int> attempts{0};
    std::atomic<int> created{0};
    std::atomic<int> destroyed{0};
    std::atomic<int> calls{0};
    std::atomic<int> wrong_vcpu_destructions{0};
    std::atomic<bool> fail{false};
};

class LocalDialer : public IDialer {
public:
    LocalCounters* state;
    vcpu_base* owner;

    explicit LocalDialer(LocalCounters* state)
        : state(state), owner(photon::get_vcpu()) {}
    ~LocalDialer() override {
        if (owner != photon::get_vcpu())
            state->wrong_vcpu_destructions.fetch_add(1, std::memory_order_relaxed);
        state->destroyed.fetch_add(1, std::memory_order_relaxed);
    }
    ISocketStream* dial(const DialTarget&, uint64_t) override {
        state->calls.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
};

struct LocalState : LocalCounters {
    IDialer* make() {
        attempts.fetch_add(1, std::memory_order_relaxed);
        if (fail.load(std::memory_order_relaxed)) return nullptr;
        created.fetch_add(1, std::memory_order_relaxed);
        return new LocalDialer(this);
    }
};

} // namespace

TEST(dialer, conditional_layers_passthrough) {
    std::unique_ptr<TLSContext> context(
        new_tls_context(nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, context);
    context->set_verify_mode(VerifyMode::NONE);

    DialTarget direct;
    direct.host = "origin.example";
    direct.port = 80;

    StreamDialer tls_underlay;
    std::unique_ptr<IDialer> origin_tls(new_tls_dialer(
        context.get(), &tls_underlay, TLSLayer::ORIGIN));
    ASSERT_NE(nullptr, origin_tls);
    auto stream = origin_tls->dial(direct);
    ASSERT_EQ(tls_underlay.last, stream);
    delete stream;

    StreamDialer proxy_tls_underlay;
    std::unique_ptr<IDialer> proxy_tls(new_tls_dialer(
        context.get(), &proxy_tls_underlay, TLSLayer::PROXY));
    ASSERT_NE(nullptr, proxy_tls);
    stream = proxy_tls->dial(direct);
    ASSERT_EQ(proxy_tls_underlay.last, stream);
    delete stream;

    StreamDialer tunnel_underlay;
    std::unique_ptr<IDialer> tunnel(
        new_connect_tunnel_dialer(&tunnel_underlay));
    ASSERT_NE(nullptr, tunnel);
    stream = tunnel->dial(direct);
    ASSERT_EQ(tunnel_underlay.last, stream);
    delete stream;

    DialTarget uds = direct;
    uds.secure = true;
    uds.uds_path = std::string_view("\0abstract", 9);
    StreamDialer uds_underlay;
    std::unique_ptr<IDialer> uds_tls(new_tls_dialer(
        context.get(), &uds_underlay, TLSLayer::ORIGIN));
    ASSERT_NE(nullptr, uds_tls);
    stream = uds_tls->dial(uds);
    ASSERT_EQ(uds_underlay.last, stream);
    delete stream;
}

TEST(dialer, tunnel_precedes_origin_tls) {
    std::unique_ptr<TLSContext> context(
        new_tls_context(nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, context);
    context->set_verify_mode(VerifyMode::NONE);

    StreamDialer transport;
    transport.input = "HTTP/1.1 200 Connection Established\r\n\r\n";
    std::unique_ptr<IDialer> tunnel(
        new_connect_tunnel_dialer(&transport));
    ASSERT_NE(nullptr, tunnel);
    std::unique_ptr<IDialer> origin_tls(new_tls_dialer(
        context.get(), tunnel.get(), TLSLayer::ORIGIN));
    ASSERT_NE(nullptr, origin_tls);

    DialTarget target;
    target.host = "origin.example";
    target.port = 443;
    target.secure = true;
    target.proxy_host = "proxy.example";
    target.proxy_port = 8080;

    auto stream = origin_tls->dial(target);
    ASSERT_NE(nullptr, stream);
    EXPECT_NE(transport.last, stream);
    EXPECT_EQ(1, transport.calls);
    EXPECT_EQ(0U, transport.last->input().size());
    EXPECT_FALSE(transport.last->output().empty());
    delete stream;
}

TEST(dialer, tunnel_failure_releases_stream) {
    int destroyed = 0;
    FailingStreamDialer underlay(&destroyed);
    std::unique_ptr<IDialer> tunnel(new_connect_tunnel_dialer(&underlay));
    ASSERT_NE(nullptr, tunnel);

    DialTarget target;
    target.host = "origin.example";
    target.port = 443;
    target.secure = true;
    target.proxy_host = "proxy.example";
    target.proxy_port = 8080;

    EXPECT_EQ(nullptr, tunnel->dial(target));
    EXPECT_EQ(1, destroyed);
}

TEST(dialer, tunnel_validates_response_status_line) {
    DialTarget target;
    target.host = "origin.example";
    target.port = 443;
    target.secure = true;
    target.proxy_host = "proxy.example";
    target.proxy_port = 8080;

    auto dial = [&](const std::string& response) {
        StreamDialer transport;
        transport.input = response + "\r\n\r\n";
        std::unique_ptr<IDialer> tunnel(
            new_connect_tunnel_dialer(&transport));
        return std::unique_ptr<ISocketStream>(tunnel->dial(target));
    };

    EXPECT_NE(nullptr, dial("HTTP/1.1 200 Connection Established"));
    EXPECT_NE(nullptr, dial("HTTP/1.0 204"));

    for (auto response : {"HTTP/1.1 2000 Invalid", "HTTP/1.1 20x Invalid",
                          "HTTP/1.x 200 Invalid", "HTTP/1.1200 Invalid",
                          "HTTP/1.1 200\tInvalid"}) {
        errno = 0;
        EXPECT_EQ(nullptr, dial(response)) << response;
        EXPECT_EQ(EPROTO, errno) << response;
    }

    errno = 0;
    EXPECT_EQ(nullptr, dial("HTTP/1.1 407 Proxy Authentication Required"));
    EXPECT_EQ(ECONNREFUSED, errno);
}

TEST(dialer, tunnel_distinguishes_eof_from_receive_errors) {
    struct ReceiveFailure : TrackedStream {
        int error;
        ReceiveFailure(int* destroyed, int error) : TrackedStream(destroyed), error(error) {}
        ssize_t recv(void*, size_t, int = 0) override {
            errno = error ? error : EAGAIN; // EOF deliberately leaves stale errno
            return error ? -1 : 0;
        }
    };
    struct Transport : IDialer {
        int error = 0, destroyed = 0;
        ISocketStream* dial(const DialTarget&, uint64_t) override {
            return new ReceiveFailure(&destroyed, error);
        }
    };
    for (int error : {0, ETIMEDOUT, ECONNRESET}) {
        Transport transport;
        transport.error = error;
        std::unique_ptr<IDialer> tunnel(new_connect_tunnel_dialer(&transport));
        DialTarget target;
        target.host = "origin.example";
        target.port = 443;
        target.secure = true;
        target.proxy_host = "proxy.example";
        target.proxy_port = 8080;
        EXPECT_EQ(nullptr, tunnel->dial(target));
        EXPECT_EQ(error ? error : ECONNRESET, errno);
        EXPECT_EQ(1, transport.destroyed);
    }
}

TEST(dialer, tunnel_preserves_duplicate_proxy_header_order) {
    StreamDialer transport;
    transport.input = "HTTP/1.1 200 Connection Established\r\n\r\n";
    std::unique_ptr<IDialer> tunnel(new_connect_tunnel_dialer(&transport));
    CommonHeaders<512> headers;
    ASSERT_EQ(0, headers.insert("X-Proxy-Z", "first"));
    ASSERT_EQ(0, headers.insert("X-Proxy-Repeat", "one", 1));
    ASSERT_EQ(0, headers.insert("x-proxy-repeat", "two", 1));
    ASSERT_EQ(0, headers.insert("X-Proxy-A", "last"));
    DialTarget target;
    target.host = "origin.example";
    target.port = 443;
    target.secure = true;
    target.proxy_host = "proxy.example";
    target.proxy_port = 8080;
    target.proxy_headers = &headers;
    target.proxy_auth = "Basic credentials";
    std::unique_ptr<ISocketStream> stream(tunnel->dial(target));
    ASSERT_NE(nullptr, stream);
    EXPECT_NE(std::string::npos, transport.last->output().find(
        headers.serialized().data(), 0, headers.serialized().size()));
    EXPECT_NE(std::string::npos, transport.last->output().find("Proxy-Authorization: Basic credentials\r\n"));
}

TEST(dialer, ownership_deletes_underlay) {
    int destroyed = 0;
    auto underlay = new StreamDialer(&destroyed);
    auto tunnel = new_connect_tunnel_dialer(underlay, true);
    ASSERT_NE(nullptr, tunnel);
    delete tunnel;
    EXPECT_EQ(1, destroyed);
}

TEST(dialer, pool_reuses_only_identical_routes) {
    int destroyed = 0;
    auto underlay = new StreamDialer(&destroyed);
    std::unique_ptr<IDialer> pool(new_pool_dialer(underlay, true));
    ASSERT_NE(nullptr, pool);

    DialTarget direct;
    direct.host = "origin.example";
    direct.port = 80;

    auto use = [&](const DialTarget& target) {
        auto stream = pool->dial(target);
        ASSERT_NE(nullptr, stream);
        delete stream;
    };

    use(direct);
    use(direct);
    EXPECT_EQ(1, underlay->calls);

    DialTarget secure = direct;
    secure.secure = true;
    secure.port = 443;
    use(secure);

    DialTarget forward = direct;
    forward.proxy_host = "proxy.example";
    forward.proxy_port = 8080;
    use(forward);

    DialTarget forward_other_origin = forward;
    forward_other_origin.host = "another.example";
    forward_other_origin.port = 81;
    use(forward_other_origin);
    EXPECT_EQ(3, underlay->calls);

    DialTarget forward_other_auth = forward;
    forward_other_auth.proxy_auth = "Basic other";
    use(forward_other_auth);

    DialTarget tunnel = secure;
    tunnel.proxy_host = "proxy.example";
    tunnel.proxy_port = 8080;
    use(tunnel);

    DialTarget tunnel_other_origin = tunnel;
    tunnel_other_origin.host = "another.example";
    use(tunnel_other_origin);

    const char abstract_path[] = "\0pool-key";
    DialTarget uds = direct;
    uds.uds_path = std::string_view(abstract_path, sizeof(abstract_path) - 1);
    use(uds);

    DialTarget uds_other_origin = uds;
    uds_other_origin.host = "another.example";
    use(uds_other_origin);

    EXPECT_EQ(8, underlay->calls);
    pool.reset();
    EXPECT_EQ(1, destroyed);
}

TEST(dialer, tunnel_pool_keys_effective_authorization) {
    StreamDialer transport;
    transport.input = "HTTP/1.1 200 Connection Established\r\n\r\n";
    std::unique_ptr<IDialer> pool(new_pool_dialer(
        new_connect_tunnel_dialer(&transport), true));
    CommonHeaders<512> headers;
    DialTarget target;
    target.host = "origin.example";
    target.port = 443;
    target.secure = true;
    target.proxy_host = "proxy.example";
    target.proxy_port = 8080;
    target.proxy_auth = "Basic fallback";
    target.proxy_headers = &headers;

    auto use = [&](const char* authorization) {
        headers.reset();
        ASSERT_EQ(0, headers.insert("pRoXy-AuThOrIzAtIoN", authorization));
        std::unique_ptr<ISocketStream> stream(pool->dial(target));
        ASSERT_NE(nullptr, stream);
    };
    use("Basic Alice");
    EXPECT_EQ(1, transport.calls);
    EXPECT_NE(std::string::npos, transport.last->output().find("Basic Alice\r\n"));
    use("Basic Alice");
    EXPECT_EQ(1, transport.calls);
    // A shadowed fallback is not the authenticated tunnel identity.
    target.proxy_auth = "Basic changed fallback";
    use("Basic Alice");
    EXPECT_EQ(1, transport.calls);
    use("Basic Bob");
    EXPECT_EQ(2, transport.calls);
    EXPECT_NE(std::string::npos, transport.last->output().find("Basic Bob\r\n"));
    use("Basic Bob");
    EXPECT_EQ(2, transport.calls);
    use(""); // a present empty field still overrides the fallback
    EXPECT_EQ(3, transport.calls);
    EXPECT_EQ(std::string::npos, transport.last->output().find("Basic changed fallback"));
    use("");
    EXPECT_EQ(3, transport.calls);
    headers.reset();
    std::unique_ptr<ISocketStream> fallback(pool->dial(target));
    ASSERT_NE(nullptr, fallback);
    EXPECT_EQ(4, transport.calls);
    EXPECT_NE(std::string::npos, transport.last->output().find("Basic changed fallback\r\n"));
}

TEST(dialer, pool_rejects_oversized_route_keys_before_connect) {
    auto underlay = new StreamDialer;
    std::unique_ptr<IDialer> pool(new_pool_dialer(underlay, true));
    ASSERT_NE(nullptr, pool);

    // "2/2/0/", a five-digit byte length plus ':', and "80/".
    constexpr size_t direct_key_overhead = 6 + 6 + 3;
    std::string host(UINT16_MAX - direct_key_overhead - 1, 'h');
    DialTarget target;
    target.host = host;
    target.port = 80;
    auto stream = pool->dial(target);
    ASSERT_NE(nullptr, stream); // largest key accepted by the socket pool
    delete stream;
    EXPECT_EQ(1, underlay->calls);

    auto reject = [&](const DialTarget& route) {
        errno = 0;
        std::unique_ptr<ISocketStream> result(pool->dial(route));
        EXPECT_EQ(nullptr, result);
        EXPECT_EQ(ENAMETOOLONG, errno);
        EXPECT_EQ(1, underlay->calls);
    };
    host.push_back('h');
    target.host = host;
    reject(target); // exactly UINT16_MAX bytes
    host.append(UINT16_MAX, 'h');
    target.host = host;
    reject(target); // a length that would wrap in uint16_t

    std::string oversized(UINT16_MAX, 'x');
    target.host = "origin.example";
    target.proxy_host = "proxy.example";
    target.proxy_port = 8080;
    target.proxy_auth = oversized;
    reject(target);
    oversized.back() = 'y';
    reject(target); // a different oversized credential is also rejected before lookup
    target.proxy_auth = {};
    target.proxy_pool_key = oversized;
    reject(target);
    target.secure = true;
    reject(target); // CONNECT route
    target.uds_path = oversized;
    reject(target);

    // Rejection leaves the previously pooled connection intact.
    host.resize(UINT16_MAX - direct_key_overhead - 1);
    DialTarget valid;
    valid.host = host;
    valid.port = 80;
    stream = pool->dial(valid);
    ASSERT_NE(nullptr, stream);
    delete stream;
    EXPECT_EQ(1, underlay->calls);
}

TEST(dialer, textual_key_fields_have_unambiguous_boundaries) {
    const char bytes[] = {'a', '\0', ':', '/', '2'};
    auto underlay = new StreamDialer;
    std::unique_ptr<IDialer> pool(new_pool_dialer(underlay, true));
    DialTarget route;
    route.proxy_host = "proxy";
    route.proxy_port = 8080;
    route.host = "origin";
    route.port = 80;
    auto use = [&]() {
        std::unique_ptr<ISocketStream> stream(pool->dial(route));
        ASSERT_NE(nullptr, stream);
    };
    route.proxy_auth = "a";
    route.proxy_pool_key = "bc";
    use();
    route.proxy_auth = "ab";
    route.proxy_pool_key = "c";
    use();
    route.proxy_auth = std::string_view(bytes, sizeof(bytes));
    use();
    use();
    EXPECT_EQ(3, underlay->calls);
}

TEST(dialer, pool_updates_timeout_on_reuse) {
    int destroyed = 0;
    auto underlay = new FailingStreamDialer(&destroyed);
    std::unique_ptr<IDialer> pool(new_pool_dialer(underlay, true));
    ASSERT_NE(nullptr, pool);

    DialTarget target;
    target.host = "origin.example";
    target.port = 80;

    auto stream = pool->dial(target, 10'000'000);
    ASSERT_NE(nullptr, stream);
    EXPECT_EQ(10'000'000U, stream->timeout());
    auto first = stream->get_underlay_object();
    delete stream;

    stream = pool->dial(target, 1'000);
    ASSERT_NE(nullptr, stream);
    EXPECT_EQ(first, stream->get_underlay_object());
    EXPECT_EQ(1'000U, stream->timeout());
    delete stream;
    pool.reset();
    EXPECT_EQ(1, destroyed);
}

TEST(dialer, checked_out_streams_keep_pool_and_underlay_alive) {
    int destroyed = 0;
    auto underlay = new StreamDialer(&destroyed);
    std::unique_ptr<IDialer> pool(new_pool_dialer(underlay, true));
    DialTarget target;
    target.host = "origin.example";
    target.port = 80;
    std::unique_ptr<ISocketStream> first(pool->dial(target));
    std::unique_ptr<ISocketStream> second(pool->dial(target));
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    pool.reset();
    EXPECT_EQ(0, destroyed);
    if (destroyed) {
        // A broken implementation has already freed the streams' pool/heads;
        // avoid dereferencing those dangling pointers after reporting failure.
        first.release();
        second.release();
        return;
    }
    EXPECT_EQ(4, first->write("test", 4));
    EXPECT_EQ(0, second->close());
    first.reset();
    EXPECT_EQ(0, destroyed);
    second.reset();
    EXPECT_EQ(1, destroyed);
}

TEST(dialer, vcpu_local_retries_and_reuses) {
    LocalState state;
    state.fail.store(true, std::memory_order_relaxed);
    std::unique_ptr<IDialer> dialer(new_vcpu_local_dialer(
        {&state, &LocalState::make}));
    ASSERT_NE(nullptr, dialer);

    DialTarget target;
    EXPECT_EQ(nullptr, dialer->dial(target));
    EXPECT_EQ(1, state.attempts.load());
    EXPECT_EQ(0, state.created.load());

    state.fail.store(false, std::memory_order_relaxed);
    EXPECT_EQ(nullptr, dialer->dial(target));
    EXPECT_EQ(nullptr, dialer->dial(target));
    EXPECT_EQ(2, state.attempts.load());
    EXPECT_EQ(1, state.created.load());
    EXPECT_EQ(2, state.calls.load());

    dialer.reset();
    EXPECT_EQ(1, state.destroyed.load());
    EXPECT_EQ(0, state.wrong_vcpu_destructions.load());
}

TEST(dialer, vcpu_local_cross_vcpu_destruction) {
    LocalState state;
    auto dialer = new_vcpu_local_dialer({&state, &LocalState::make});
    ASSERT_NE(nullptr, dialer);
    DialTarget target;
    EXPECT_EQ(nullptr, dialer->dial(target));

    photon::semaphore ready(0), release(0);
    std::thread worker([&] {
        ASSERT_EQ(0, photon::init(photon::INIT_EVENT_DEFAULT,
                                  photon::INIT_IO_NONE));
        DEFER(photon::fini());
        EXPECT_EQ(nullptr, dialer->dial(target));
        EXPECT_EQ(nullptr, dialer->dial(target));
        ready.signal(1);
        release.wait(1);
    });

    ASSERT_EQ(0, ready.wait(1, 5ULL * 1000 * 1000));
    EXPECT_EQ(2, state.created.load());
    EXPECT_EQ(3, state.calls.load());
    delete dialer;
    EXPECT_EQ(2, state.destroyed.load());
    EXPECT_EQ(0, state.wrong_vcpu_destructions.load());
    release.signal(1);
    worker.join();
}

int main(int argc, char** argv) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
