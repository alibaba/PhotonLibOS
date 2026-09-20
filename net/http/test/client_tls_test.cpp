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

// HTTP-level test: verify set_ca_cert works through the HTTP client. Runs in
// its own std::thread with a dedicated photon runtime so the client and its
// per-client dialer (and the TLS context they hold) are created and torn down
// in isolation from the other tests.
//
// The certificate must carry DNS:localhost, since loading a CA turns on peer
// verification and the client now also checks the name in the URL against the
// certificate. The shared cert in cert-key.cpp has no SAN and a common name of
// DefaultCompanyLt, so it cannot serve this test.
TEST(client_tls, http_with_ca_cert) {
    auto chain = generate_ca_signed_cert({"DNS:localhost", "IP:127.0.0.1"}, "localhost");

    // Server: TLS + HTTP, using a cert issued for localhost
    auto server_ctx = net::new_tls_context(chain.cert_pem.c_str(), chain.key_pem.c_str(), nullptr);
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

        // Client: separate TLSContext, load server's CA
        auto client_ctx = net::new_tls_context();
        DEFER(delete client_ctx);
        if (client_ctx->set_ca_cert(chain.ca_pem.c_str()) != 0) {
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

// Verify HTTP clients with different CA configs are isolated. Each client owns
// its own PooledDialer (one per vCPU), so a TLS context loaded into one client
// never leaks into another; running each in its own std::thread proves it.
TEST(client_tls, http_client_cross_thread_isolation) {
    // Reached over https://127.0.0.1, so the certificate needs IP:127.0.0.1.
    auto chain = generate_ca_signed_cert({"DNS:localhost", "IP:127.0.0.1"}, "localhost");
    auto server_ctx = net::new_tls_context(chain.cert_pem.c_str(), chain.key_pem.c_str(), nullptr);
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
        ctx_a->set_ca_cert(chain.ca_pem.c_str());
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

// The connection pool must not let one hostname's verified connection serve a
// request for another. The certificate covers DNS:localhost only, so the second
// request must fail even though it reaches the same IP and port; keyed on the
// endpoint alone, it would reuse the first connection and wrongly succeed.
TEST(client_tls, pool_does_not_reuse_across_hostnames) {
    auto chain = generate_ca_signed_cert({"DNS:localhost"}, "localhost");
    auto server_ctx = net::new_tls_context(chain.cert_pem.c_str(), chain.key_pem.c_str(), nullptr);
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
    int by_name_result = -1, by_ip_result = 0;
    photon::semaphore sem;

    std::thread t([&, port] {
        photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
        DEFER(photon::fini());

        auto ctx = net::new_tls_context();
        DEFER(delete ctx);
        ctx->set_ca_cert(chain.ca_pem.c_str());
        auto client = net::http::new_http_client(nullptr, ctx);
        DEFER(delete client);

        char buf[4096];
        {
            auto by_name = estring().appends("https://localhost:", port, "/test");
            auto op1 = client->new_operation(net::http::Verb::GET, by_name);
            DEFER(client->destroy_operation(op1));
            op1->req.headers.range(0, 19);
            // Retries are left enabled here: "localhost" may resolve to ::1 on
            // hosts whose /etc/hosts lists it first, while the server binds IPv4
            // only, and the dialer discards a failed address so the next attempt
            // reaches 127.0.0.1. The second request below is the one that must
            // fail, and it is pinned to a single attempt.
            by_name_result = op1->call();
            // Drain the body and end the operation, so the connection goes back
            // to the pool idle rather than being dropped. Without this the
            // second request dials afresh and the reuse path is never taken,
            // leaving the test passing while checking nothing.
            EXPECT_EQ(20, op1->resp.read(buf, 20));
        }

        // Same endpoint, different name: must not ride on the pooled connection
        // that was verified for localhost.
        auto by_ip = estring().appends("https://127.0.0.1:", port, "/test");
        auto op2 = client->new_operation(net::http::Verb::GET, by_ip);
        DEFER(client->destroy_operation(op2));
        op2->req.headers.range(0, 19);
        op2->retry = 0;
        by_ip_result = op2->call();
        sem.signal(1);
    });
    t.detach();
    sem.wait(1);

    EXPECT_EQ(0, by_name_result);
    EXPECT_NE(0, by_ip_result);
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
