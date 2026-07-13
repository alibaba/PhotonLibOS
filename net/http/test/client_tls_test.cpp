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

#include <thread>

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/thread/thread.h>
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

TEST(client_tls, basic) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete ctx);
    auto tcpserver = net::new_tls_server(ctx, net::new_tcp_socket_server(), true);
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

    auto client = net::http::new_http_client(nullptr, ctx);
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
    return RUN_ALL_TESTS();
}
