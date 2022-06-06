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

#include <fcntl.h>
#include <time.h>

#include <gtest/gtest.h>
#include <photon/net/http/server.h>
#include <photon/net/http/client.h>
#include <photon/io/fd-events.h>
#include <photon/net/etsocket.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/localfs.h>

#include "../server.h"

using namespace photon;
using namespace photon::net;

RetType idiot_handle(void*, HTTPServerRequest &req, HTTPServerResponse &resp) {
    std::string str;
    auto r = req.Range();
    auto cl = r.second - r.first + 1;
    if (cl > 4096) {
        LOG_ERROR_RETURN(0, RetType::failed, "RetType failed test");
    }
    resp.ContentLength(cl);
    resp.SetResult(200);
    str.resize(cl);
    memset((void*)str.data(), '0', cl);
    resp.Insert("Test_Handle", "test");
    resp.Write((void*)str.data(), str.size());
    return RetType::success;
}

TEST(http_server, headers) {
    auto server = new_http_server(19876);
    DEFER(delete server);
    server->SetHandler({nullptr, &idiot_handle});
    EXPECT_EQ(true, server->Launch());
    auto client = new_http_client();
    DEFER(delete client);
    auto op = client->new_operation(Verb::GET, "localhost:19876/test");
    DEFER(delete op);
    auto exp_len = 20;
    op->req.insert_range(0, exp_len - 1);
    op->call();
    EXPECT_EQ(200, op->resp.status_code());
    char buf[4096];
    auto ret = op->resp_body->read(buf, 4096);
    EXPECT_EQ(exp_len, ret);
    EXPECT_EQ(true, "test" == op->resp["Test_Handle"]);
    server->Stop();
}
std::string fs_handler_std_str = "01234567890123456789";

void test_case(Client* client, off_t st, size_t len, size_t exp_content_length, bool invalid = false) {
    auto op = client->new_operation(Verb::GET, "localhost:19876/fs_handler_test");
    DEFER(delete op);
    op->req.insert_range(st, st + len - 1);
    auto ret = op->call();
    EXPECT_EQ(0, ret);
    if (!invalid) {
        if (exp_content_length != fs_handler_std_str.size())
            EXPECT_EQ(206, op->resp.status_code());
        else
            EXPECT_EQ(200, op->resp.status_code());
        char buf[4096];
        ret = op->resp_body->read(buf, 4096);
        EXPECT_EQ(exp_content_length, ret);
        if (st >= fs_handler_std_str.size()) st = fs_handler_std_str.size() - 1;
        if (st + len > fs_handler_std_str.size()) len = fs_handler_std_str.size() - st;
        EXPECT_EQ(true, std::string(fs_handler_std_str.data() + st, exp_content_length) ==
                        std::string(buf, exp_content_length));
    }
}
TEST(http_server, fs_handler) {
    system(std::string("mkdir -p /tmp/ease_ut/http_server/").c_str());
    system(std::string("touch /tmp/ease_ut/http_server/fs_handler_test").c_str());
    system(std::string("echo -n '" + fs_handler_std_str + "' > /tmp/ease_ut/http_server/fs_handler_test").c_str());
    auto server = new_http_server(19876);
    DEFER(delete server);
    auto fs = fs::new_localfs_adaptor("/tmp/ease_ut/http_server/");
    DEFER(delete fs);
    auto fs_handler = new_fs_handler(fs);
    DEFER(delete fs_handler);
    server->SetHandler(fs_handler->GetHandler());
    EXPECT_EQ(true, server->Launch());
    auto client = new_http_client();
    DEFER(delete client);
    test_case(client, 5, 10, 10);
    test_case(client, 5, 20, 0, true);
    test_case(client, 25, 5, 0, true);
    test_case(client, 0, 20, 20);
    server->Stop();
}

net::EndPoint ep{net::IPAddr("127.0.0.1"), 19731};
std::string std_data;
const size_t std_data_size = 64 * 1024;
constexpr char header_data[] = "HTTP/1.1 200 ok\r\n"
                               "Transfer-Encoding: chunked\r\n"
                               "Connection: close\r\n"
                               "\r\n";
void chunked_send(int offset, int size, net::ISocketStream* sock) {
    char s[10];
    auto len = snprintf(s, sizeof(s), "%x\r\n", size);
    sock->write(s, len);
    auto ret = sock->write(std_data.data() + offset, size);
    EXPECT_EQ(ret, size);
    sock->write("\r\n", 2);
}
std::vector<int> rec;
int chunked_handler_pt(void*, net::ISocketStream* sock) {
    EXPECT_NE(nullptr, sock);
    LOG_DEBUG("Accepted");
    char recv[4096];
    auto len = sock->recv(recv, 4096);
    EXPECT_GT(len, 0);
    LOG_INFO("source server recv request, len:", len);
    LOG_INFO("req:", std::string_view(recv, len));
    auto ret = sock->write(header_data, sizeof(header_data) - 1);
    EXPECT_EQ(sizeof(header_data) - 1, ret);
    auto offset = 0;
    rec.clear();
    while (offset < std_data_size) {
        auto remain = std_data_size - offset;
        if (remain <= 1024) {
            rec.push_back(remain);
            chunked_send(offset, remain, sock);
            break;
        }
        auto max_seg = std::min(remain - 1024, 2 * 4 * 1024UL);
        auto seg = 1024 + rand() % max_seg;
        chunked_send(offset, seg, sock);
        rec.push_back(seg);
        offset += seg;
    }
    sock->write("0\r\n\r\n", 5);
    return 0;
}

RetType test_director(void*, HTTPServerRequest& req) {
    req.Insert("proxy_server_test", "just4test");
    req.SetTarget("/filename_not_important");
    req.Erase("Host");
    req.Insert("Host", "127.0.0.1:19731");
    return RetType::success;
}

RetType test_modifier(void*, HTTPServerResponse& resp) {
    resp.Insert("proxy_server_test", "just4test");
    return RetType::success;
}
TEST(http_server, proxy_handler) {
    std_data.resize(std_data_size);
    int num = 0;
    for (auto &c : std_data) {
        c = '0' + ((++num) % 10);
    }
    srand(time(0));
    //------------start source server---------------
    auto source_server = net::new_tcp_socket_server();
    DEFER({ delete source_server; });
    source_server->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    source_server->set_handler({nullptr, &chunked_handler_pt});
    auto ret = source_server->bind(ep.port, ep.addr);
    if (ret < 0) LOG_ERROR(VALUE(errno));
    ret |= source_server->listen(100);
    if (ret < 0) LOG_ERROR(VALUE(errno));
    EXPECT_EQ(0, ret);
    LOG_INFO("Ready to accept");
    source_server->start_loop();
    photon::thread_sleep(1);
    //------------------------------------------
    auto client = new_http_client();
    DEFER(delete client);
    //--------start proxy server ------------
    auto proxy_server = new_http_server(19876);
    DEFER(delete proxy_server);
    auto proxy_handler = new_reverse_proxy_handler({nullptr, &test_director},
                                                   {nullptr, &test_modifier},
                                                   client);
    proxy_server->SetHandler(proxy_handler->GetHandler());
    EXPECT_EQ(true, proxy_server->Launch());
    DEFER(proxy_server->Stop());
    //----------------------------------------------------
    auto op = client->new_operation(Verb::GET, "localhost:19876/filename");
    DEFER(delete op);
    ret = op->call();
    EXPECT_EQ(0, ret);
    std::string data_buf;
    data_buf.resize(std_data_size + 1000);
    ret = op->resp_body->read((void*)data_buf.data(), data_buf.size());
    EXPECT_EQ(std_data_size, ret);
    data_buf.resize(ret);
    EXPECT_EQ(true, data_buf == std_data);
}

TEST(http_server, mux_handler) {
    system(std::string("mkdir -p /tmp/ease_ut/http_server/").c_str());
    system(std::string("touch /tmp/ease_ut/http_server/fs_handler_test").c_str());
    system(std::string("echo -n '" + fs_handler_std_str + "' > /tmp/ease_ut/http_server/fs_handler_test").c_str());
    std_data.resize(std_data_size);
    int num = 0;
    for (auto &c : std_data) {
        c = '0' + ((++num) % 10);
    }
    srand(time(0));
    //------------start source server---------------
    auto source_server = net::new_tcp_socket_server();
    DEFER({ delete source_server; });
    source_server->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    source_server->set_handler({nullptr, &chunked_handler_pt});
    auto ret = source_server->bind(ep.port, ep.addr);
    if (ret < 0) LOG_ERROR(VALUE(errno));
    ret |= source_server->listen(100);
    if (ret < 0) LOG_ERROR(VALUE(errno));
    EXPECT_EQ(0, ret);
    LOG_INFO("Ready to accept");
    source_server->start_loop();
    photon::thread_sleep(1);
    //------------------------------------------
    auto client = new_http_client();
    DEFER(delete client);
    //--------start mux server ------------
    auto mux_server = new_http_server(19876);
    DEFER(delete mux_server);
    auto proxy_handler = new_reverse_proxy_handler({nullptr, &test_director},
                                                   {nullptr, &test_modifier},
                                                   client);
    auto fs = fs::new_localfs_adaptor("/tmp/ease_ut/http_server/");
    DEFER(delete fs);
    auto fs_handler = new_fs_handler(fs, "static_service/");
    DEFER(delete fs_handler);
    auto mux_handler = new_mux_handler();
    mux_handler->AddHandler("/static_service/", fs_handler);
    mux_handler->AddHandler("/proxy/", proxy_handler);
    mux_server->SetHandler(mux_handler->GetHandler());
    EXPECT_EQ(true, mux_server->Launch());
    DEFER(mux_server->Stop());
    //----------------------------------------------------
    //--------------test static service--------------------
    auto op_static = client->new_operation(Verb::GET, "localhost:19876/static_service/fs_handler_test");
    DEFER(delete op_static);
    ret = op_static->call();
    EXPECT_EQ(0, ret);
    EXPECT_EQ(200, op_static->resp.status_code());
    std::string data_buf;
    data_buf.resize(fs_handler_std_str.size());
    ret = op_static->resp_body->read((void*)data_buf.data(), data_buf.size());
    EXPECT_EQ(data_buf.size(), ret);
    EXPECT_EQ(true, data_buf == fs_handler_std_str);
    //--------------test proxy service---------------------
    auto op_proxy = client->new_operation(Verb::GET, "localhost:19876/proxy/filename_not_important");
    DEFER(delete op_proxy);
    ret = op_proxy->call();
    EXPECT_EQ(0, ret);
    EXPECT_EQ(200, op_proxy->resp.status_code());
    data_buf.resize(std_data_size + 1000);
    ret = op_proxy->resp_body->read((void*)data_buf.data(), data_buf.size());
    EXPECT_EQ(std_data_size, ret);
    data_buf.resize(ret);
    EXPECT_EQ(true, data_buf == std_data);
    //-------------test mux default handler---------------
    auto op_default = client->new_operation(Verb::GET, "localhost:19876/not_recorded/should_be_404");
    DEFER(delete op_default);
    ret = op_default->call();
    EXPECT_EQ(0, ret);
    EXPECT_EQ(404, op_default->resp.status_code());
}

int main(int argc, char** arg) {
    photon::thread_init();
    DEFER(photon::thread_fini());
    photon::fd_events_init();
    DEFER(photon::fd_events_fini());
    if (net::et_poller_init() < 0) {
        LOG_ERROR("net::et_poller_init failed");
        exit(EAGAIN);
    }
    DEFER(net::et_poller_fini());
    set_log_output_level(ALOG_DEBUG);
    ::testing::InitGoogleTest(&argc, arg);
    LOG_DEBUG("test result:`", RUN_ALL_TESTS());
}
