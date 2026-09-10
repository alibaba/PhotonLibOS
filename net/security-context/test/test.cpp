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

#include "../../../test/gtest.h"

#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>

#include <photon/net/socket.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/common/estring.h>
#include <photon/common/utility.h>
#include <photon/common/alog-stdstring.h>
#include <photon/net/security-context/tls-stream.h>

#include "../../test/cert-key.cpp"
#include "test_cert_utils.h"

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
    EXPECT_EQ(1048576ULL, rx);
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
    ASSERT_EQ(0, server->bind_v4localhost());
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
    EXPECT_EQ(409600ULL, rx);
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
    ASSERT_EQ(0, server->bind_v4localhost());
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
    ASSERT_EQ(0, server->bind_v4localhost());
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
    LOG_DEBUG("befor write");
    auto ret = stream->write(buf, 6);
    LOG_DEBUG("after write ret=", ret);
    EXPECT_EQ(6, ret);
    char b[4096];
    size_t rx = 0;
    for (int i = 0; i < 256; i++) {
        rx += stream->recv(b, 4096);
    }
    EXPECT_EQ(1048576ULL, rx);
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
    ASSERT_EQ(0, server->bind_v4localhost());
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
#ifdef __APPLE__
    LOG_INFO("skip this case in MacOS");
#endif
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
    ASSERT_EQ(0, server->bind_v4localhost());
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

estring_view alpn_select_cb(void*, const std::vector<estring_view>& p) {
    for (auto const &x : p) {
        LOG_INFO(VALUE(x));
    }
    return p[1];
}

TEST(basic, alpn) {
    // Server side
    auto ctx_s = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete ctx_s);
    ctx_s->set_alpn_select_cb({alpn_select_cb, nullptr});
    auto srv = net::new_tls_server(ctx_s, net::new_tcp_socket_server(), true);
    DEFER(delete srv);

    srv->bind_v4any();
    srv->listen();
    photon::thread_create11([&]{
        net::ISocketStream* s_s = nullptr;
        while (!s_s) {
            s_s = srv->accept();
        }
        DEFER(delete s_s);
        auto server_proto = net::tls_stream_get_alpn_selected(s_s);
        LOG_INFO(VALUE(server_proto));
    });
    auto ep = srv->getsockname();
    LOG_INFO("Listen at `", ep);


    // Client side
    // Build ALPN Protos buf and set to client
    auto ctx_c = net::new_tls_context(cert_str, key_str, passphrase_str);
    DEFER(delete ctx_c);
    auto ret = ctx_c->set_alpn_protos({"h2", "http/1.1"});
    LOG_INFO("set_alpn_protos `", ret);
    auto cli =
        net::new_tls_client(ctx_c, net::new_tcp_socket_client(), true);
    DEFER(delete cli);

    auto port = ep.port;
    auto s_c = cli->connect(net::EndPoint("127.0.0.1", port));
    DEFER(delete s_c);
    // The client handshake is lazy (deferred to the first I/O), so drive it with a
    // write before querying the negotiated ALPN.
    char probe = 'x';
    s_c->write(&probe, 1);
    auto cli_proto = net::tls_stream_get_alpn_selected(s_c);
    LOG_INFO(VALUE(cli_proto));
    EXPECT_TRUE(cli_proto == "http/1.1");
}

// Regression for #1292: the SNI hostname must be carried in the ClientHello.
// A plain-TCP server captures the first flight (the ClientHello) as raw bytes;
// the SNI extension is not encrypted, so the hostname must appear verbatim once
// the SNI has been set before the handshake.
//
// This exercises the wire format, not certificate verification, so it uses
// tls_stream_set_sni(): the peer here is a bare TCP socket that never presents a
// certificate at all.
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
static std::string g_captured_client_hello;
static photon::semaphore sni_sem(0);

static int sni_capture_handler(void*, net::ISocketStream* stream) {
    char buf[4096];
    auto n = stream->recv(buf, sizeof(buf));  // first flight == ClientHello
    if (n > 0) g_captured_client_hello.assign(buf, n);
    sni_sem.signal(1);
    return 0;
}

TEST(sni, hostname_in_client_hello) {
    g_captured_client_hello.clear();
    DEFER(photon::wait_all());
    auto srv = net::new_tcp_socket_server();  // plain TCP: just capture raw bytes
    DEFER(delete srv);
    ASSERT_EQ(0, srv->bind_v4localhost());
    ASSERT_EQ(0, srv->listen());
    srv->set_handler({&sni_capture_handler, nullptr});
    ASSERT_EQ(0, srv->start_loop(false));
    photon::thread_yield();
    auto ep = srv->getsockname();

    auto ctx = net::new_tls_context();
    DEFER(delete ctx);
    auto tcp_cli = net::new_tcp_socket_client();
    tcp_cli->timeout(1UL * 1000 * 1000);  // 1s, mandatory: bounds the handshake (the plain server never sends a ServerHello)
    auto cli = net::new_tls_client(ctx, tcp_cli, true);
    DEFER(delete cli);
    auto s = cli->connect(ep);
    ASSERT_NE(nullptr, s);
    DEFER(delete s);

    const char* kHost = "sni-probe.example.test";
    ASSERT_EQ(0, net::tls_stream_set_sni(s, kHost));
    char req = 'x';
    s->write(&req, 1);  // drives the client handshake -> sends the ClientHello
    sni_sem.wait(1);
    EXPECT_NE(std::string::npos, g_captured_client_hello.find(kHost));
}
#endif

// ==================== CA cert tests ====================

// Server handler that tolerates TLS handshake failure (for negative tests)
int s_handler_noassert(void*, net::ISocketStream* stream) {
    char buf[6];
    stream->read(buf, 6);  // may fail due to TLS handshake error, that's OK
    sem.signal(1);
    return 0;
}

TEST(ca_cert, pem_string) {
    auto server_ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    ASSERT_NE(server_ctx, nullptr);
    DEFER(delete server_ctx);
    DEFER(photon::wait_all());

    auto server = net::new_tls_server(server_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete server);
    ASSERT_EQ(0, server->bind_v4localhost());
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    ASSERT_EQ(0, server->start_loop(false));
    server->set_handler({s_handler, server_ctx});
    photon::thread_yield();

    // Client loads cert_str as CA
    auto client_ctx = net::new_tls_context();
    ASSERT_NE(client_ctx, nullptr);
    DEFER(delete client_ctx);
    ASSERT_EQ(0, client_ctx->set_ca_cert(cert_str));

    auto client = net::new_tls_client(client_ctx, net::new_tcp_socket_client(), true);
    DEFER(delete client);

    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    s_client_test(stream);
}

TEST(ca_cert, file_path) {
    auto fn = "/tmp/ca-cert-test-" + std::to_string(::getpid()) + ".pem";
    FILE* f = fopen(fn.c_str(), "w");
    ASSERT_NE(f, nullptr);
    fputs(cert_str, f);
    fclose(f);
    DEFER(unlink(fn.c_str()));

    auto server_ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    ASSERT_NE(server_ctx, nullptr);
    DEFER(delete server_ctx);
    DEFER(photon::wait_all());

    auto server = net::new_tls_server(server_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete server);
    ASSERT_EQ(0, server->bind_v4localhost());
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    ASSERT_EQ(0, server->start_loop(false));
    server->set_handler({s_handler, server_ctx});
    photon::thread_yield();

    auto client_ctx = net::new_tls_context();
    ASSERT_NE(client_ctx, nullptr);
    DEFER(delete client_ctx);
    ASSERT_EQ(0, client_ctx->set_ca_file(fn.c_str()));

    auto client = net::new_tls_client(client_ctx, net::new_tcp_socket_client(), true);
    DEFER(delete client);

    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    s_client_test(stream);
}

TEST(ca_cert, ca_path_directory) {
    auto dir = "/tmp/ca-path-test-" + std::to_string(::getpid());
    mkdir(dir.c_str(), 0755);
    DEFER(rmdir(dir.c_str()));

    // Compute subject hash and place cert as {hash}.0 (OpenSSL ca_path convention)
    auto bio = BIO_new_mem_buf((void*)cert_str, -1);
    auto x509 = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    ASSERT_NE(x509, nullptr);
    auto hash = X509_subject_name_hash(x509);
    X509_free(x509);

    char hash_name[256];
    snprintf(hash_name, sizeof(hash_name), "%s/%08lx.0", dir.c_str(), hash);
    FILE* f = fopen(hash_name, "w");
    ASSERT_NE(f, nullptr);
    fputs(cert_str, f);
    fclose(f);
    DEFER(unlink(hash_name));

    auto server_ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    ASSERT_NE(server_ctx, nullptr);
    DEFER(delete server_ctx);
    DEFER(photon::wait_all());

    auto server = net::new_tls_server(server_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete server);
    ASSERT_EQ(0, server->bind_v4localhost());
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    ASSERT_EQ(0, server->start_loop(false));
    server->set_handler({s_handler, server_ctx});
    photon::thread_yield();

    auto client_ctx = net::new_tls_context();
    ASSERT_NE(client_ctx, nullptr);
    DEFER(delete client_ctx);
    ASSERT_EQ(0, client_ctx->set_ca_file(nullptr, dir.c_str()));

    auto client = net::new_tls_client(client_ctx, net::new_tcp_socket_client(), true);
    DEFER(delete client);

    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    s_client_test(stream);
}

TEST(ca_cert, verify_fail_without_ca) {
    auto server_ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    ASSERT_NE(server_ctx, nullptr);
    DEFER(delete server_ctx);
    DEFER(photon::wait_all());

    auto server = net::new_tls_server(server_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete server);
    ASSERT_EQ(0, server->bind_v4localhost());
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    ASSERT_EQ(0, server->start_loop(false));
    server->set_handler({s_handler_noassert, server_ctx});
    photon::thread_yield();

    // PEER verify enabled, but no CA loaded. Handshake should fail on first write.
    auto client_ctx = net::new_tls_context();
    ASSERT_NE(client_ctx, nullptr);
    DEFER(delete client_ctx);
    client_ctx->set_verify_mode(net::VerifyMode::PEER);

    auto client = net::new_tls_client(client_ctx, net::new_tcp_socket_client(), true);
    DEFER(delete client);

    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    char buf[] = "Hello";
    auto ret = stream->write(buf, 6);
    EXPECT_LT(ret, 0);
    sem.wait(1);
}

TEST(ca_cert, verify_fail_wrong_ca) {
    auto wrong_ca_pem = generate_different_self_signed_cert();
    auto server_ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    ASSERT_NE(server_ctx, nullptr);
    DEFER(delete server_ctx);
    DEFER(photon::wait_all());

    auto server = net::new_tls_server(server_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete server);
    ASSERT_EQ(0, server->bind_v4localhost());
    ASSERT_EQ(0, server->listen());
    auto ep = server->getsockname();
    ASSERT_EQ(0, server->start_loop(false));
    server->set_handler({s_handler_noassert, server_ctx});
    photon::thread_yield();

    // Wrong CA: set_ca_cert succeeds (valid X509), but handshake fails
    auto client_ctx = net::new_tls_context();
    ASSERT_NE(client_ctx, nullptr);
    DEFER(delete client_ctx);
    ASSERT_EQ(0, client_ctx->set_ca_cert(wrong_ca_pem.c_str()));

    auto client = net::new_tls_client(client_ctx, net::new_tcp_socket_client(), true);
    DEFER(delete client);

    auto stream = client->connect(ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    char buf[] = "Hello";
    auto ret = stream->write(buf, 6);
    EXPECT_LT(ret, 0);
    sem.wait(1);
}

TEST(ca_cert, invalid_ca_cert) {
    auto ctx = net::new_tls_context();
    ASSERT_NE(ctx, nullptr);
    DEFER(delete ctx);

    EXPECT_NE(0, ctx->set_ca_cert("not a valid cert"));
    EXPECT_NE(0, ctx->set_ca_cert(""));
}

TEST(ca_cert, invalid_ca_file) {
    auto ctx = net::new_tls_context();
    ASSERT_NE(ctx, nullptr);
    DEFER(delete ctx);

    EXPECT_NE(0, ctx->set_ca_file("/tmp/nonexistent-ca-file.pem"));
    EXPECT_NE(0, ctx->set_ca_file(nullptr, nullptr));
}

// ==================== hostname verification tests ====================

// Runs a TLS server presenting `chain`, and has a client that trusts the chain's
// CA connect asking for `request_host`. Returns whether the handshake was
// accepted, which is what the name check governs.
//
// A chain check alone cannot answer this question: in every case here the
// certificate is genuinely signed by the CA the client trusts. What differs is
// only who it was issued to.
static bool handshake_accepted(const TestCertChain& chain, const char* request_host,
                               bool sni_only = false, bool verify_hostname = true) {
    auto server_ctx = net::new_tls_context(chain.cert_pem.c_str(), chain.key_pem.c_str(), nullptr);
    EXPECT_NE(nullptr, server_ctx);
    if (!server_ctx) return false;
    DEFER(delete server_ctx);
    DEFER(photon::wait_all());

    auto server = net::new_tls_server(server_ctx, net::new_tcp_socket_server(), true);
    DEFER(delete server);
    EXPECT_EQ(0, server->bind_v4localhost());
    EXPECT_EQ(0, server->listen());
    auto ep = server->getsockname();
    EXPECT_EQ(0, server->start_loop(false));
    server->set_handler({s_handler_noassert, server_ctx});
    photon::thread_yield();

    auto client_ctx = net::new_tls_context();
    EXPECT_NE(nullptr, client_ctx);
    if (!client_ctx) return false;
    DEFER(delete client_ctx);
    if (!verify_hostname) client_ctx->set_verify_hostname(false);
    // set_ca_cert() turns on SSL_VERIFY_PEER as a side effect, so it must come
    // after any set_verify_mode() call, not before.
    EXPECT_EQ(0, client_ctx->set_ca_cert(chain.ca_pem.c_str()));

    auto client = net::new_tls_client(client_ctx, net::new_tcp_socket_client(), true);
    DEFER(delete client);
    auto stream = client->connect(ep);
    EXPECT_NE(nullptr, stream);
    if (!stream) return false;
    DEFER(delete stream);

    int ret = sni_only ? net::tls_stream_set_sni(stream, request_host)
                       : net::tls_stream_set_hostname(stream, request_host);
    EXPECT_EQ(0, ret);
    if (ret < 0) return false;

    char buf[] = "Hello";  // drives the deferred client handshake
    bool accepted = stream->write(buf, 6) == 6;
    sem.wait(1);
    return accepted;
}

// The reported vulnerability: the client asks for registry.example.com and the
// server presents a CA-signed certificate for a name the attacker controls.
// SSL_VERIFY_PEER alone accepts this, because a valid chain says the certificate
// is genuine, not who it belongs to.
TEST(verify_host, mismatch_is_rejected) {
    auto chain = generate_ca_signed_cert({"DNS:attacker-controlled.example.com"},
                                         "attacker-controlled.example.com");
    EXPECT_FALSE(handshake_accepted(chain, "registry.example.com"));
}

TEST(verify_host, match_is_accepted) {
    auto chain = generate_ca_signed_cert({"DNS:registry.example.com"}, "registry.example.com");
    EXPECT_TRUE(handshake_accepted(chain, "registry.example.com"));
}

// With no SAN present, name matching falls back to the subject common name.
TEST(verify_host, common_name_fallback) {
    auto chain = generate_ca_signed_cert({}, "registry.example.com");
    EXPECT_TRUE(handshake_accepted(chain, "registry.example.com"));
    auto other = generate_ca_signed_cert({}, "attacker-controlled.example.com");
    EXPECT_FALSE(handshake_accepted(other, "registry.example.com"));
}

// A wildcard covers exactly one label, so *.example.com must not stand in for
// a.b.example.com.
TEST(verify_host, wildcard_matches_single_label) {
    auto chain = generate_ca_signed_cert({"DNS:*.example.com"}, "*.example.com");
    EXPECT_TRUE(handshake_accepted(chain, "registry.example.com"));
    EXPECT_FALSE(handshake_accepted(chain, "a.b.example.com"));
    EXPECT_FALSE(handshake_accepted(chain, "example.com"));
}

// IP literals are matched against iPAddress SANs. X509_check_host() never looks
// at those, so this needs its own path and its own test.
TEST(verify_host, ip_san) {
    auto chain = generate_ca_signed_cert({"IP:127.0.0.1"}, "127.0.0.1");
    EXPECT_TRUE(handshake_accepted(chain, "127.0.0.1"));

    // A dNSName spelling of the address does not satisfy an IP request, and a
    // certificate for another address does not either.
    auto dns_only = generate_ca_signed_cert({"DNS:127.0.0.1"}, "127.0.0.1");
    EXPECT_FALSE(handshake_accepted(dns_only, "127.0.0.1"));
    auto other_ip = generate_ca_signed_cert({"IP:10.0.0.1"}, "10.0.0.1");
    EXPECT_FALSE(handshake_accepted(other_ip, "127.0.0.1"));
}

// The opt-outs both keep the pre-existing behavior: SNI is sent, the name is not
// checked, and a mismatched certificate is accepted.
TEST(verify_host, optout_accepts_mismatch) {
    auto chain = generate_ca_signed_cert({"DNS:attacker-controlled.example.com"},
                                         "attacker-controlled.example.com");
    EXPECT_TRUE(handshake_accepted(chain, "registry.example.com", true));
    EXPECT_TRUE(handshake_accepted(chain, "registry.example.com", false, false));
}

// Under VerifyMode::NONE, SSL_set1_host would be silently inert. Requesting a
// hostname there is refused rather than quietly ignored.
TEST(verify_host, none_verify_mode_is_refused) {
    DEFER(photon::wait_all());
    auto srv = net::new_tcp_socket_server();
    DEFER(delete srv);
    ASSERT_EQ(0, srv->bind_v4localhost());
    ASSERT_EQ(0, srv->listen());
    auto ep = srv->getsockname();

    auto ctx = net::new_tls_context();  // defaults to VerifyMode::NONE
    DEFER(delete ctx);
    auto tcp_cli = net::new_tcp_socket_client();
    tcp_cli->timeout(1UL * 1000 * 1000);
    auto cli = net::new_tls_client(ctx, tcp_cli, true);
    DEFER(delete cli);
    auto s = cli->connect(ep);
    ASSERT_NE(nullptr, s);
    DEFER(delete s);

    EXPECT_EQ(-1, net::tls_stream_set_hostname(s, "registry.example.com"));
    EXPECT_EQ(EINVAL, errno);

    // The opt-outs make the intent explicit and are accepted.
    EXPECT_EQ(0, net::tls_stream_set_sni(s, "registry.example.com"));
    ASSERT_EQ(0, ctx->set_verify_hostname(false));
    EXPECT_EQ(0, net::tls_stream_set_hostname(s, "registry.example.com"));
}

TEST(verify_host, invalid_arguments) {
    DEFER(photon::wait_all());
    auto srv = net::new_tcp_socket_server();
    DEFER(delete srv);
    ASSERT_EQ(0, srv->bind_v4localhost());
    ASSERT_EQ(0, srv->listen());
    auto ep = srv->getsockname();

    auto ctx = net::new_tls_context();
    DEFER(delete ctx);
    auto tcp_cli = net::new_tcp_socket_client();
    tcp_cli->timeout(1UL * 1000 * 1000);
    auto cli = net::new_tls_client(ctx, tcp_cli, true);
    DEFER(delete cli);
    auto s = cli->connect(ep);
    ASSERT_NE(nullptr, s);
    DEFER(delete s);

    EXPECT_EQ(-1, net::tls_stream_set_hostname(s, nullptr));
    EXPECT_EQ(-1, net::tls_stream_set_hostname(s, ""));
    EXPECT_EQ(-1, net::tls_stream_set_sni(s, nullptr));

    // A plain TCP stream has no name to bind, so asking is an error rather than
    // a silent no-op that leaves the caller believing it verified something.
    auto plain = net::new_tcp_socket_client();
    DEFER(delete plain);
    plain->timeout(1UL * 1000 * 1000);
    auto ps = plain->connect(ep);
    ASSERT_NE(nullptr, ps);
    DEFER(delete ps);
    EXPECT_EQ(-1, net::tls_stream_set_hostname(ps, "registry.example.com"));

    // A server does not check a peer hostname, and the failure should say so
    // rather than blame the verify mode.
    auto srv_side = net::new_tls_stream(ctx, ps, net::SecurityRole::Server, false);
    ASSERT_NE(nullptr, srv_side);
    DEFER(delete srv_side);
    EXPECT_EQ(-1, net::tls_stream_set_hostname(srv_side, "registry.example.com"));
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
