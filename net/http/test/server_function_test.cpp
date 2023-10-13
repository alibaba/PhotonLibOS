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
#include <photon/net/socket.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/localfs.h>

#include "../server.h"

using namespace photon;
using namespace photon::net;
using namespace photon::net::http;

int idiot_handle(void*, Request &req, Response &resp, std::string_view) {
    std::string str;
    auto r = req.headers.range();
    auto cl = r.second - r.first + 1;
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

TEST(http_server, headers) {
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto server = new_http_server();
    DEFER(delete server);
    server->add_handler({nullptr, &idiot_handle});
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();
    auto client = new_http_client();
    DEFER(delete client);
    auto op = client->new_operation(Verb::GET, "localhost:19876/test");
    DEFER(delete op);
    auto exp_len = 20;
    op->req.headers.range(0, exp_len - 1);
    op->call();
    EXPECT_EQ(200, op->resp.status_code());
    char buf[4096];
    auto ret = op->resp.read(buf, 4096);
    EXPECT_EQ(exp_len, ret);
    EXPECT_EQ(true, "test" == op->resp.headers["Test_Handle"]);
}


int body_check_handler(void*, Request &req, Response &resp, std::string_view) {
    char buf[4096];
    auto ret = req.read(buf, 4096);
    EXPECT_EQ(ret, 10);
    EXPECT_EQ(0, strncmp(buf, "1234567890", 10));

    resp.set_result(200);
    std::string str = "success";
    resp.headers.content_length(7);
    resp.headers.insert("Test_Handle", "test");
    resp.write((void*)str.data(), str.size());
    return 0;
}


TEST(http_server, post) {
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto server = new_http_server();
    DEFER(delete server);
    server->add_handler({nullptr, &body_check_handler});
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();
    auto client = new_http_client();
    DEFER(delete client);
    auto op = client->new_operation(Verb::POST, "localhost:19876/test");
    DEFER(delete op);
    op->req.headers.content_length(10);
    std::string body = "1234567890";
    auto writer = [&](Request *req)-> ssize_t {
        return req->write(body.data(), body.size());
    };
    op->body_writer = writer;
    op->call();
    EXPECT_EQ(200, op->resp.status_code());
    EXPECT_EQ(true, "test" == op->resp.headers["Test_Handle"]);
    char buf[4096];
    auto ret = op->resp.read(buf, 4096);
    EXPECT_EQ(ret, 7);
    EXPECT_EQ(0, strncmp(buf, "success", ret));
}

std::string fs_handler_std_str = "01234567890123456789";

void test_case(Client* client, off_t st, size_t len, size_t exp_content_length, bool invalid = false) {
    LOG_INFO("test case start");
    auto op = client->new_operation(Verb::GET, "localhost:19876/fs_handler_test");
    DEFER(delete op);
    op->req.headers.range(st, st + len - 1);
    auto ret = op->call();
    LOG_INFO("call finished");
    EXPECT_EQ(0, ret);
    if (!invalid) {
        if (exp_content_length != fs_handler_std_str.size())
            EXPECT_EQ(206, op->resp.status_code());
        else
            EXPECT_EQ(200, op->resp.status_code());
        char buf[4096];
        ret = op->resp.read(buf, 4096);
        EXPECT_EQ(exp_content_length, ret);
        if (st >= fs_handler_std_str.size()) st = fs_handler_std_str.size() - 1;
        if (st + len > fs_handler_std_str.size()) len = fs_handler_std_str.size() - st;
        EXPECT_EQ(true, std::string(fs_handler_std_str.data() + st, exp_content_length) ==
                        std::string(buf, exp_content_length));
    }
}
void test_head_case(Client* client, off_t st, size_t len, size_t exp_content_length) {
    LOG_INFO("test HEAD case start");
    auto op = client->new_operation(Verb::HEAD, "localhost:19876/fs_handler_test");
    DEFER(delete op);
    op->req.headers.range(st, st + len - 1);
    op->req.headers.content_length(fs_handler_std_str.size());
    auto ret = op->call();
    LOG_INFO("call finished");
    EXPECT_EQ(0, ret);
    if (exp_content_length != fs_handler_std_str.size())
        EXPECT_EQ(206, op->resp.status_code());
    else
        EXPECT_EQ(200, op->resp.status_code());
    char range[64];
    auto range_len = snprintf(range, 64, "bytes %lu-%lu/%lu", st, st + len - 1, fs_handler_std_str.size());
    std::string rangestr(op->resp.headers["Content-Range"]);
    EXPECT_EQ(0, memcmp(range, rangestr.data(), range_len));
}
TEST(http_server, fs_handler) {
    system(std::string("mkdir -p /tmp/ease_ut/http_server/").c_str());
    system(std::string("touch /tmp/ease_ut/http_server/fs_handler_test").c_str());
    system(std::string("printf '" + fs_handler_std_str + "' > /tmp/ease_ut/http_server/fs_handler_test").c_str());
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto server = new_http_server();
    DEFER(delete server);
    auto fs = fs::new_localfs_adaptor("/tmp/ease_ut/http_server/");
    DEFER(delete fs);
    auto fs_handler = new_fs_handler(fs);
    DEFER(delete fs_handler);
    server->add_handler(fs_handler);
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();
    auto client = new_http_client();
    DEFER(delete client);
    test_case(client, 5, 10, 10);
    test_case(client, 5, 20, 0, true);
    test_case(client, 25, 5, 0, true);
    test_case(client, 0, 20, 20);
    test_head_case(client, 5, 10, 10);
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

int test_director(void*, Request& src, Request& dst) {
    estring url;
    url.appends(http_url_scheme, "127.0.0.1:19731", "/filename_not_important");
    dst.reset(src.verb(), url);
    dst.headers.insert("proxy_server_test", "just4test");
    for (auto kv = src.headers.begin(); kv != src.headers.end(); kv++) {
        if (kv.first() != "Host") dst.headers.insert(kv.first(), kv.second(), 1);
    }
    return 0;
}

int test_modifier(void*, Response& src, Response& dst) {
    dst.set_result(src.status_code());
    for (auto kv : src.headers) {
        dst.headers.insert(kv.first, kv.second);
        LOG_DEBUG(kv.first, ": ", kv.second);
    }
    dst.headers.insert("proxy_server_test", "just4test");

    return 0;
}
TEST(http_server, proxy_handler_get) {
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
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto proxy_server = new_http_server();
    DEFER(delete proxy_server);
    auto proxy_handler = new_proxy_handler({nullptr, &test_director}, {nullptr, &test_modifier}, client);
    proxy_server->add_handler(proxy_handler);
    tcpserver->set_handler(proxy_server->get_connection_handler());
    tcpserver->start_loop();
    //----------------------------------------------------
    auto op = client->new_operation(Verb::GET, "localhost:19876/filename");
    DEFER(delete op);
    ret = op->call();
    EXPECT_EQ(0, ret);
    std::string data_buf;
    data_buf.resize(std_data_size + 1000);
    ret = op->resp.read((void*)data_buf.data(), data_buf.size());
    EXPECT_EQ(std_data_size, ret);
    data_buf.resize(ret);
    EXPECT_EQ(true, data_buf == std_data);
}


TEST(http_server, proxy_handler_post) {
    auto source_server = new_tcp_socket_server();
    source_server->timeout(1000UL*1000);
    source_server->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    source_server->bind(19731, IPAddr("127.0.0.1"));
    source_server->listen();
    DEFER(delete source_server);
    auto source_http_server = new_http_server();
    DEFER(delete source_http_server);
    source_http_server->add_handler({nullptr, &body_check_handler});
    source_server->set_handler(source_http_server->get_connection_handler());
    source_server->start_loop();

    photon::thread_sleep(1);
    //------------------------------------------
    auto client = new_http_client();
    DEFER(delete client);
    //--------start proxy server ------------
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto proxy_server = new_http_server();
    DEFER(delete proxy_server);
    auto proxy_handler = new_proxy_handler({nullptr, &test_director}, {nullptr, &test_modifier}, client);
    proxy_server->add_handler(proxy_handler);
    tcpserver->set_handler(proxy_server->get_connection_handler());
    tcpserver->start_loop();
    //----------------------------------------------------
    auto op = client->new_operation(Verb::POST, "localhost:19876/filename");
    DEFER(delete op);
    std::string body = "1234567890";
    op->req.headers.content_length(10);
    auto writer = [&](Request *req)-> ssize_t {
        return req->write(body.data(), body.size());
    };
    op->body_writer = writer;
    int ret = op->call();
    EXPECT_EQ(0, ret);
    char buf[4096];
    ret = op->resp.read(buf, 4096);
    EXPECT_EQ(ret, 7);
    EXPECT_EQ(0, strncmp(buf, "success", ret));
}

int test_forward_director(void*, Request& src, Request& dst) {
    LOG_INFO("request url = `", src.target());
    estring url;
    url.appends(http_url_scheme, "127.0.0.1:19731", "/filename_not_important");
    dst.reset(src.verb(), url);
    dst.headers.insert("proxy_server_test", "just4test");
    for (auto kv = src.headers.begin(); kv != src.headers.end(); kv++) {
        if (kv.first() != "Host") dst.headers.insert(kv.first(), kv.second(), 1);
    }
    return 0;
}

TEST(http_server, proxy_handler_post_forward) {
    auto source_server = new_tcp_socket_server();
    source_server->timeout(1000UL*1000);
    source_server->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    source_server->bind(19731, IPAddr("127.0.0.1"));
    source_server->listen();
    DEFER(delete source_server);
    auto source_http_server = new_http_server();
    DEFER(delete source_http_server);
    source_http_server->add_handler({nullptr, &body_check_handler});
    source_server->set_handler(source_http_server->get_connection_handler());
    source_server->start_loop();

    photon::thread_sleep(1);
    //------------------------------------------
    auto client = new_http_client();
    DEFER(delete client);
    //--------start proxy server ------------
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto proxy_server = new_http_server();
    DEFER(delete proxy_server);
    auto proxy_handler = new_proxy_handler({nullptr, &test_forward_director}, {nullptr, &test_modifier}, client);
    proxy_server->add_handler(proxy_handler);
    tcpserver->set_handler(proxy_server->get_connection_handler());
    tcpserver->start_loop();
    //----------------------------------------------------
    auto client1 = new_http_client();
    DEFER(delete client1);
    client1->set_proxy("http://localhost:19876/");
    auto op = client1->new_operation(Verb::POST, "http://localhost:19731/filename");
    DEFER(delete op);
    std::string body = "1234567890";
    op->req.headers.content_length(10);
    auto writer = [&](Request *req)-> ssize_t {
        return req->write(body.data(), body.size());
    };
    op->body_writer = writer;
    int ret = op->call();
    EXPECT_EQ(0, ret);
    char buf[4096];
    ret = op->resp.read(buf, 4096);
    EXPECT_EQ(ret, 7);
    EXPECT_EQ(0, strncmp(buf, "success", ret));
}


TEST(http_server, proxy_handler_failure) {
    //------------------------------------------
    auto client = new_http_client();
    DEFER(delete client);
    //--------start proxy server ------------
    auto client_proxy = new_http_client();
    DEFER(delete client_proxy);
    client_proxy->timeout_ms(500);
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto proxy_server = new_http_server();
    DEFER(delete proxy_server);
    auto proxy_handler = new_proxy_handler({nullptr, &test_director}, {nullptr, &test_modifier}, client_proxy);
    proxy_server->add_handler(proxy_handler);
    tcpserver->set_handler(proxy_server->get_connection_handler());
    tcpserver->start_loop();
    //----------------------------------------------------
    auto op = client->new_operation(Verb::GET, "localhost:19876/filename");
    DEFER(delete op);
    auto ret = op->call();
    EXPECT_EQ(0, ret);
    EXPECT_EQ(502, op->resp.status_code());
}

TEST(http_server, mux_handler) {
    system(std::string("mkdir -p /tmp/ease_ut/http_server/").c_str());
    system(std::string("touch /tmp/ease_ut/http_server/fs_handler_test").c_str());
    system(std::string("printf '" + fs_handler_std_str + "' > /tmp/ease_ut/http_server/fs_handler_test").c_str());
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
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserver->bind(19876, IPAddr("127.0.0.1"));
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto proxy_handler = new_proxy_handler({nullptr, &test_director}, {nullptr, &test_modifier}, client);
    auto fs = fs::new_localfs_adaptor("/tmp/ease_ut/http_server/");
    DEFER(delete fs);
    auto fs_handler = new_fs_handler(fs);
    auto server = new_http_server();
    DEFER(delete server);
    server->add_handler(fs_handler, true, "/static_service/");
    server->add_handler(proxy_handler, true, "/proxy/");
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();
    //----------------------------------------------------
    //--------------test static service--------------------
    auto op_static = client->new_operation(Verb::GET, "localhost:19876/static_service/fs_handler_test");
    DEFER(delete op_static);
    ret = op_static->call();
    EXPECT_EQ(0, ret);
    EXPECT_EQ(200, op_static->resp.status_code());
    std::string data_buf;
    data_buf.resize(fs_handler_std_str.size());
    ret = op_static->resp.read((void*)data_buf.data(), data_buf.size());
    EXPECT_EQ(data_buf.size(), ret);
    EXPECT_EQ(true, data_buf == fs_handler_std_str);
    //--------------test proxy service---------------------
    auto op_proxy = client->new_operation(Verb::GET, "localhost:19876/proxy/filename_not_important");
    DEFER(delete op_proxy);
    ret = op_proxy->call();
    EXPECT_EQ(0, ret);
    EXPECT_EQ(200, op_proxy->resp.status_code());
    data_buf.resize(std_data_size + 1000);
    ret = op_proxy->resp.read((void*)data_buf.data(), data_buf.size());
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
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
#ifdef __linux__
    if (net::et_poller_init() < 0) {
        LOG_ERROR("net::et_poller_init failed");
        exit(EAGAIN);
    }
    DEFER(net::et_poller_fini());
#endif
    set_log_output_level(ALOG_DEBUG);
    ::testing::InitGoogleTest(&argc, arg);
    LOG_DEBUG("test result:`", RUN_ALL_TESTS());
}
