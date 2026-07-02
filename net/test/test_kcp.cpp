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

#include "../../test/gtest.h"

#include <atomic>
#include <cstring>
#include <string>

#include <photon/photon.h>
#include <photon/common/utility.h>
#include <photon/net/datagram_socket.h>
#include <photon/net/socket.h>
#include <photon/net/kcp.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>

using namespace photon;
using namespace net;

static const uint64_t TEST_TIMEOUT = 10 * 1000 * 1000;  // 10s in us

TEST(KcpSocket, BasicEcho) {
    auto server_udp = new_udp_socket();
    DEFER(delete server_udp);
    server_udp->bind_v4localhost();
    auto server_ep = server_udp->getsockname();

    auto server = new_kcp_socket_server(server_udp);
    DEFER(delete server);
    server->timeout(TEST_TIMEOUT);
    server->bind(server_ep);
    server->listen();
    auto handler = [&](ISocketStream* stream) -> int {
        char buf[4096];
        ssize_t n = stream->recv(buf, sizeof(buf));
        if (n > 0) stream->write(buf, n);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();

    auto client_udp = new_udp_socket();
    DEFER(delete client_udp);

    auto client = new_kcp_socket_client(client_udp);
    DEFER(delete client);

    auto stream = client->connect(server_ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);
    stream->timeout(TEST_TIMEOUT);

    const char* msg = "Hello KCP!";
    size_t len = strlen(msg);
    ASSERT_EQ((ssize_t)len, stream->write(msg, len));

    char buf[4096];
    ssize_t n = stream->recv(buf, sizeof(buf));
    EXPECT_EQ((ssize_t)len, n);
    EXPECT_EQ(0, memcmp(buf, msg, len));
}

TEST(KcpSocket, NoDelayOption) {
    auto server_udp = new_udp_socket();
    DEFER(delete server_udp);
    server_udp->bind_v4localhost();
    auto server_ep = server_udp->getsockname();

    auto server = new_kcp_socket_server(server_udp);
    DEFER(delete server);
    server->timeout(TEST_TIMEOUT);
    server->bind(server_ep);
    server->listen();
    auto handler = [&](ISocketStream* stream) -> int {
        char buf[4096];
        ssize_t n = stream->recv(buf, sizeof(buf));
        if (n > 0) stream->write(buf, n);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();

    auto client_udp = new_udp_socket();
    DEFER(delete client_udp);

    auto client = new_kcp_socket_client(client_udp);
    DEFER(delete client);

    int nodelay_opts[4] = {1, 100, 0, 0};  // nodelay, interval, resend, nc
    client->setsockopt(SOL_KCP, KCP_NODELAY_OPTS, nodelay_opts, sizeof(nodelay_opts));

    auto stream = client->connect(server_ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);
    stream->timeout(TEST_TIMEOUT);

    const char* msg = "nodelay test";
    size_t len = strlen(msg);
    stream->write(msg, len);

    char buf[4096];
    ssize_t n = stream->recv(buf, sizeof(buf));
    EXPECT_EQ((ssize_t)len, n);
    EXPECT_EQ(0, memcmp(buf, msg, len));
}

TEST(KcpSocket, MultipleConnections) {
    auto server_udp = new_udp_socket();
    DEFER(delete server_udp);
    server_udp->bind_v4localhost();
    auto server_ep = server_udp->getsockname();

    auto server = new_kcp_socket_server(server_udp);
    DEFER(delete server);
    server->timeout(TEST_TIMEOUT);
    server->bind(server_ep);
    server->listen();

    std::atomic<int> echo_count{0};
    auto handler = [&](ISocketStream* stream) -> int {
        char buf[4096];
        ssize_t n = stream->recv(buf, sizeof(buf));
        if (n > 0) stream->write(buf, n);
        echo_count.fetch_add(1, std::memory_order_release);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();

    const int NUM = 3;
    for (int i = 0; i < NUM; i++) {
        auto client_udp = new_udp_socket();
        auto client = new_kcp_socket_client(client_udp);
        auto stream = client->connect(server_ep);
        ASSERT_NE(nullptr, stream);
        stream->timeout(TEST_TIMEOUT);

        std::string msg = "msg_" + std::to_string(i);
        stream->write(msg.data(), msg.size());

        char buf[4096];
        ssize_t n = stream->recv(buf, sizeof(buf));
        EXPECT_EQ((ssize_t)msg.size(), n);
        EXPECT_EQ(0, memcmp(buf, msg.data(), msg.size()));

        stream->close();
        delete stream;
        delete client;
        delete client_udp;
    }

    while (echo_count.load(std::memory_order_acquire) < NUM)
        thread_usleep(10000);
}

TEST(KcpSocket, SetsockoptOnStream) {
    auto server_udp = new_udp_socket();
    DEFER(delete server_udp);
    server_udp->bind_v4localhost();
    auto server_ep = server_udp->getsockname();

    auto server = new_kcp_socket_server(server_udp);
    DEFER(delete server);
    server->timeout(TEST_TIMEOUT);
    server->bind(server_ep);
    server->listen();
    auto handler = [&](ISocketStream* stream) -> int {
        char buf[4096];
        ssize_t n = stream->recv(buf, sizeof(buf));
        if (n > 0) stream->write(buf, n);
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();

    auto client_udp = new_udp_socket();
    DEFER(delete client_udp);

    auto client = new_kcp_socket_client(client_udp);
    DEFER(delete client);

    auto stream = client->connect(server_ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);

    int nodelay_opts[4] = {1, 10, 2, 1};
    stream->setsockopt(SOL_KCP, KCP_NODELAY_OPTS, nodelay_opts, sizeof(nodelay_opts));

    int wnd_opts[2] = {64, 128};
    stream->setsockopt(SOL_KCP, KCP_WND_OPTS, wnd_opts, sizeof(wnd_opts));

    int opts[4] = {0};
    socklen_t opts_len = sizeof(opts);
    stream->getsockopt(SOL_KCP, KCP_NODELAY_OPTS, opts, &opts_len);
    EXPECT_EQ(1, opts[0]);
    EXPECT_EQ(10, opts[1]);
    EXPECT_EQ(2, opts[2]);
    EXPECT_EQ(1, opts[3]);

    int wopts[2] = {0};
    socklen_t wopts_len = sizeof(wopts);
    stream->getsockopt(SOL_KCP, KCP_WND_OPTS, wopts, &wopts_len);
    EXPECT_EQ(64, wopts[0]);
    EXPECT_EQ(128, wopts[1]);

    stream->timeout(TEST_TIMEOUT);

    const char* msg = "stream opts";
    size_t msglen = strlen(msg);
    stream->write(msg, msglen);

    char buf[4096];
    ssize_t n = stream->recv(buf, sizeof(buf));
    EXPECT_EQ((ssize_t)msglen, n);
    EXPECT_EQ(0, memcmp(buf, msg, msglen));
}

TEST(KcpSocket, LargeDataTransfer) {
    auto server_udp = new_udp_socket();
    DEFER(delete server_udp);
    server_udp->bind_v4localhost();
    auto server_ep = server_udp->getsockname();

    auto server = new_kcp_socket_server(server_udp);
    DEFER(delete server);
    server->timeout(TEST_TIMEOUT);
    server->bind(server_ep);
    server->listen();
    auto handler = [&](ISocketStream* stream) -> int {
        char buf[4096];
        while (true) {
            ssize_t n = stream->recv(buf, sizeof(buf));
            if (n <= 0) break;
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t r = stream->write(buf + sent, n - sent);
                if (r <= 0) break;
                sent += r;
            }
        }
        return 0;
    };
    server->set_handler(handler);
    server->start_loop();

    auto client_udp = new_udp_socket();
    DEFER(delete client_udp);

    auto client = new_kcp_socket_client(client_udp);
    DEFER(delete client);

    auto stream = client->connect(server_ep);
    ASSERT_NE(nullptr, stream);
    DEFER(delete stream);
    stream->timeout(TEST_TIMEOUT);

    const size_t DATA_SIZE = 4096;
    char send_buf[DATA_SIZE];
    char recv_buf[DATA_SIZE];
    for (size_t i = 0; i < DATA_SIZE; i++)
        send_buf[i] = (char)(i & 0xFF);

    ssize_t total_sent = 0;
    while (total_sent < (ssize_t)DATA_SIZE) {
        ssize_t n = stream->write(send_buf + total_sent, DATA_SIZE - total_sent);
        ASSERT_GT(n, 0);
        total_sent += n;
    }

    ssize_t total_recv = 0;
    while (total_recv < (ssize_t)DATA_SIZE) {
        ssize_t n = stream->read(recv_buf + total_recv, DATA_SIZE - total_recv);
        ASSERT_GT(n, 0);
        total_recv += n;
    }

    EXPECT_EQ(DATA_SIZE, (size_t)total_recv);
    EXPECT_EQ(0, memcmp(send_buf, recv_buf, DATA_SIZE));
}

int main(int argc, char** argv) {
    photon::init();
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
