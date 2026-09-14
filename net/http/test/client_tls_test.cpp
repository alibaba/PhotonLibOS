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

#include <openssl/ssl.h>

#include <atomic>
#include <thread>

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>
#include <photon/net/http/message.h>
#include <photon/net/http/server.h>
#include <photon/net/http/client.h>
#include "../../../test/gtest.h"
#include "to_url.h"

#include "../../test/cert-key.cpp"
#include "../../security-context/test/test_cert_utils.h"

using namespace photon;

int idiot_handler(void*, net::http::Request &req, net::http::Response &resp, std::string_view) {
    std::string str;
    auto r = req.headers.range();
    auto cl = r.second - r.first + 1;
    LOG_DEBUG("content_range: `-` (`)", r.first, r.second, cl);
    if (cl > 4096) {
        LOG_ERROR_RETURN(0, -1, "RetType failed test");
    }
    resp.set_result(200);
    resp.headers.content_length(cl);
    resp.headers.insert("Test_Handle", "test");

    str.resize(cl);
    memset((void*)str.data(), '0', cl);
    resp.write((void*)str.data(), str.size());
    return 0;
}

net::TLSContext *tls_ctx;

TEST(client_tls, basic) {
    auto tcpserver = net::new_tls_server(tls_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tcpserver);
    tcpserver->timeout(1000ULL*1000);
    int r = tcpserver->bind_v4localhost();
    if (r != 0)
        LOG_ERRNO_RETURN(0, , "failed to bind to localhost");
    LOG_DEBUG("bind to :", tcpserver->getsockname());
    tcpserver->listen();

    auto server = net::http::new_http_server();
    DEFER(delete server);
    server->add_handler({nullptr, &idiot_handler});

    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();

    auto client = net::http::new_http_client(nullptr, tls_ctx);
    DEFER(delete client);
    auto op = client->new_operation(net::http::Verb::GET, to_surl(tcpserver, "/test"));
    DEFER(client->destroy_operation(op));
    auto exp_len = 20;
    op->req.headers.range(0, exp_len - 1);
    op->call();
    EXPECT_EQ(200, op->resp.status_code());
    char buf[4096];
    auto ret = op->resp.read(buf, 4096);
    EXPECT_EQ(exp_len, ret);
    EXPECT_EQ("test", op->resp.headers["Test_Handle"]);
}

int echo_target_handler(void*, net::http::Request &req, net::http::Response &resp, std::string_view) {
    auto target = req.target();
    resp.set_result(200);
    resp.headers.content_length(target.size());
    resp.write(target.data(), target.size());
    return 0;
}

// Runs a forward-proxy HTTP server on its own OS thread (its own photon vCPU).
//
// Rationale: the HTTP client caches a PooledDialer in thread-local storage.
// If the proxy shared a vCPU with the test client, the proxy's internal
// forward client and the test client would use the same dialer / connection
// pool, and the proxy's bidirectional tunnel_copy loops would interleave with
// the client on the same cooperative scheduler. Under macOS/kqueue wakeup
// ordering this can stall the tunnel and hang the test. Running the proxy on
// a dedicated vCPU mirrors the real-world topology (a proxy is a separate
// process) and keeps the two dialers independent.
class ForwardProxyThread {
public:
    // handler == nullptr → a default forward proxy handler is created and
    // owned internally. Otherwise ownership of `handler` is transferred here;
    // it is destroyed on the proxy vCPU during teardown.
    explicit ForwardProxyThread(net::http::HTTPHandler* handler = nullptr) {
        os_thread_ = std::thread([this, handler] { run(handler); });
        sem_ready_.wait(1);
    }
    ~ForwardProxyThread() {
        stop_.store(true);
        os_thread_.join();
    }
    uint16_t port() const { return port_; }

private:
    void run(net::http::HTTPHandler* handler) {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());

        auto proxy_tcp = net::new_tcp_socket_server();
        auto proxy_http = net::http::new_http_server();
        proxy_tcp->timeout(1000UL * 1000);
        if (proxy_tcp->bind_v4localhost() != 0 || proxy_tcp->listen() != 0) {
            delete proxy_tcp;
            delete proxy_http;
            sem_ready_.signal(1);
            return;
        }
        auto* owned = handler ? handler : net::http::new_default_forward_proxy_handler();
        proxy_http->add_handler(owned, false);
        proxy_tcp->set_handler(proxy_http->get_connection_handler());
        proxy_tcp->start_loop();
        port_ = proxy_tcp->getsockname().port;
        sem_ready_.signal(1);

        while (!stop_.load())
            photon::thread_usleep(10 * 1000);

        delete proxy_tcp;   // stops server, joins connection threads
        delete proxy_http;  // safe: no threads reference handler now
        delete owned;
    }

    std::thread os_thread_;
    photon::semaphore sem_ready_;
    std::atomic<bool> stop_{false};
    uint16_t port_ = 0;
};

TEST(client_proxy, connect_tunnel) {
    auto tls_svr = net::new_tls_server(tls_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tls_svr);
    tls_svr->timeout(1000UL * 1000);
    ASSERT_EQ(0, tls_svr->bind_v4localhost());
    ASSERT_EQ(0, tls_svr->listen());

    auto target_http = net::http::new_http_server();
    DEFER(delete target_http);
    target_http->add_handler({nullptr, &echo_target_handler});
    tls_svr->set_handler(target_http->get_connection_handler());
    tls_svr->start_loop();
    auto target_port = tls_svr->getsockname().port;

    ForwardProxyThread proxy;
    auto proxy_port = proxy.port();

    auto client = net::http::new_http_client(nullptr, tls_ctx);
    DEFER({ delete client; photon::thread_usleep(50 * 1000); });
    char proxy_url[128];
    snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u", proxy_port);
    client->set_proxy(proxy_url);

    char target_url[128];
    snprintf(target_url, sizeof(target_url), "https://127.0.0.1:%u/test-tunnel", target_port);
    auto op = client->new_operation(net::http::Verb::GET, target_url);
    DEFER(client->destroy_operation(op));
    op->req.headers.content_length(0);
    op->retry = 0;
    int ret = op->call();
    ASSERT_EQ(0, ret);
    EXPECT_EQ(200, op->resp.status_code());

    char buf[4096];
    auto n = op->resp.read(buf, op->resp.headers.content_length());
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    EXPECT_STREQ("/test-tunnel", buf);
}

// Handler that captures CONNECT request headers for verification,
// then delegates to the default forward proxy handler for tunneling.
//
// Runs on the proxy vCPU while assertions read its fields on the main vCPU.
// `connect_count` is atomic and incremented last within the CONNECT branch,
// so its release ordering publishes the captured_* strings written before it;
// tests read connect_count first, which acquires those writes.
class HeaderCapturingProxyHandler : public net::http::HTTPHandler {
public:
    std::string captured_user_agent;
    std::string captured_custom_header;
    std::string captured_target;  // CONNECT request-line target (host:port)
    std::string captured_proxy_auth;  // Proxy-Authorization header value
    std::atomic<int> connect_count{0};

    int handle_request(net::http::Request &req, net::http::Response &resp,
                       std::string_view prefix) override {
        if (req.verb() == net::http::Verb::CONNECT) {
            captured_target = std::string(req.target());
            auto ua = req.headers["User-Agent"];
            if (!ua.empty()) captured_user_agent = std::string(ua);
            auto custom = req.headers["X-Test-Header"];
            if (!custom.empty()) captured_custom_header = std::string(custom);
            auto auth = req.headers["Proxy-Authorization"];
            if (!auth.empty()) captured_proxy_auth = std::string(auth);
            connect_count++;
        }
        return inner_->handle_request(req, resp, prefix);
    }

    HeaderCapturingProxyHandler() {
        inner_.reset(net::http::new_default_forward_proxy_handler());
    }
private:
    std::unique_ptr<net::http::HTTPHandler> inner_;
};

TEST(client_proxy, connect_tunnel_inherits_headers) {
    // TLS target server
    auto tls_svr = net::new_tls_server(tls_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tls_svr);
    tls_svr->timeout(1000UL * 1000);
    ASSERT_EQ(0, tls_svr->bind_v4localhost());
    ASSERT_EQ(0, tls_svr->listen());
    auto target_http = net::http::new_http_server();
    DEFER(delete target_http);
    target_http->add_handler({nullptr, &echo_target_handler});
    tls_svr->set_handler(target_http->get_connection_handler());
    tls_svr->start_loop();
    auto target_port = tls_svr->getsockname().port;

    // Proxy server (own vCPU) with header-capturing handler
    auto* cap_handler = new HeaderCapturingProxyHandler();
    ForwardProxyThread proxy(cap_handler);
    auto proxy_port = proxy.port();

    // Client with proxy headers (used for CONNECT handshake)
    auto client = net::http::new_http_client(nullptr, tls_ctx);
    DEFER({ delete client; photon::thread_usleep(50 * 1000); });
    char proxy_url[128];
    snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u", proxy_port);
    client->set_proxy(proxy_url);
    // Use proxy_headers() for headers that should appear in CONNECT request
    client->proxy_headers()->insert("User-Agent", "TestClient/1.0");
    client->proxy_headers()->insert("X-Test-Header", "hello-proxy");

    char target_url[128];
    snprintf(target_url, sizeof(target_url), "https://127.0.0.1:%u/test-headers", target_port);
    auto op = client->new_operation(net::http::Verb::GET, target_url);
    DEFER(client->destroy_operation(op));
    op->req.headers.content_length(0);
    op->retry = 0;
    int ret = op->call();
    ASSERT_EQ(0, ret);
    EXPECT_EQ(200, op->resp.status_code());

    // Verify CONNECT request received the proxy headers
    EXPECT_EQ(1, cap_handler->connect_count.load());
    EXPECT_EQ("TestClient/1.0", cap_handler->captured_user_agent);
    EXPECT_EQ("hello-proxy", cap_handler->captured_custom_header);
    // Verify CONNECT target is in host:port format (RFC 7231 §4.3.6)
    char expected_target[64];
    snprintf(expected_target, sizeof(expected_target), "127.0.0.1:%u", target_port);
    EXPECT_EQ(expected_target, cap_handler->captured_target);
}

TEST(client_proxy, connect_tunnel_multiple_requests) {
    auto tls_svr = net::new_tls_server(tls_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tls_svr);
    tls_svr->timeout(1000UL * 1000);
    ASSERT_EQ(0, tls_svr->bind_v4localhost());
    ASSERT_EQ(0, tls_svr->listen());

    auto target_http = net::http::new_http_server();
    DEFER(delete target_http);
    target_http->add_handler({nullptr, &echo_target_handler});
    tls_svr->set_handler(target_http->get_connection_handler());
    tls_svr->start_loop();
    auto target_port = tls_svr->getsockname().port;

    ForwardProxyThread proxy;
    auto proxy_port = proxy.port();

    auto client = net::http::new_http_client(nullptr, tls_ctx);
    DEFER({ delete client; photon::thread_usleep(50 * 1000); });
    char proxy_url[128];
    snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u", proxy_port);
    client->set_proxy(proxy_url);

    for (int i = 0; i < 3; i++) {
        char target_url[128];
        snprintf(target_url, sizeof(target_url), "https://127.0.0.1:%u/req-%d", target_port, i);
        auto op = client->new_operation(net::http::Verb::GET, target_url);
        DEFER(client->destroy_operation(op));
        op->req.headers.content_length(0);
        op->retry = 0;
        int ret = op->call();
        ASSERT_EQ(0, ret);
        EXPECT_EQ(200, op->resp.status_code());

        char buf[4096];
        auto n = op->resp.read(buf, op->resp.headers.content_length());
        ASSERT_GT(n, 0);
        buf[n] = '\0';
        char expected[64];
        snprintf(expected, sizeof(expected), "/req-%d", i);
        EXPECT_STREQ(expected, buf);
    }
}

// Proxy handler that rejects every CONNECT with a non-2xx status, so the
// tunnel handshake never completes. Used to verify the client surfaces the
// failure instead of proceeding as if the tunnel were established.
class RejectingConnectProxyHandler : public net::http::HTTPHandler {
public:
    std::atomic<int> connect_count{0};
    RejectingConnectProxyHandler() {
        inner_.reset(net::http::new_default_forward_proxy_handler());
    }
    int handle_request(net::http::Request &req, net::http::Response &resp,
                       std::string_view prefix) override {
        if (req.verb() == net::http::Verb::CONNECT) {
            connect_count++;
            resp.set_result(403, "Forbidden");
            resp.headers.content_length(0);
            resp.keep_alive(false);
            return 0;
        }
        return inner_->handle_request(req, resp, prefix);
    }
private:
    std::unique_ptr<net::http::HTTPHandler> inner_;
};

TEST(client_proxy, connect_tunnel_proxy_rejects) {
    // Proxy answers CONNECT with 403; no tunnel is established.
    auto* rej_handler = new RejectingConnectProxyHandler();
    ForwardProxyThread proxy(rej_handler);
    auto proxy_port = proxy.port();

    auto client = net::http::new_http_client(nullptr, tls_ctx);
    DEFER({ delete client; photon::thread_usleep(50 * 1000); });
    char proxy_url[128];
    snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u", proxy_port);
    client->set_proxy(proxy_url);

    // Target need not exist: the proxy rejects before any tunnel is set up.
    auto op = client->new_operation(net::http::Verb::GET, "https://127.0.0.1:1/never");
    DEFER(client->destroy_operation(op));
    op->req.headers.content_length(0);
    op->retry = 0;
    int ret = op->call();
    EXPECT_NE(0, ret);
    EXPECT_EQ(-1, op->status_code);
    EXPECT_EQ(1, rej_handler->connect_count.load());
}

TEST(client_proxy, connect_tunnel_concurrent_reuse) {
    auto tls_svr = net::new_tls_server(tls_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tls_svr);
    tls_svr->timeout(1000UL * 1000);
    ASSERT_EQ(0, tls_svr->bind_v4localhost());
    ASSERT_EQ(0, tls_svr->listen());

    auto target_http = net::http::new_http_server();
    DEFER(delete target_http);
    target_http->add_handler({nullptr, &echo_target_handler});
    tls_svr->set_handler(target_http->get_connection_handler());
    tls_svr->start_loop();
    auto target_port = tls_svr->getsockname().port;

    ForwardProxyThread proxy;
    auto proxy_port = proxy.port();

    auto client = net::http::new_http_client(nullptr, tls_ctx);
    DEFER({ delete client; photon::thread_usleep(50 * 1000); });
    char proxy_url[128];
    snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u", proxy_port);
    client->set_proxy(proxy_url);

    // Concurrent requests through the same tunnel pool.
    // All target the same host:port so they should reuse a single tunnel.
    constexpr int N = 5;
    photon::semaphore sem(0);
    int success_count = 0;

    for (int i = 0; i < N; i++) {
        auto fn = [&, i] {
            char target_url[128];
            snprintf(target_url, sizeof(target_url),
                     "https://127.0.0.1:%u/concurrent-%d", target_port, i);
            auto op = client->new_operation(net::http::Verb::GET, target_url);
            DEFER(client->destroy_operation(op));
            op->req.headers.content_length(0);
            op->retry = 0;
            int ret = op->call();
            if (ret == 0 && op->resp.status_code() == 200) {
                char buf[4096];
                auto n = op->resp.read(buf, op->resp.headers.content_length());
                if (n > 0) {
                    buf[n] = '\0';
                    char expected[64];
                    snprintf(expected, sizeof(expected), "/concurrent-%d", i);
                    EXPECT_STREQ(expected, buf);
                    success_count++;
                }
            }
            sem.signal(1);
        };
        photon::thread_create11(fn);
    }
    sem.wait(N);
    EXPECT_EQ(N, success_count);
}

// Proxy handler that counts CONNECT requests for verification.
// connect_count is atomic: written on the proxy vCPU, read on the main vCPU.
class ConnectCountingProxyHandler : public net::http::HTTPHandler {
public:
    std::atomic<int> connect_count{0};
    ConnectCountingProxyHandler() {
        inner_.reset(net::http::new_default_forward_proxy_handler());
    }
    int handle_request(net::http::Request &req, net::http::Response &resp,
                       std::string_view prefix) override {
        if (req.verb() == net::http::Verb::CONNECT)
            connect_count++;
        return inner_->handle_request(req, resp, prefix);
    }
private:
    std::unique_ptr<net::http::HTTPHandler> inner_;
};

// Helper: set up proxy (own vCPU) + target servers, return proxy/target ports.
struct TunnelTestEnv {
    net::http::HTTPServer* target_http = nullptr;
    net::ISocketServer* tls_svr = nullptr;
    ForwardProxyThread* proxy = nullptr;
    net::TLSContext* ctx = nullptr;
    uint16_t target_port = 0;
    uint16_t proxy_port = 0;
};

static void setup_tunnel_test(TunnelTestEnv& env,
                               ConnectCountingProxyHandler* counter = nullptr) {
    env.ctx = net::new_tls_context(cert_str, key_str, passphrase_str);

    env.tls_svr = net::new_tls_server(env.ctx, net::new_tcp_socket_server(), true);
    env.tls_svr->timeout(1000UL * 1000);
    ASSERT_EQ(0, env.tls_svr->bind_v4localhost());
    ASSERT_EQ(0, env.tls_svr->listen());

    env.target_http = net::http::new_http_server();
    env.target_http->add_handler({nullptr, &echo_target_handler});
    env.tls_svr->set_handler(env.target_http->get_connection_handler());
    env.tls_svr->start_loop();
    env.target_port = env.tls_svr->getsockname().port;

    // Proxy runs on its own vCPU (see ForwardProxyThread rationale).
    env.proxy = new ForwardProxyThread(counter);
    env.proxy_port = env.proxy->port();
}

static void teardown_tunnel_test(TunnelTestEnv& env) {
    delete env.proxy;       // stops proxy vCPU, joins its connection threads
    delete env.tls_svr;
    delete env.target_http;
    delete env.ctx;
}

// Verifies Proxy-Authorization from proxy URL is forwarded in the CONNECT handshake.
TEST(client_proxy, connect_tunnel_auth) {
    // TLS target server
    auto tls_svr = net::new_tls_server(tls_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tls_svr);
    tls_svr->timeout(1000UL * 1000);
    ASSERT_EQ(0, tls_svr->bind_v4localhost());
    ASSERT_EQ(0, tls_svr->listen());
    auto target_http = net::http::new_http_server();
    DEFER(delete target_http);
    target_http->add_handler({nullptr, &echo_target_handler});
    tls_svr->set_handler(target_http->get_connection_handler());
    tls_svr->start_loop();
    auto target_port = tls_svr->getsockname().port;

    // Proxy server (own vCPU) with header-capturing handler
    auto* cap_handler = new HeaderCapturingProxyHandler();
    ForwardProxyThread proxy(cap_handler);
    auto proxy_port = proxy.port();

    // Client with proxy URL containing user:pass credentials
    auto client = net::http::new_http_client(nullptr, tls_ctx);
    DEFER({ delete client; photon::thread_usleep(50 * 1000); });
    char proxy_url[128];
    snprintf(proxy_url, sizeof(proxy_url), "http://user:pass@127.0.0.1:%u", proxy_port);
    client->set_proxy(proxy_url);

    char target_url[128];
    snprintf(target_url, sizeof(target_url),
             "https://127.0.0.1:%u/auth-check", target_port);
    auto op = client->new_operation(net::http::Verb::GET, target_url);
    DEFER(client->destroy_operation(op));
    op->req.headers.content_length(0);
    op->retry = 0;
    int ret = op->call();
    ASSERT_EQ(0, ret);
    EXPECT_EQ(200, op->resp.status_code());

    // CONNECT must carry Proxy-Authorization: Basic base64(user:pass)
    EXPECT_EQ(1, cap_handler->connect_count.load());
    EXPECT_FALSE(cap_handler->captured_proxy_auth.empty());
    EXPECT_TRUE(cap_handler->captured_proxy_auth.substr(0, 6) == "Basic ");
}

// After the target server closes the connection, the tunnel pool should
// detect the dead fd and create a new CONNECT tunnel on the next request.
TEST(client_proxy, connect_tunnel_server_close) {
    TunnelTestEnv env;
    auto counter = new ConnectCountingProxyHandler();
    setup_tunnel_test(env, counter);

    auto client = net::http::new_http_client(nullptr, env.ctx);
    char proxy_url[128];
    snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u", env.proxy_port);
    client->set_proxy(proxy_url);

    char target_url[128];
    snprintf(target_url, sizeof(target_url),
             "https://127.0.0.1:%u/req-1", env.target_port);

    // Request 1: creates a new tunnel.
    auto op = client->new_operation(net::http::Verb::GET, target_url);
    op->req.headers.content_length(0);
    op->retry = 0;
    ASSERT_EQ(0, op->call());
    EXPECT_EQ(200, op->resp.status_code());
    // Drain response body so the stream is clean for return-to-pool.
    char buf[4096];
    while (op->resp.read(buf, sizeof(buf)) > 0) {}
    EXPECT_EQ(1, counter->connect_count.load());

    // Brief yield to let the target server finish closing the connection.
    photon::thread_usleep(10 * 1000);

    // Request 2: old tunnel's fd should be detected as dead (EOF).
    // Pool misses → creates a new CONNECT.
    snprintf(target_url, sizeof(target_url),
             "https://127.0.0.1:%u/req-2", env.target_port);
    auto op2 = client->new_operation(net::http::Verb::GET, target_url);
    op2->req.headers.content_length(0);
    op2->retry = 0;
    ASSERT_EQ(0, op2->call());
    EXPECT_EQ(200, op2->resp.status_code());
    while (op2->resp.read(buf, sizeof(buf)) > 0) {}
    EXPECT_GE(counter->connect_count.load(), 2);

    client->destroy_operation(op);
    client->destroy_operation(op2);
    delete client;
    photon::thread_usleep(50 * 1000);
    teardown_tunnel_test(env);
}

// Requests to different targets (host:port) should use separate tunnels.
TEST(client_proxy, connect_tunnel_multi_target) {
    // Target A
    auto tls_svr_a = net::new_tls_server(tls_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tls_svr_a);
    tls_svr_a->timeout(1000UL * 1000);
    ASSERT_EQ(0, tls_svr_a->bind_v4localhost());
    ASSERT_EQ(0, tls_svr_a->listen());
    auto http_a = net::http::new_http_server();
    DEFER(delete http_a);
    http_a->add_handler({nullptr, &echo_target_handler});
    tls_svr_a->set_handler(http_a->get_connection_handler());
    tls_svr_a->start_loop();
    auto port_a = tls_svr_a->getsockname().port;

    // Target B
    auto tls_svr_b = net::new_tls_server(tls_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tls_svr_b);
    tls_svr_b->timeout(1000UL * 1000);
    ASSERT_EQ(0, tls_svr_b->bind_v4localhost());
    ASSERT_EQ(0, tls_svr_b->listen());
    auto http_b = net::http::new_http_server();
    DEFER(delete http_b);
    http_b->add_handler({nullptr, &echo_target_handler});
    tls_svr_b->set_handler(http_b->get_connection_handler());
    tls_svr_b->start_loop();
    auto port_b = tls_svr_b->getsockname().port;

    // Proxy (own vCPU)
    auto* counter = new ConnectCountingProxyHandler();
    ForwardProxyThread proxy(counter);
    auto proxy_port = proxy.port();

    auto client = net::http::new_http_client(nullptr, tls_ctx);
    char proxy_url[128];
    snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u", proxy_port);
    client->set_proxy(proxy_url);

    // Request to target A
    char url_a[128];
    snprintf(url_a, sizeof(url_a), "https://127.0.0.1:%u/path-a", port_a);
    auto op_a = client->new_operation(net::http::Verb::GET, url_a);
    op_a->req.headers.content_length(0);
    op_a->retry = 0;
    ASSERT_EQ(0, op_a->call());
    EXPECT_EQ(200, op_a->resp.status_code());
    char buf[4096];
    auto n = op_a->resp.read(buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    EXPECT_STREQ("/path-a", buf);

    // Request to target B — different port → different pool key → new tunnel.
    char url_b[128];
    snprintf(url_b, sizeof(url_b), "https://127.0.0.1:%u/path-b", port_b);
    auto op_b = client->new_operation(net::http::Verb::GET, url_b);
    op_b->req.headers.content_length(0);
    op_b->retry = 0;
    ASSERT_EQ(0, op_b->call());
    EXPECT_EQ(200, op_b->resp.status_code());
    n = op_b->resp.read(buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    EXPECT_STREQ("/path-b", buf);

    // Two different targets → exactly 2 CONNECT requests (no sharing).
    EXPECT_EQ(2, counter->connect_count.load());

    // Explicit cleanup: close pool connections first, then let proxy threads finish.
    client->destroy_operation(op_a);
    client->destroy_operation(op_b);
    delete client;
    photon::thread_usleep(50 * 1000);
}

// Same target with different proxy credentials must NOT share tunnels.
// Regression guard: TunnelPool::connect_tunnel includes auth in the pool key.
TEST(client_proxy, connect_tunnel_auth_isolation) {
    TunnelTestEnv env;
    auto counter = new ConnectCountingProxyHandler();
    setup_tunnel_test(env, counter);

    auto client = net::http::new_http_client(nullptr, env.ctx);

    char target_url[128];
    snprintf(target_url, sizeof(target_url),
             "https://127.0.0.1:%u/auth-test", env.target_port);

    // Request 1 with user1:pass1 → creates tunnel A.
    char proxy1[128];
    snprintf(proxy1, sizeof(proxy1), "http://user1:pass1@127.0.0.1:%u", env.proxy_port);
    client->set_proxy(proxy1);
    auto op1 = client->new_operation(net::http::Verb::GET, target_url);
    op1->req.headers.content_length(0);
    op1->retry = 0;
    ASSERT_EQ(0, op1->call());
    EXPECT_EQ(200, op1->resp.status_code());
    char buf[4096];
    while (op1->resp.read(buf, sizeof(buf)) > 0) {}
    client->destroy_operation(op1);

    // Request 2 with user2:pass2 to same target → different auth → new tunnel.
    char proxy2[128];
    snprintf(proxy2, sizeof(proxy2), "http://user2:pass2@127.0.0.1:%u", env.proxy_port);
    client->set_proxy(proxy2);
    auto op2 = client->new_operation(net::http::Verb::GET, target_url);
    op2->req.headers.content_length(0);
    op2->retry = 0;
    ASSERT_EQ(0, op2->call());
    EXPECT_EQ(200, op2->resp.status_code());
    while (op2->resp.read(buf, sizeof(buf)) > 0) {}
    client->destroy_operation(op2);

    // Different auth → 2 CONNECT requests (not sharing the same tunnel).
    EXPECT_EQ(2, counter->connect_count.load());

    delete client;
    photon::thread_usleep(50 * 1000);
    teardown_tunnel_test(env);
}

// Server Name Indication (SNI) for SSL
#if OPENSSL_VERSION_NUMBER >= 0x10100000LL
TEST(http_client, DISABLED_SNI) {
    auto tls = photon::net::new_tls_context();
    DEFER(delete tls);
    auto client = photon::net::http::new_http_client(nullptr, tls);
    DEFER(delete client);
    auto op = client->new_operation(photon::net::http::Verb::GET, "https://debug.fly.dev");
    DEFER(client->destroy_operation(op));
    op->retry = 0;
    int res = op->call();
    ASSERT_EQ(0, res);
}
#endif

// HTTP-level test: verify set_ca_cert works through the HTTP client.
// Must run in a separate std::thread because the thread_local PooledDialer
// caches the TLS context from previous tests; reusing it after the context
// is freed would be a use-after-free.
TEST(client_tls, http_with_ca_cert) {
    // Server: TLS + HTTP, using self-signed cert
    auto server_ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete server_ctx);
    auto tcpserver = net::new_tls_server(server_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tcpserver);
    tcpserver->timeout(1000UL * 1000);
    ASSERT_EQ(0, tcpserver->bind_v4localhost());
    tcpserver->listen();

    auto server = net::http::new_http_server();
    DEFER(delete server);
    server->add_handler({nullptr, &idiot_handler});
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();

    auto port = tcpserver->getsockname().port;
    int client_result = -1;
    int status_code = 0;
    std::string test_handle;
    photon::semaphore sem;

    std::thread t([&, port] {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());

        // Client: separate TLSContext, load server's cert as CA
        auto client_ctx = net::new_tls_context();
        DEFER(delete client_ctx);
        if (client_ctx->set_ca_cert(cert_str) != 0) {
            sem.signal(1);
            return;
        }

        auto client = net::http::new_http_client(nullptr, client_ctx);
        DEFER(delete client);

        auto url = estring().appends("https://localhost:", port, "/test");
        auto op = client->new_operation(net::http::Verb::GET, url);
        DEFER(client->destroy_operation(op));
        op->req.headers.range(0, 19);
        client_result = op->call();
        if (client_result == 0) {
            status_code = op->resp.status_code();
            test_handle = std::string(op->resp.headers["Test_Handle"]);
        }
        sem.signal(1);
    });
    t.detach();
    sem.wait(1);

    ASSERT_EQ(0, client_result);
    EXPECT_EQ(200, status_code);
    EXPECT_EQ("test", test_handle);
}

// Verify HTTP clients with different CA configs are isolated across OS threads.
// Each client runs in its own std::thread to get an independent PooledDialer,
// avoiding use-after-free on the thread_local dialer's cached TLS context.
TEST(client_tls, http_client_cross_thread_isolation) {
    auto server_ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete server_ctx);
    auto tcpserver = net::new_tls_server(server_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete tcpserver);
    tcpserver->timeout(1000UL * 1000);
    ASSERT_EQ(0, tcpserver->bind_v4localhost());
    tcpserver->listen();

    auto server = net::http::new_http_server();
    DEFER(delete server);
    server->add_handler({nullptr, &idiot_handler});
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();

    auto port = tcpserver->getsockname().port;

    // Client A with correct CA (separate thread for fresh PooledDialer)
    int thread_a_result = -1;
    int thread_a_status = 0;
    photon::semaphore sem_a;
    std::thread ta([&, port] {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());

        auto ctx_a = net::new_tls_context();
        DEFER(delete ctx_a);
        ctx_a->set_ca_cert(cert_str);
        auto client_a = net::http::new_http_client(nullptr, ctx_a);
        DEFER(delete client_a);

        auto url = estring().appends("https://127.0.0.1:", port, "/test");
        auto op = client_a->new_operation(net::http::Verb::GET, url);
        DEFER(client_a->destroy_operation(op));
        op->retry = 0;
        thread_a_result = op->call();
        if (thread_a_result == 0)
            thread_a_status = op->resp.status_code();
        sem_a.signal(1);
    });
    ta.detach();
    sem_a.wait(1);
    ASSERT_EQ(0, thread_a_result);
    EXPECT_EQ(200, thread_a_status);

    // Client B with wrong CA (separate thread)
    auto wrong_ca_pem = generate_different_self_signed_cert();
    int thread_b_result = 0;
    photon::semaphore sem_b;
    std::thread tb([&, port] {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());

        auto ctx_b = net::new_tls_context();
        DEFER(delete ctx_b);
        ctx_b->set_ca_cert(wrong_ca_pem.c_str());
        auto client_b = net::http::new_http_client(nullptr, ctx_b);
        DEFER(delete client_b);

        auto url = estring().appends("https://127.0.0.1:", port, "/test");
        auto op = client_b->new_operation(net::http::Verb::GET, url);
        DEFER(client_b->destroy_operation(op));
        op->retry = 0;
        thread_b_result = op->call();
        sem_b.signal(1);
    });
    tb.detach();
    sem_b.wait(1);
    EXPECT_NE(0, thread_b_result);
}

int main(int argc, char** arg) {
    LOG_DEBUG("Begin test");
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    set_log_output_level(ALOG_DEBUG);
    ::testing::InitGoogleTest(&argc, arg);

    tls_ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete tls_ctx);

    return RUN_ALL_TESTS();
}
