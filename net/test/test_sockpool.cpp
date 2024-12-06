#include <gtest/gtest.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

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
    server->bind();
    server->listen();
    auto ep = photon::net::EndPoint(photon::net::IPAddr("127.0.0.1"),
                                    server->getsockname().port);
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
    task(client, ep);
    EXPECT_EQ(1, conncount);
}

TEST(Socket, pooled_multisock) {
    auto server = photon::net::new_tcp_socket_server();
    int conncount = 0;
    server->bind();
    server->listen();
    auto ep = photon::net::EndPoint(photon::net::IPAddr("127.0.0.1"),
                                    server->getsockname().port);
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
    server->bind();
    server->listen();
    auto ep = photon::net::EndPoint(photon::net::IPAddr("127.0.0.1"),
                                    server->getsockname().port);
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
    std::vector<photon::join_handle*> jhs;
    for (int i = 0; i < 1; i++) {
        jhs.emplace_back(photon::thread_enable_join(
            photon::thread_create11(task, client, ep)));
    }
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
    DEFER(photon::thread_usleep(100UL * 1000));
    auto server = photon::net::new_tcp_socket_server();
    int conncount = 0;
    server->bind();
    server->listen();
    auto ep = photon::net::EndPoint(photon::net::IPAddr("127.0.0.1"),
                                    server->getsockname().port);
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
    DEFER(delete server);
    auto client = photon::net::new_tcp_socket_pool(
        photon::net::new_tcp_socket_client(),
        1UL * 1000 * 1000, true);  // release every 1 sec
    DEFER(delete client);
    std::vector<photon::join_handle*> jhs;
    for (int i = 0; i < 4; i++) {
        jhs.emplace_back(photon::thread_enable_join(
            photon::thread_create11(stask, client, ep)));
    }
    for (auto j : jhs) {
        photon::thread_join(j);
    }
    EXPECT_EQ(4, conncount);
    photon::thread_usleep(2100 * 1000);
    EXPECT_LT(conncount, 4);
}

int main(int argc, char** arg) {
    photon::init();
    DEFER(photon::fini());

    ::testing::InitGoogleTest(&argc, arg);

    RUN_ALL_TESTS();
}