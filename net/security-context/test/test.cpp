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

#include <gtest/gtest.h>

#include <photon/net/socket.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/common/utility.h>
#include <photon/common/alog-stdstring.h>
#include <photon/net/security-context/tls-stream.h>

#include "../../test/cert-key.cpp"

using namespace photon;

photon::semaphore sem(0);

int handler(void* arg, net::ISocketStream* stream) {
    auto* ctx = (net::TLSContext*)arg;
    char buf[6];
    char buffer[1048576];
    auto ss = net::new_tls_stream(ctx, stream,
                                       net::SecurityRole::Server, false);
    DEFER(delete ss);
    LOG_INFO("BEFORE READ");
    auto ret = ss->read(buf, 6);
    LOG_INFO("AFTER READ");
    EXPECT_EQ(6, ret);
    LOG_INFO(VALUE(buf));
    LOG_INFO("BEFORE WRITE");
    ss->write(buffer, 1048576);
    LOG_INFO("AFTER WRITE");
    sem.signal(1);
    return 0;
}

void client_test(net::ISocketStream* stream, net::TLSContext* ctx) {
    auto ss = net::new_tls_stream(ctx, stream,
                                       net::SecurityRole::Client, false);
    DEFER(delete ss);
    char buf[] = "Hello";
    auto ret = ss->write(buf, 6);
    EXPECT_EQ(6, ret);
    char b[4096];
    size_t rx = 0;
    for (int i = 0; i < 256; i++) {
        rx += ss->recv(b, 4096);
    }
    EXPECT_EQ(1048576UL, rx);
    sem.wait(1);
}

TEST(basic, test) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete ctx);
    DEFER(photon::wait_all());
    auto server = net::new_tcp_socket_server();
    DEFER(delete server);
    auto client = net::new_tcp_socket_client();
    DEFER(delete client);
    ASSERT_EQ(0, server->bind(0, net::IPAddr("127.0.0.1")));
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    LOG_INFO(VALUE(ep));
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({handler, ctx});
    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    client_test(stream, ctx);
}

int close_test_handler_during_read(void* arg, net::ISocketStream* stream) {
    auto* ctx = (net::TLSContext*)arg;
    char buf[6];
    char buffer[1048576];
    auto ss = net::new_tls_stream(ctx, stream,
                                       net::SecurityRole::Server, false);
    DEFER(delete ss);
    LOG_INFO("BEFORE READ");
    auto ret = ss->read(buf, 6);
    LOG_INFO("AFTER READ");
    // since client will shutdown, return value should be 0
    EXPECT_EQ(3, ret);
    LOG_INFO(VALUE(buf));
    LOG_INFO("BEFORE WRITE");
    ss->write(buffer, 1048576);
    LOG_INFO("AFTER WRITE");
    sem.signal(1);
    return 0;
}

int close_test_handler_during_write(void* arg, net::ISocketStream* stream) {
    auto* ctx = (net::TLSContext*)arg;
    char buf[6];
    char buffer[1048576];
    auto ss = net::new_tls_stream(ctx, stream,
                                       net::SecurityRole::Server, false);
    DEFER(delete ss);
    LOG_INFO("BEFORE READ");
    auto ret = ss->read(buf, 6);
    LOG_INFO("AFTER READ");
    EXPECT_EQ(6, ret);
    LOG_INFO(VALUE(buf));
    LOG_INFO("BEFORE WRITE");
    ss->write(buffer, 1048576);
    LOG_INFO("AFTER WRITE");
    sem.signal(1);
    return 0;
}

void close_sending_client_test(net::ISocketStream* stream, net::TLSContext* ctx) {
    auto ss = net::new_tls_stream(ctx, stream,
                                       net::SecurityRole::Client, false);
    char buf[] = "Hello";
    ss->write(buf, 3);
    delete ss;
    stream->close();
    sem.wait(1);
}

void close_reading_client_test(net::ISocketStream* stream, net::TLSContext* ctx) {
    auto ss = net::new_tls_stream(ctx, stream,
                                       net::SecurityRole::Client, false);
    char buf[] = "Hello";
    auto ret = ss->write(buf, 6);
    EXPECT_EQ(6, ret);
    char b[4096];
    size_t rx = 0;
    for (int i = 0; i < 100; i++) {
        rx += ss->read(b, 4096);
    }
    EXPECT_EQ(409600UL, rx);
    delete ss;
    stream->close();
    sem.wait(1);}

TEST(basic, socket_close_in_read) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete ctx);
    DEFER(photon::wait_all());
    auto server = net::new_tcp_socket_server();
    DEFER(delete server);
    auto client = net::new_tcp_socket_client();
    DEFER(delete client);
    ASSERT_EQ(0, server->bind(0, net::IPAddr("127.0.0.1")));
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    LOG_INFO(VALUE(ep));
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({close_test_handler_during_read, ctx});
    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    close_sending_client_test(stream, ctx);
}

TEST(basic, socket_close_in_write) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete ctx);
    DEFER(photon::wait_all());
    auto server = net::new_tcp_socket_server();
    DEFER(delete server);
    auto client = net::new_tcp_socket_client();
    DEFER(delete client);
    ASSERT_EQ(0, server->bind(0, net::IPAddr("127.0.0.1")));
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    LOG_INFO(VALUE(ep));
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({close_test_handler_during_write, ctx});
    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    close_reading_client_test(stream, ctx);
}

TEST(basic, uds) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete ctx);
    DEFER(photon::wait_all());
    auto server = net::new_uds_server(true);
    DEFER(delete server);
    auto client = net::new_uds_client();
    DEFER(delete client);
    auto fn = "/tmp/uds-tls-test-" + std::to_string(::getpid()) + ".sock";
    ASSERT_EQ(0, server->bind(fn.c_str()));
    ASSERT_EQ(0, server->listen());
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({handler, ctx});
    auto stream = client->connect(fn.c_str());
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    client_test(stream, ctx);
}

int s_handler(void*, net::ISocketStream* stream) {
    char buf[6];
    char buffer[1048576];
    LOG_INFO("BEFORE READ");
    auto ret = stream->read(buf, 6);
    LOG_INFO("AFTER READ");
    EXPECT_EQ(6, ret);
    LOG_INFO(VALUE(buf));
    LOG_INFO("BEFORE WRITE");
    stream->write(buffer, 1048576);
    LOG_INFO("AFTER WRITE");
    sem.signal(1);
    return 0;
}

void s_client_test(net::ISocketStream* stream) {
    char buf[] = "Hello";
    auto ret = stream->write(buf, 6);
    EXPECT_EQ(6, ret);
    char b[4096];
    size_t rx = 0;
    for (int i = 0; i < 256; i++) {
        rx += stream->recv(b, 4096);
    }
    EXPECT_EQ(1048576UL, rx);
    sem.wait(1);
}

TEST(cs, test) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete ctx);
    DEFER(photon::wait_all());
    auto server =
        net::new_tls_server(ctx, net::new_tcp_socket_server(), true);
    DEFER(delete server);
    auto client =
        net::new_tls_client(ctx, net::new_tcp_socket_client(), true);
    DEFER(delete client);
    ASSERT_EQ(0, server->bind(0, net::IPAddr("127.0.0.1")));
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    LOG_INFO(VALUE(ep));
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({s_handler, ctx});
    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    s_client_test(stream);
}

TEST(cs, uds) {
    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete ctx);
    DEFER(photon::wait_all());
    auto server =
        net::new_tls_server(ctx, net::new_uds_server(true), true);
    DEFER(delete server);
    auto client = net::new_tls_client(ctx, net::new_uds_client(), true);
    DEFER(delete client);
    auto fn = "/tmp/uds-tls-test-" + std::to_string(::getpid()) + ".sock";
    ASSERT_EQ(0, server->bind(fn.c_str()));
    ASSERT_EQ(0, server->listen());
    ASSERT_EQ(0, server->start_loop(false));
    photon::thread_yield();
    server->set_handler({s_handler, ctx});
    auto stream = client->connect(fn.c_str());
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    s_client_test(stream);
}

TEST(Socket, nested) {
#ifdef __linux___
    ASSERT_GE(net::et_poller_init(), 0);
    DEFER(net::et_poller_fini());
#endif

    auto server_ssl_ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    ASSERT_NE(server_ssl_ctx, nullptr);
    DEFER(delete server_ssl_ctx);
#ifdef __linux___
    auto server = net::new_tls_server(server_ssl_ctx, net::new_et_tcp_socket_server(), true);
    auto client = net::new_et_tcp_socket_client();
#else
    auto server = net::new_tls_server(server_ssl_ctx, net::new_tcp_socket_server(), true);
    auto client = net::new_tcp_socket_client();
#endif
    DEFER(delete server);

    server->set_handler({s_handler, server_ssl_ctx});
    ASSERT_EQ(0, server->bind());
    ASSERT_EQ(0, server->listen());
    ASSERT_EQ(0, server->start_loop(false));

    net::EndPoint ep1, ep2;
    ASSERT_EQ(0, server->getsockname(ep1));
    LOG_INFO("Sock address: `", ep1);

    auto client_ssl_ctx = net::new_tls_context(nullptr, nullptr, nullptr);
    auto tls_client = net::new_tls_client(client_ssl_ctx, client, true);
    DEFER(delete client);

    auto pooled_client = net::new_tcp_socket_pool(tls_client);
    DEFER(delete pooled_client);

    auto conn = pooled_client->connect(ep1);
    ASSERT_NE(conn, nullptr);

    ASSERT_EQ(0, conn->getpeername(ep2));
    LOG_INFO("Peer address: `", ep2);

    ASSERT_EQ(ep1.port, ep2.port);

    s_client_test(conn);

    auto u1 = pooled_client->get_underlay_object(0);
    ASSERT_EQ(u1, tls_client);

    auto u2 = pooled_client->get_underlay_object(1);
    ASSERT_EQ(u2, client);

    auto u3 = pooled_client->get_underlay_object(2);
    ASSERT_EQ(u3, nullptr);

    auto u4 = server->get_underlay_object(1);
    auto u5 = server->get_underlay_object(-1);
    auto fd = server->get_underlay_fd();
    ASSERT_TRUE((uint64_t) u4 == (uint64_t) u5 && (uint64_t) u4 == (uint64_t) fd);
}

int main(int argc, char** arg) {
#ifdef __linux__
    int ev_engine = photon::INIT_EVENT_EPOLL;
#else
    int ev_engine = photon::INIT_EVENT_KQUEUE;
#endif
    if (photon::init(ev_engine, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
