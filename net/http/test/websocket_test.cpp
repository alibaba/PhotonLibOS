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
#include "../websocket.h"
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>
#include <photon/net/socket.h>
#include <photon/net/http/client.h>
#include <photon/net/http/server.h>
#include <photon/common/iovector.h>
#include <cstring>
#include <atomic>
#include "to_url.h"

using namespace photon;
using namespace photon::net;
using namespace photon::net::http;

TEST(websocket, constants) {
    // Test constant values
    ASSERT_EQ((int)WebSocketOpcode::Continuation, 0x0);
    ASSERT_EQ((int)WebSocketOpcode::Text, 0x1);
    ASSERT_EQ((int)WebSocketOpcode::Binary, 0x2);
    ASSERT_EQ((int)WebSocketOpcode::Close, 0x8);
    ASSERT_EQ((int)WebSocketOpcode::Ping, 0x9);
    ASSERT_EQ((int)WebSocketOpcode::Pong, 0xA);
    
    ASSERT_EQ((int)WebSocketCloseCode::NormalClosure, 1000);
    ASSERT_EQ((int)WebSocketCloseCode::GoingAway, 1001);
    ASSERT_EQ((int)WebSocketCloseCode::ProtocolError, 1002);
    ASSERT_EQ((int)WebSocketCloseCode::UnsupportedDataType, 1003);
    ASSERT_EQ((int)WebSocketCloseCode::InvalidFramePayloadData, 1007);
    ASSERT_EQ((int)WebSocketCloseCode::PolicyViolation, 1008);
    ASSERT_EQ((int)WebSocketCloseCode::MessageTooBig, 1009);
    ASSERT_EQ((int)WebSocketCloseCode::InternalServerError, 1011);
    
    LOG_INFO("WebSocket constants verified successfully");
}

// Echo handler for WebSocket server tests
int ws_echo_handler(void*, IWebSocketStream* ws) {
    LOG_INFO("ws_echo_handler: starting, ws=", (void*)ws);
    char buf[4096];
    WebSocketOpcode opcode;
    
    LOG_INFO("ws_echo_handler: checking is_closed");
    bool closed = ws->is_closed();
    LOG_INFO("ws_echo_handler: is_closed=", closed);
    
    while (!closed) {
        LOG_INFO("ws_echo_handler: calling recv_frame");
        auto len = ws->recv_frame(buf, sizeof(buf), &opcode);
        LOG_INFO("ws_echo_handler: recv_frame returned ", len);
        if (len < 0) {
            LOG_DEBUG("recv_frame returned error, closing");
            break;
        }
        
        if (opcode == WebSocketOpcode::Close) {
            LOG_DEBUG("Received close frame");
            break;
        }
        
        if (opcode == WebSocketOpcode::Text) {
            ws->send_text(std::string_view(buf, len));
        } else if (opcode == WebSocketOpcode::Binary) {
            ws->send_binary(buf, len);
        }
        // Ping/Pong handled automatically
    }
    
    LOG_INFO("ws_echo_handler: returning");
    return 0;
}

TEST(websocket, echo_text) {
    LOG_INFO("echo_text: starting test");
    
    // Setup HTTP server with WebSocket handler
    auto tcpserver = new_tcp_socket_server();
    LOG_INFO("echo_text: tcp server created");
    
    tcpserver->timeout(5000UL * 1000);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    DEFER(delete tcpserver);
    
    LOG_INFO("echo_text: tcp server listening");
    
    auto http_server = new_http_server();
    DEFER(delete http_server);
    
    LOG_INFO("echo_text: http server created");
    
    auto ws_handler = new_websocket_handler({nullptr, &ws_echo_handler});
    http_server->add_handler(ws_handler, true, "/ws");
    tcpserver->set_handler(http_server->get_connection_handler());
    tcpserver->start_loop();
    
    LOG_INFO("echo_text: server started, creating client");
    
    // Client connects
    auto client = new_http_client();
    DEFER(delete client);
    
    LOG_INFO("echo_text: http client created");
    
    auto port = tcpserver->getsockname().port;
    LOG_INFO("echo_text: connecting to port ", port);
    
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", port);
    
    LOG_INFO("echo_text: calling websocket_connect");
    
    auto ws = client->websocket_connect(url, 5000000UL);
    LOG_INFO("echo_text: upgrade returned, ws=", (void*)ws);
    ASSERT_NE(ws, nullptr);
    DEFER(delete ws);
    
    // Test text echo
    std::string test_msg = "Hello, WebSocket!";
    auto sent = ws->send_text(test_msg);
    ASSERT_EQ(sent, (ssize_t)test_msg.length());
    
    char recv_buf[4096];
    WebSocketOpcode opcode;
    auto received = ws->recv_frame(recv_buf, sizeof(recv_buf), &opcode);
    
    ASSERT_EQ(received, (ssize_t)test_msg.length());
    ASSERT_EQ(opcode, WebSocketOpcode::Text);
    ASSERT_EQ(std::string_view(recv_buf, received), test_msg);
    
    ws->close();
    
    LOG_INFO("WebSocket text echo test passed");
}

TEST(websocket, echo_binary) {
    // Setup HTTP server with WebSocket handler
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(5000UL * 1000);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    DEFER(delete tcpserver);
    
    auto http_server = new_http_server();
    DEFER(delete http_server);
    
    auto ws_handler = new_websocket_handler({nullptr, &ws_echo_handler});
    http_server->add_handler(ws_handler, true, "/ws");
    tcpserver->set_handler(http_server->get_connection_handler());
    tcpserver->start_loop();
    
    // Client connects
    auto client = new_http_client();
    DEFER(delete client);
    
    auto port = tcpserver->getsockname().port;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", port);
    
    auto ws = client->websocket_connect(url, 5000000UL);
    ASSERT_NE(ws, nullptr);
    DEFER(delete ws);
    
    // Test binary echo
    uint8_t binary_data[] = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0x80, 0x7F};
    auto sent = ws->send_binary(binary_data, sizeof(binary_data));
    ASSERT_EQ(sent, (ssize_t)sizeof(binary_data));
    
    char recv_buf[4096];
    WebSocketOpcode opcode;
    auto received = ws->recv_frame(recv_buf, sizeof(recv_buf), &opcode);
    
    ASSERT_EQ(received, (ssize_t)sizeof(binary_data));
    ASSERT_EQ(opcode, WebSocketOpcode::Binary);
    ASSERT_EQ(memcmp(recv_buf, binary_data, sizeof(binary_data)), 0);
    
    ws->close();
    
    LOG_INFO("WebSocket binary echo test passed");
}

TEST(websocket, multiple_messages) {
    // Setup HTTP server with WebSocket handler
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(5000UL * 1000);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    DEFER(delete tcpserver);
    
    auto http_server = new_http_server();
    DEFER(delete http_server);
    
    auto ws_handler = new_websocket_handler({nullptr, &ws_echo_handler});
    http_server->add_handler(ws_handler, true, "/ws");
    tcpserver->set_handler(http_server->get_connection_handler());
    tcpserver->start_loop();
    
    // Client connects
    auto client = new_http_client();
    DEFER(delete client);
    
    auto port = tcpserver->getsockname().port;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", port);
    
    auto ws = client->websocket_connect(url, 5000000UL);
    ASSERT_NE(ws, nullptr);
    DEFER(delete ws);
    
    // Send and receive multiple messages
    for (int i = 0; i < 10; i++) {
        std::string msg = "Message " + std::to_string(i);
        auto sent = ws->send_text(msg);
        ASSERT_EQ(sent, (ssize_t)msg.length());
        
        char recv_buf[4096];
        WebSocketOpcode opcode;
        auto received = ws->recv_frame(recv_buf, sizeof(recv_buf), &opcode);
        
        ASSERT_EQ(received, (ssize_t)msg.length());
        ASSERT_EQ(opcode, WebSocketOpcode::Text);
        ASSERT_EQ(std::string_view(recv_buf, received), msg);
    }
    
    ws->close();
    
    LOG_INFO("WebSocket multiple messages test passed");
}

TEST(websocket, large_message) {
    // Setup HTTP server with WebSocket handler
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(10000UL * 1000);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    DEFER(delete tcpserver);
    
    auto http_server = new_http_server();
    DEFER(delete http_server);
    
    auto ws_handler = new_websocket_handler({nullptr, &ws_echo_handler});
    http_server->add_handler(ws_handler, true, "/ws");
    tcpserver->set_handler(http_server->get_connection_handler());
    tcpserver->start_loop();
    
    // Client connects
    auto client = new_http_client();
    DEFER(delete client);
    
    auto port = tcpserver->getsockname().port;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", port);
    
    auto ws = client->websocket_connect(url, 10000000UL);
    ASSERT_NE(ws, nullptr);
    DEFER(delete ws);
    
    // Test with payload > 125 bytes (uses 16-bit length encoding)
    std::string medium_msg(300, 'M');
    auto sent = ws->send_text(medium_msg);
    ASSERT_EQ(sent, (ssize_t)medium_msg.length());
    
    char recv_buf[4096];
    WebSocketOpcode opcode;
    auto received = ws->recv_frame(recv_buf, sizeof(recv_buf), &opcode);
    
    ASSERT_EQ(received, (ssize_t)medium_msg.length());
    ASSERT_EQ(opcode, WebSocketOpcode::Text);
    ASSERT_EQ(std::string_view(recv_buf, received), medium_msg);
    
    ws->close();
    
    LOG_INFO("WebSocket large message test passed");
}

TEST(websocket, iovector_send) {
    // Setup HTTP server with WebSocket handler
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(5000UL * 1000);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    DEFER(delete tcpserver);
    
    auto http_server = new_http_server();
    DEFER(delete http_server);
    
    auto ws_handler = new_websocket_handler({nullptr, &ws_echo_handler});
    http_server->add_handler(ws_handler, true, "/ws");
    tcpserver->set_handler(http_server->get_connection_handler());
    tcpserver->start_loop();
    
    // Client connects
    auto client = new_http_client();
    DEFER(delete client);
    
    auto port = tcpserver->getsockname().port;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", port);
    
    auto ws = client->websocket_connect(url, 5000000UL);
    ASSERT_NE(ws, nullptr);
    DEFER(delete ws);
    
    // Test IOVector send
    char part1[] = "Hello, ";
    char part2[] = "World";
    char part3[] = "!";
    
    struct iovec iov[3];
    iov[0].iov_base = part1;
    iov[0].iov_len = strlen(part1);
    iov[1].iov_base = part2;
    iov[1].iov_len = strlen(part2);
    iov[2].iov_base = part3;
    iov[2].iov_len = strlen(part3);
    
    iovector_view view(iov, 3);
    auto sent = ws->send_binary(view);
    size_t total_len = strlen(part1) + strlen(part2) + strlen(part3);
    ASSERT_EQ(sent, (ssize_t)total_len);
    
    char recv_buf[4096];
    WebSocketOpcode opcode;
    auto received = ws->recv_frame(recv_buf, sizeof(recv_buf), &opcode);
    
    ASSERT_EQ(received, (ssize_t)total_len);
    ASSERT_EQ(opcode, WebSocketOpcode::Binary);
    ASSERT_EQ(std::string_view(recv_buf, received), "Hello, World!");
    
    ws->close();
    
    LOG_INFO("WebSocket IOVector send test passed");
}

TEST(websocket, close_codes) {
    // Setup HTTP server with WebSocket handler
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(5000UL * 1000);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    DEFER(delete tcpserver);
    
    auto http_server = new_http_server();
    DEFER(delete http_server);
    
    auto ws_handler = new_websocket_handler({nullptr, &ws_echo_handler});
    http_server->add_handler(ws_handler, true, "/ws");
    tcpserver->set_handler(http_server->get_connection_handler());
    tcpserver->start_loop();
    
    // Client connects
    auto client = new_http_client();
    DEFER(delete client);
    
    auto port = tcpserver->getsockname().port;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", port);
    
    auto ws = client->websocket_connect(url, 5000000UL);
    ASSERT_NE(ws, nullptr);
    DEFER(delete ws);
    
    ASSERT_FALSE(ws->is_closed());
    
    // Close with custom code and reason
    int ret = ws->close(WebSocketCloseCode::NormalClosure, "Test closure");
    ASSERT_EQ(ret, 0);
    ASSERT_TRUE(ws->is_closed());
    
    
    LOG_INFO("WebSocket close codes test passed");
}

TEST(websocket, empty_message) {
    // Setup HTTP server with WebSocket handler
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(5000UL * 1000);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    DEFER(delete tcpserver);
    
    auto http_server = new_http_server();
    DEFER(delete http_server);
    
    auto ws_handler = new_websocket_handler({nullptr, &ws_echo_handler});
    http_server->add_handler(ws_handler, true, "/ws");
    tcpserver->set_handler(http_server->get_connection_handler());
    tcpserver->start_loop();
    
    // Client connects
    auto client = new_http_client();
    DEFER(delete client);
    
    auto port = tcpserver->getsockname().port;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", port);
    
    auto ws = client->websocket_connect(url, 5000000UL);
    ASSERT_NE(ws, nullptr);
    DEFER(delete ws);
    
    // Test empty message
    auto sent = ws->send_text("");
    ASSERT_EQ(sent, 0);
    
    char recv_buf[4096];
    WebSocketOpcode opcode;
    auto received = ws->recv_frame(recv_buf, sizeof(recv_buf), &opcode);
    
    ASSERT_EQ(received, 0);
    ASSERT_EQ(opcode, WebSocketOpcode::Text);
    
    ws->close();
    
    LOG_INFO("WebSocket empty message test passed");
}

// Test invalid upgrade (non-WebSocket request to WebSocket handler)
int ws_invalid_handler(void*, Request& req, Response& resp, std::string_view) {
    // This should handle non-upgrade requests gracefully
    if (req.headers["Upgrade"] != "websocket") {
        resp.set_result(400, "Bad Request");
        resp.headers.content_length(0);
        return 0;
    }
    
    auto* ws = server_accept_websocket(req, resp);
    if (ws) {
        delete ws;
    }
    return 0;
}

TEST(websocket, invalid_upgrade_request) {
    // Setup HTTP server with WebSocket handler
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(5000UL * 1000);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    DEFER(delete tcpserver);
    
    auto http_server = new_http_server();
    DEFER(delete http_server);
    
    // Use delegate handler instead of websocket handler to test manual handling
    http_server->add_handler({nullptr, &ws_invalid_handler}, "/ws");
    tcpserver->set_handler(http_server->get_connection_handler());
    tcpserver->start_loop();
    
    // Client sends normal HTTP request (not WebSocket upgrade)
    auto client = new_http_client();
    DEFER(delete client);
    
    // Use 127.0.0.1 directly to avoid IPv6 resolution issues
    auto port = tcpserver->getsockname().port;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", port);
    auto op = client->new_operation(Verb::GET, url);
    DEFER(client->destroy_operation(op));
    
    op->call();
    EXPECT_EQ(400, op->resp.status_code());
    
    LOG_INFO("WebSocket invalid upgrade request test passed");
}

int main(int argc, char** argv) {
    if (photon::init()) {
        LOG_ERROR("Failed to initialize photon");
        return -1;
    }
    DEFER(photon::fini());

#ifdef __linux__
    if (net::et_poller_init() < 0) {
        LOG_ERROR("net::et_poller_init failed");
        exit(EAGAIN);
    }
    DEFER(net::et_poller_fini());
#endif

    set_log_output_level(ALOG_INFO);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
