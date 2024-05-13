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
    tcpserver->timeout(1000UL*1000);
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
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
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

int main(int argc, char** arg) {
    LOG_DEBUG("Begin test");
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    set_log_output_level(ALOG_DEBUG);
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
