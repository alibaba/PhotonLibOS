#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/tcp.h>
#ifdef __APPLE__
#define TCP_KEEPIDLE  TCP_KEEPALIVE
#define SOL_TCP       IPPROTO_TCP
#endif
#endif
#include <string_view>
#include "../../test/gtest.h"

template<typename Pred>
static bool poll_until(Pred pred, uint64_t timeout_us, uint64_t interval_us = 100'000) {
    uint64_t waited = 0;
    while (waited < timeout_us) {
        if (pred()) return true;
        auto sleep = std::min(interval_us, timeout_us - waited);
        photon::thread_usleep(sleep);
        waited += sleep;
    }
    return pred();
}

void task(photon::net::ISocketClient* client, photon::net::EndPoint ep) {
    photon::net::ISocketStream* st = nullptr;
    for (int i = 0; i < 100; i++) {
        st = client->connect(ep);
        ASSERT_NE(st, nullptr);
        ssize_t ret = 0;
        while (ret != 4) {
            ret = st->write("TEST", 4);
            if (ret < 0) {
                LOG_DEBUG(VALUE(ret), VALUE(errno));
                delete st;
                st = client->connect(ep);
            }
        }
        st->skip_read(4);
        photon::thread_yield();
        delete st;
    }
}

TEST(Socket, pooled) {
    auto server = photon::net::new_tcp_socket_server();
    int conncount = 0;
    server->bind_v4localhost();
    server->listen();
    auto handler = [&](photon::net::ISocketStream* stream) {
        LOG_INFO("Accept new connection `", stream);
        DEFER(LOG_INFO("Done connection `", stream));
        conncount++;
        char buf[4];
        while (stream->read(buf, 4) > 0) stream->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto client =
        photon::net::new_tcp_socket_pool(photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete client);
    task(client, server->getsockname());
    EXPECT_EQ(1, conncount);
}

TEST(Socket, pooled_multisock) {
    auto server = photon::net::new_tcp_socket_server();
    int conncount = 0;
    server->bind_v4localhost();
    server->listen();
    auto handler = [&](photon::net::ISocketStream* stream) {
        LOG_INFO("Accept new connection `", stream);
        DEFER(LOG_INFO("Done connection `", stream));
        conncount++;
        char buf[4];
        while (stream->read(buf, 4) > 0) stream->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto client =
        photon::net::new_tcp_socket_pool(photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete client);
    auto ep = server->getsockname();
    std::vector<photon::join_handle*> jhs;
    for (int i = 0; i < 5; i++) {
        jhs.emplace_back(photon::thread_enable_join(
            photon::thread_create11(task, client, ep)));
    }
    for (auto j : jhs) {
        photon::thread_join(j);
    }
    EXPECT_EQ(5, conncount);
}

TEST(Socket, pooled_multisock_serverclose) {
    auto server = photon::net::new_tcp_socket_server();
    int conncount = 0;
    server->bind_v4localhost();
    server->listen();
    auto handler = [&](photon::net::ISocketStream* stream) {
        LOG_INFO("Accept new connection `", stream);
        DEFER(LOG_INFO("Done connection `", stream));
        conncount++;
        char buf[4];
        size_t cnt = 0;
        ssize_t ret = 0;
        while ((ret = stream->recv(buf, 4)) > 0) {
            stream->write("TEST", 4);
            if (cnt + ret >= 100) {
                break;
            } else
                cnt += ret;
        }
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto client =
        photon::net::new_tcp_socket_pool(photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete client);
    auto ep = server->getsockname();
    std::vector<photon::join_handle*> jhs;
    jhs.emplace_back(photon::thread_enable_join(
        photon::thread_create11(task, client, ep)));
    for (auto j : jhs) {
        photon::thread_join(j);
    }
    EXPECT_EQ(4, conncount);
}

void stask(photon::net::ISocketClient* client, photon::net::EndPoint ep) {
    photon::net::ISocketStream* st = nullptr;
    st = client->connect(ep);
    ASSERT_NE(st, nullptr);
    ssize_t ret = 0;
    while (ret != 4) {
        ret = st->write("TEST", 4);
        if (ret < 0) {
            LOG_DEBUG(VALUE(ret), VALUE(errno));
            delete st;
            st = client->connect(ep);
        }
    }
    st->skip_read(4);
    photon::thread_yield();
    delete st;
}

TEST(Socket, pooled_expiration) {
    DEFER(photon::thread_usleep(100ULL * 1000));
    auto server = photon::net::new_tcp_socket_server();
    int conncount = 0;
    server->bind_v4localhost();
    server->listen();
    auto handler = [&](photon::net::ISocketStream* stream) {
        LOG_INFO("Accept new connection `", stream);
        DEFER(LOG_INFO("Done connection `", stream));
        conncount++;
        char buf[4];
        size_t cnt = 0;
        ssize_t ret = 0;
        while ((ret = stream->recv(buf, 4)) > 0) {
            stream->write("TEST", 4);
            if (cnt + ret >= 100) {
                break;
            } else
                cnt += ret;
        }
        conncount--;
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER({
        delete server;  // dtor doesn't guarantee completion of connections and their serving threads
        poll_until([&] { return conncount == 0; }, 10'000'000);
    });
    auto client = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(),
        2ULL * 1000 * 1000, true);  // release every 2 sec
    DEFER(delete client);
    auto ep = server->getsockname();
    std::vector<photon::join_handle*> jhs;
    for (int i = 0; i < 4; i++) {
        jhs.emplace_back(photon::thread_enable_join(
            photon::thread_create11(stask, client, ep)));
    }
    for (auto j : jhs) {
        photon::thread_join(j);
    }
    EXPECT_EQ(4, conncount);
    poll_until([&] { return conncount < 4; }, 10'000'000);
    EXPECT_LT(conncount, 4);
}

static void roundtrip(photon::net::ISocketStream* st) {
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(4, st->write("TEST", 4));
    EXPECT_TRUE(st->skip_read(4));
}

TEST(Socket, pooled_key_connect) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    auto ep = server->getsockname();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);

    for (int i = 0; i < 5; i++) {
        auto* st = sp->connect(std::string_view("K"), ep);
        roundtrip(st);
        delete st;
    }
    EXPECT_EQ(1, conncount);

    for (int i = 0; i < 3; i++) {
        auto* st = sp->connect(std::string_view("K2"), ep);
        roundtrip(st);
        delete st;
    }
    EXPECT_EQ(2, conncount);
}

TEST(Socket, pooled_key_isolation) {
    auto server1 = photon::net::new_tcp_socket_server();
    server1->bind_v4localhost();
    server1->listen();
    DEFER(delete server1);

    auto server2 = photon::net::new_tcp_socket_server();
    server2->bind_v4localhost();
    server2->listen();
    DEFER(delete server2);

    int cnt1 = 0, cnt2 = 0;
    auto handler1 = [&](photon::net::ISocketStream* s) -> int {
        cnt1++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("AAA", 4);
        return 0;
    };
    auto handler2 = [&](photon::net::ISocketStream* s) -> int {
        cnt2++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("BBB", 4);
        return 0;
    };
    server1->set_handler(handler1);
    server2->set_handler(handler2);
    server1->start_loop();
    server2->start_loop();

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);

    auto ep1 = server1->getsockname();
    auto ep2 = server2->getsockname();

    auto* s1 = sp->connect(std::string_view("A"), ep1);
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(4, s1->write("TEST", 4));
    char buf[4] = {};
    EXPECT_EQ(4, s1->read(buf, 4));
    EXPECT_STREQ("AAA", buf);
    delete s1;

    auto* s2 = sp->connect(std::string_view("A"), ep1);
    roundtrip(s2);
    delete s2;
    EXPECT_EQ(1, cnt1);

    auto* s3 = sp->connect(std::string_view("B"), ep1);
    roundtrip(s3);
    delete s3;
    EXPECT_EQ(2, cnt1);

    auto* s4 = sp->connect(std::string_view("A"), ep2);
    ASSERT_NE(s4, nullptr);
    EXPECT_EQ(4, s4->write("TEST", 4));
    memset(buf, 0, sizeof(buf));
    EXPECT_EQ(4, s4->read(buf, 4));
    EXPECT_STREQ("AAA", buf);
    delete s4;
    EXPECT_EQ(2, cnt1);
    EXPECT_EQ(0, cnt2);
}

TEST(Socket, pooled_domain_port) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);
    auto ep = server->getsockname();

    for (int i = 0; i < 3; i++) {
        auto* st = sp->connect(std::string_view("127.0.0.1"), ep.port);
        roundtrip(st);
        delete st;
    }
    EXPECT_EQ(1, conncount);
}

TEST(Socket, pooled_key_connector) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto raw_client = photon::net::new_tcp_socket_client();
    auto pool = photon::net::new_tcp_socket_pool(raw_client, -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);
    auto ep = server->getsockname();

    int connector_calls = 0;
    auto connector = [&]() -> photon::net::ISocketStream* {
        connector_calls++;
        return raw_client->connect(ep);
    };

    auto* s1 = sp->connect(std::string_view("C"), connector);
    roundtrip(s1);
    delete s1;
    EXPECT_EQ(1, connector_calls);
    EXPECT_EQ(1, conncount);

    auto* s2 = sp->connect(std::string_view("C"), connector);
    roundtrip(s2);
    delete s2;
    EXPECT_EQ(1, connector_calls);
    EXPECT_EQ(1, conncount);

    auto* s3 = sp->connect(std::string_view("D"), connector);
    roundtrip(s3);
    delete s3;
    EXPECT_EQ(2, connector_calls);
    EXPECT_EQ(2, conncount);
}

TEST(Socket, pooled_connector_failure_refcnt) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto raw_client = photon::net::new_tcp_socket_client();
    auto pool = photon::net::new_tcp_socket_pool(raw_client, -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);
    auto ep = server->getsockname();

    int connector_calls = 0;
    auto connector = [&]() -> photon::net::ISocketStream* {
        connector_calls++;
        return raw_client->connect(ep);
    };

    auto* s1 = sp->connect(std::string_view("E"), connector);
    roundtrip(s1);
    s1->shutdown(ShutdownHow::ReadWrite);
    delete s1;
    EXPECT_EQ(1, connector_calls);
    EXPECT_EQ(1, conncount);

    int fail_count = 0;
    auto fail_connector = [&]() -> photon::net::ISocketStream* {
        fail_count++;
        return nullptr;
    };
    auto* s2 = sp->connect(std::string_view("E"), fail_connector);
    EXPECT_EQ(s2, nullptr);
    EXPECT_EQ(1, fail_count);

    auto* s3 = sp->connect(std::string_view("E"), connector);
    roundtrip(s3);
    s3->shutdown(ShutdownHow::ReadWrite);
    delete s3;
    EXPECT_EQ(2, connector_calls);
    EXPECT_EQ(2, conncount);

    for (int i = 0; i < 3; i++) {
        auto* s = sp->connect(std::string_view("E"), fail_connector);
        EXPECT_EQ(s, nullptr);
    }
    EXPECT_EQ(4, fail_count);

    auto* s4 = sp->connect(std::string_view("E"), connector);
    roundtrip(s4);
    delete s4;
    EXPECT_EQ(3, connector_calls);
    EXPECT_EQ(3, conncount);
}

TEST(Socket, pooled_shutdown_close_drop) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);
    auto ep = server->getsockname();

    auto* st = sp->connect(std::string_view("S"), ep);
    roundtrip(st);
    delete st;
    EXPECT_EQ(1, conncount);

    {
        auto* s = sp->connect(std::string_view("S"), ep);
        roundtrip(s);
        s->shutdown(ShutdownHow::ReadWrite);
        delete s;
    }
    photon::thread_usleep(1000 * 1000);

    {
        auto* s = sp->connect(std::string_view("S"), ep);
        roundtrip(s);
        s->close();
        delete s;
    }
    photon::thread_usleep(1000 * 1000);

    auto* s2 = sp->connect(std::string_view("S"), ep);
    roundtrip(s2);
    delete s2;
    EXPECT_GE(conncount, 2);
}

TEST(Socket, pooled_concurrent_keys) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);
    auto ep = server->getsockname();

    auto worker = [&](const char* key) {
        for (int i = 0; i < 50; i++) {
            auto* st = sp->connect(std::string_view(key), ep);
            ASSERT_NE(st, nullptr);
            EXPECT_EQ(4, st->write("TEST", 4));
            EXPECT_TRUE(st->skip_read(4));
            delete st;
            photon::thread_yield();
        }
    };

    std::vector<photon::join_handle*> jhs;
    for (int i = 0; i < 3; i++) {
        jhs.emplace_back(photon::thread_enable_join(
            photon::thread_create11(worker, (const char*)"X")));
    }
    for (int i = 0; i < 3; i++) {
        jhs.emplace_back(photon::thread_enable_join(
            photon::thread_create11(worker, (const char*)"Y")));
    }
    for (auto* j : jhs) photon::thread_join(j);

    EXPECT_EQ(6, conncount);
}

TEST(Socket, pooled_server_close_reuse) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        if (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);
    auto sp = pool;
    auto ep = server->getsockname();

    for (int i = 0; i < 5; i++) {
        auto* st = sp->connect(std::string_view("R"), ep);
        roundtrip(st);
        delete st;
        photon::thread_usleep(1000 * 1000);
    }
    EXPECT_EQ(5, conncount);
}

TEST(Socket, pooled_multiple_pools) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    int conn_alive = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        conn_alive++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        conn_alive--;
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER({
        delete server;  // dtor doesn't guarantee completion of connections and their serving threads
        poll_until([&] { return conncount == 0; }, 10'000'000);
    });
    auto ep = server->getsockname();

    auto pool1 = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), 1000 * 1000, true);
    DEFER(delete pool1);
    auto pool2 = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool2);

    auto* s1 = pool1->connect(ep);
    roundtrip(s1);
    delete s1;

    auto* s2 = pool2->connect(ep);
    roundtrip(s2);
    delete s2;

    EXPECT_EQ(2, conncount);

    poll_until([&] { return conn_alive < 2; }, 10'000'000);

    auto* s3 = pool1->connect(ep);
    roundtrip(s3);
    delete s3;
    EXPECT_EQ(3, conncount);

    auto* s4 = pool2->connect(ep);
    roundtrip(s4);
    delete s4;
    EXPECT_EQ(3, conncount);
}

TEST(Socket, pooled_read_eof_drop) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        s->write("EOF!", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);
    auto ep = server->getsockname();

    {
        auto* st = sp->connect(std::string_view("EOF"), ep);
        ASSERT_NE(st, nullptr);
        char buf[4];
        EXPECT_EQ(4, st->read(buf, 4));
        ssize_t ret = st->read(buf, 1);
        EXPECT_EQ(0, ret);
        delete st;
    }
    photon::thread_usleep(1000 * 1000);

    {
        auto* st = sp->connect(std::string_view("EOF"), ep);
        ASSERT_NE(st, nullptr);
        char buf[4];
        EXPECT_EQ(4, st->read(buf, 4));
        delete st;
    }
    EXPECT_EQ(2, conncount);
}

TEST(Socket, pooled_dead_idle_recovery) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        if (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);
    auto ep = server->getsockname();

    for (int i = 0; i < 5; i++) {
        auto* st = sp->connect(std::string_view("D"), ep);
        roundtrip(st);
        delete st;
        photon::thread_usleep(1000 * 1000);
    }
    EXPECT_EQ(5, conncount);
}

TEST(Socket, pooled_key_domain_port) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);
    auto ep = server->getsockname();

    for (int i = 0; i < 3; i++) {
        auto* st = sp->connect(std::string_view("my-key"),
                               std::string_view("127.0.0.1"), ep.port);
        roundtrip(st);
        delete st;
    }
    EXPECT_EQ(1, conncount);

    for (int i = 0; i < 2; i++) {
        auto* st = sp->connect(std::string_view("other-key"),
                               std::string_view("127.0.0.1"), ep.port);
        roundtrip(st);
        delete st;
    }
    EXPECT_EQ(2, conncount);
}

TEST(Socket, pooled_heartbeater_success) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    int conn_alive = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        conn_alive++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        conn_alive--;
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto ep = server->getsockname();

    int hb_count = 0;
    auto heartbeater = [&](photon::net::ISocketStream*) -> int {
        hb_count++;
        return 0;
    };

    auto raw_client = photon::net::new_tcp_socket_client();
    photon::net::SocketPoolArgs args;
    args.sockclient = raw_client;
    args.client_ownership = true;
    args.heartbeater = heartbeater;
    args.heartbeat_interval = 300'000;
    args.expiration = -1ULL;
    auto pool = photon::net::new_tcp_socket_pool(args);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);

    auto* s1 = sp->connect(std::string_view("HB"), ep);
    roundtrip(s1);
    delete s1;

    poll_until([&] { return hb_count > 0; }, 10'000'000);
    EXPECT_GT(hb_count, 0);

    auto* s2 = sp->connect(std::string_view("HB"), ep);
    roundtrip(s2);
    delete s2;

    auto* s3 = sp->connect(std::string_view("HB"), ep);
    roundtrip(s3);
    delete s3;

    EXPECT_EQ(1, conncount);

    delete pool;
    pool = nullptr;
    poll_until([&] { return conn_alive == 0; }, 5'000'000);
    EXPECT_EQ(0, conn_alive);
}

TEST(Socket, pooled_heartbeater_failure) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    int conn_alive = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        conn_alive++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        conn_alive--;
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto ep = server->getsockname();

    auto heartbeater = [&](photon::net::ISocketStream*) -> int {
        return -1;
    };

    auto raw_client = photon::net::new_tcp_socket_client();
    photon::net::SocketPoolArgs args;
    args.sockclient = raw_client;
    args.client_ownership = true;
    args.heartbeater = heartbeater;
    args.heartbeat_interval = 300'000;
    args.expiration = -1ULL;
    auto pool = photon::net::new_tcp_socket_pool(args);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);

    auto* s1 = sp->connect(std::string_view("HBF"), ep);
    roundtrip(s1);
    delete s1;
    EXPECT_EQ(1, conncount);

    poll_until([&] { return conn_alive == 0; }, 10'000'000);
    EXPECT_EQ(0, conn_alive);

    auto* s2 = sp->connect(std::string_view("HBF"), ep);
    roundtrip(s2);
    delete s2;
    EXPECT_EQ(2, conncount);

    poll_until([&] { return conn_alive == 0; }, 10'000'000);
    EXPECT_EQ(0, conn_alive);

    auto* s3 = sp->connect(std::string_view("HBF"), ep);
    roundtrip(s3);
    delete s3;
    EXPECT_EQ(3, conncount);

    poll_until([&] { return conn_alive == 0; }, 10'000'000);
    EXPECT_EQ(0, conn_alive);
}

TEST(Socket, pooled_heartbeater_multiple_keys) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    int conn_alive = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        conn_alive++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        conn_alive--;
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto ep = server->getsockname();

    int hb_count = 0;
    auto heartbeater = [&](photon::net::ISocketStream*) -> int {
        hb_count++;
        return 0;
    };

    auto raw_client = photon::net::new_tcp_socket_client();
    photon::net::SocketPoolArgs args;
    args.sockclient = raw_client;
    args.client_ownership = true;
    args.heartbeater = heartbeater;
    args.heartbeat_interval = 300'000;
    args.expiration = -1ULL;
    auto pool = photon::net::new_tcp_socket_pool(args);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);

    auto* s1 = sp->connect(std::string_view("HK1"), ep);
    roundtrip(s1);
    delete s1;

    auto* s2 = sp->connect(std::string_view("HK2"), ep);
    roundtrip(s2);
    delete s2;

    poll_until([&] { return hb_count >= 2; }, 10'000'000);
    EXPECT_GE(hb_count, 2);

    auto* s3 = sp->connect(std::string_view("HK1"), ep);
    roundtrip(s3);
    delete s3;

    auto* s4 = sp->connect(std::string_view("HK2"), ep);
    roundtrip(s4);
    delete s4;

    poll_until([&] { return conncount == 2; }, 5'000'000);
    EXPECT_EQ(2, conncount);

    delete pool;
    pool = nullptr;
    poll_until([&] { return conn_alive == 0; }, 5'000'000);
    EXPECT_EQ(0, conn_alive);
}

TEST(Socket, pooled_heartbeater_io) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    int conn_alive = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        conn_alive++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("PONG", 4);
        conn_alive--;
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto ep = server->getsockname();

    int hb_count = 0;
    auto heartbeater = [&](photon::net::ISocketStream* s) -> int {
        if (s->write("PING", 4) != 4) return -1;
        char buf[4];
        if (s->read(buf, 4) != 4) return -1;
        if (memcmp(buf, "PONG", 4) != 0) return -1;
        hb_count++;
        return 0;
    };

    auto raw_client = photon::net::new_tcp_socket_client();
    photon::net::SocketPoolArgs args;
    args.sockclient = raw_client;
    args.client_ownership = true;
    args.heartbeater = heartbeater;
    args.heartbeat_interval = 300'000;
    args.expiration = -1ULL;
    auto pool = photon::net::new_tcp_socket_pool(args);
    DEFER(delete pool);
    auto* sp = static_cast<photon::net::ISocketPool*>(pool);

    auto* s1 = sp->connect(std::string_view("HBIO"), ep);
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(4, s1->write("PING", 4));
    char buf[4];
    EXPECT_EQ(4, s1->read(buf, 4));
    EXPECT_EQ(0, memcmp(buf, "PONG", 4));
    delete s1;

    poll_until([&] { return hb_count > 0; }, 10'000'000);
    EXPECT_GT(hb_count, 0);

    auto* s2 = sp->connect(std::string_view("HBIO"), ep);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(4, s2->write("PING", 4));
    EXPECT_EQ(4, s2->read(buf, 4));
    EXPECT_EQ(0, memcmp(buf, "PONG", 4));
    delete s2;

    poll_until([&] { return conncount == 1; }, 5'000'000);
    EXPECT_EQ(1, conncount);

    delete pool;
    pool = nullptr;
    poll_until([&] { return conn_alive == 0; }, 5'000'000);
    EXPECT_EQ(0, conn_alive);
}

TEST(Socket, pooled_args_ownership) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    int conncount = 0;
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        conncount++;
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto ep = server->getsockname();

    auto raw_client = photon::net::new_tcp_socket_client();
    photon::net::SocketPoolArgs args;
    args.sockclient = raw_client;
    args.client_ownership = true;
    auto pool = photon::net::new_tcp_socket_pool(args);
    DEFER(delete pool);

    auto* s = pool->connect(ep);
    roundtrip(s);
    delete s;
    EXPECT_EQ(1, conncount);

    auto* s2 = pool->connect(ep);
    roundtrip(s2);
    delete s2;
    EXPECT_EQ(1, conncount);
}

#ifndef _WIN32
static void verify_keepalive_params(photon::net::ISocketStream* st,
                                     int time_s, int intvl_s, int probes) {
    int idle = -1;
    EXPECT_EQ(0, st->getsockopt<int>(SOL_TCP, TCP_KEEPIDLE, &idle));
    EXPECT_EQ(time_s, idle);

    int intvl = -1;
    EXPECT_EQ(0, st->getsockopt<int>(SOL_TCP, TCP_KEEPINTVL, &intvl));
    EXPECT_EQ(intvl_s, intvl);

    int cnt = -1;
    EXPECT_EQ(0, st->getsockopt<int>(SOL_TCP, TCP_KEEPCNT, &cnt));
    EXPECT_EQ(probes, cnt);
}
#else
#ifndef SIO_KEEPALIVE_VALS
#define SIO_KEEPALIVE_VALS _WSAIOW(IOC_VENDOR, 4)
#endif
struct tcp_keepalive_test {
    u_long onoff;
    u_long keepalivetime;
    u_long keepaliveinterval;
};

static void verify_keepalive_params(photon::net::ISocketStream* st,
                                     int time_s, int intvl_s, int) {
    SOCKET s = (SOCKET)(intptr_t)st->get_underlay_fd();
    tcp_keepalive_test params = {};
    DWORD bytes_returned = 0;
    int ret = WSAIoctl(s, SIO_KEEPALIVE_VALS,
                       nullptr, 0,
                       &params, sizeof(params),
                       &bytes_returned, nullptr, nullptr);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1u, params.onoff);
    EXPECT_EQ((u_long)(time_s * 1000), params.keepalivetime);
    EXPECT_EQ((u_long)(intvl_s * 1000), params.keepaliveinterval);
}
#endif

TEST(Socket, pooled_keepalive_default) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto ep = server->getsockname();

    auto pool = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(), -1, true);
    DEFER(delete pool);

    auto* st = pool->connect(ep);
    ASSERT_NE(st, nullptr);
    int keepalive = -1;
    EXPECT_EQ(0, st->getsockopt<int>(SOL_SOCKET, SO_KEEPALIVE, &keepalive));
    EXPECT_EQ(0, keepalive);
    delete st;
}

TEST(Socket, pooled_keepalive_enabled) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto ep = server->getsockname();

    auto raw_client = photon::net::new_tcp_socket_client();
    photon::net::SocketPoolArgs args;
    args.sockclient = raw_client;
    args.client_ownership = true;
    args.enable_tcp_keepalive = true;
    auto pool = photon::net::new_tcp_socket_pool(args);
    DEFER(delete pool);

    auto* st = pool->connect(ep);
    ASSERT_NE(st, nullptr);

    int keepalive = -1;
    EXPECT_EQ(0, st->getsockopt<int>(SOL_SOCKET, SO_KEEPALIVE, &keepalive));
    EXPECT_NE(0, keepalive);

    verify_keepalive_params(st, 2, 2, 15);
    delete st;
}

TEST(Socket, pooled_keepalive_custom_values) {
    auto server = photon::net::new_tcp_socket_server();
    server->bind_v4localhost();
    server->listen();
    auto handler = [&](photon::net::ISocketStream* s) -> int {
        char buf[4];
        while (s->read(buf, 4) > 0) s->write("TEST", 4);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();
    DEFER(delete server);
    auto ep = server->getsockname();

    auto raw_client = photon::net::new_tcp_socket_client();
    photon::net::SocketPoolArgs args;
    args.sockclient = raw_client;
    args.client_ownership = true;
    args.enable_tcp_keepalive = true;
    args.tcp_keepalive_time = 30;
    args.tcp_keepalive_intvl = 10;
    args.tcp_keepalive_probes = 5;
    auto pool = photon::net::new_tcp_socket_pool(args);
    DEFER(delete pool);

    auto* st = pool->connect(ep);
    ASSERT_NE(st, nullptr);

    verify_keepalive_params(st, 30, 10, 5);
    delete st;
}

int main(int argc, char** arg) {
    photon::init();
    DEFER(photon::fini());

    ::testing::InitGoogleTest(&argc, arg);

    auto ret = RUN_ALL_TESTS();
    photon::thread_usleep(100 * 1000);
    return ret;
}