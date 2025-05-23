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
#include <netinet/tcp.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <string>

#include <photon/net/socket.h>
#include <photon/common/alog.h>
#include "../client.cpp"
#include "../server.h"
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/common/stream.h>
#include <photon/fs/localfs.h>
#include "../../../test/gtest.h"
#include "to_url.h"

using namespace photon::net;
using namespace photon::net::http;

static char socket_buf[] =
    "this is a http_client request body text for socket stream";

int socket_put_cb(void* self, IStream* stream) {
    auto ret = stream->write(socket_buf, sizeof(socket_buf));
    EXPECT_EQ(sizeof(socket_buf), ret);
    return (sizeof(socket_buf) == ret) ? 0 : -1;
}

int socket_get_cb(void* self, IStream* stream) { return 0; }

int timeout_writer(void *self, IStream* stream) {
    photon::thread_usleep(5 * 1000UL * 1000UL);
    char c = '1';
    stream->write((void*)&c, 1);
    return 0;
}

class SimpleHandler : public http::HTTPHandler {
public:
    int handle_request(http::Request& req, http::Response& resp, std::string_view) {
        std::string url_path(req.target());
        resp.set_result(200);
        resp.headers.content_length(url_path.size());
        resp.headers.insert("Content-Type", "application/octet-stream");
        auto n = resp.write(url_path.data(), url_path.size());
        if (n != (ssize_t) url_path.size()) {
            LOG_ERRNO_RETURN(0, -1, "send body failed");
        }
        return 0;
    }
};

TEST(http_client, get) {
    system("mkdir -p /tmp/ease_ut/http_test/");
    system("echo \"this is a http_client request body text for socket stream\" > /tmp/ease_ut/http_test/ease-httpclient-gettestfile");
    auto tcpserver = new_tcp_socket_server();
    tcpserver->setsockopt<int>(IPPROTO_TCP, TCP_NODELAY, 1);
    tcpserver->bind_v4any();
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto server = new_http_server();
    DEFER(delete server);
    auto fs = photon::fs::new_localfs_adaptor("/tmp/ease_ut/http_test/");
    DEFER(delete fs);
    auto fs_handler = new_fs_handler(fs);
    DEFER(delete fs_handler);
    server->add_handler(fs_handler);
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();
    auto target = to_url(tcpserver, "/ease-httpclient-gettestfile");
    auto client = new_http_client();
    DEFER(delete client);
    auto op2 = client->new_operation(Verb::GET, target);
    DEFER(client->destroy_operation(op2));
    op2->req.headers.content_length(0);
    int ret = client->call(op2);
    GTEST_ASSERT_EQ(0, ret);

    char resp_body_buf[1024];
    EXPECT_EQ(sizeof(socket_buf), op2->resp.resource_size());
    ret = op2->resp.read(resp_body_buf, sizeof(socket_buf));
    EXPECT_EQ(sizeof(socket_buf), ret);
    resp_body_buf[sizeof(socket_buf) - 1] = '\0';
    LOG_DEBUG(resp_body_buf);
    EXPECT_EQ(0, strcmp(resp_body_buf, socket_buf));

    auto op3 = client->new_operation(Verb::GET, target);
    DEFER(client->destroy_operation(op3));
    op3->req.headers.content_length(0);
    op3->req.headers.range(10, 19);
    client->call(op3);
    char resp_body_buf_range[1024];
    ret = op3->resp.read(resp_body_buf_range, op3->resp.headers.content_length());
    resp_body_buf_range[10] = '\0';
    EXPECT_EQ(0, strcmp("http_clien", resp_body_buf_range));
    LOG_DEBUG(resp_body_buf_range);

    auto op4 = client->new_operation(Verb::GET, target);
    DEFER(client->destroy_operation(op4));
    op4->req.headers.content_length(0);
    op4->call();
    EXPECT_EQ(sizeof(socket_buf), op4->resp.resource_size());
    ret =  op4->resp.read(resp_body_buf, 10);
    EXPECT_EQ(10, ret);
    resp_body_buf[10] = '\0';
    EXPECT_EQ(0, strcmp("this is a ", resp_body_buf));
    LOG_DEBUG(resp_body_buf);
    ret =  op4->resp.read(resp_body_buf, 10);
    EXPECT_EQ(10, ret);
    resp_body_buf[10] = '\0';
    LOG_DEBUG(resp_body_buf);
    EXPECT_EQ(0, strcmp("http_clien", resp_body_buf));

    static const char target_tb[] = "http://www.taobao.com?x";
    auto op5 = client->new_operation(Verb::GET, target_tb);
    DEFER(client->destroy_operation(op5));
    op5->req.headers.content_length(0);
    op5->call();
    EXPECT_EQ(op5->resp.status_code(), 200);
}

int body_check_handler(void*, Request &req, Response &resp, std::string_view) {
    auto fs = photon::fs::new_localfs_adaptor("/tmp/ease_ut/http_test/");
    DEFER(delete fs);

    char buf[4096];
    auto len_body = req.read(buf, 4096);
    char buf_file[4096];
    auto file = fs->open("ease-httpclient-posttestfile", O_RDONLY);
    DEFER(delete file);
    auto len_file = file->read(buf_file, 4096);
    EXPECT_EQ(len_body, len_file);
    EXPECT_EQ(0, strncmp(buf, buf_file, len_body));

    resp.set_result(200);
    std::string str = "success";
    resp.headers.content_length(7);

    resp.write((void*)str.data(), str.size());
    return 0;
}

TEST(http_client, post) {
    system("mkdir -p /tmp/ease_ut/http_test/");
    system("echo \"this is a http_client request body text for socket stream\" > /tmp/ease_ut/http_test/ease-httpclient-posttestfile");
    auto tcpserver = new_tcp_socket_server();
    tcpserver->timeout(1000UL*1000);
    tcpserver->bind_v4localhost(0);
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto server = new_http_server();
    DEFER(delete server);
    server->add_handler({nullptr, &body_check_handler});
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();


    auto fs = photon::fs::new_localfs_adaptor("/tmp/ease_ut/http_test/");
    DEFER(delete fs);
    auto target = to_url(tcpserver, "/ease-httpclient-posttestfile");
    auto client = new_http_client();
    DEFER(delete client);

    auto file = fs->open("ease-httpclient-posttestfile", O_RDONLY);
    DEFER(delete file);

    // body stream test
    auto op1 = client->new_operation(Verb::POST, target);
    DEFER(client->destroy_operation(op1));
    struct stat st;
    EXPECT_EQ(0, file->fstat(&st));
    op1->req.headers.content_length(st.st_size);
    op1->body_stream = file;
    client->call(op1);
    EXPECT_EQ(200, op1->resp.status_code());
    char buf[4096];
    auto ret = op1->resp.read(buf, 4096);
    EXPECT_EQ(ret, 7);
    EXPECT_EQ(0, strncmp(buf, "success", ret));

    // body writer test
    auto op2 = client->new_operation(Verb::POST, target);
    DEFER(client->destroy_operation(op2));
    op2->req.headers.content_length(st.st_size);
    auto writer = [&](Request *req)-> ssize_t {
        file->lseek(0, SEEK_SET);
        return req->write_stream(file, st.st_size);
    };
    op2->body_writer = writer;
    client->call(op2);
    EXPECT_EQ(200, op2->resp.status_code());
    ret = op2->resp.read(buf, 4096);
    EXPECT_EQ(ret, 7);
    EXPECT_EQ(0, strncmp(buf, "success", ret));

    // body buffer test
    auto op3 = client->new_operation(Verb::POST, target);
    DEFER(client->destroy_operation(op3));
    void *body_buf = malloc(st.st_size);
    EXPECT_EQ(st.st_size, file->pread(body_buf, st.st_size, 0));
    op3->set_body(body_buf, st.st_size);
    client->call(op3);
    EXPECT_EQ(200, op3->resp.status_code());
    ret = op3->resp.read(buf, 4096);
    EXPECT_EQ(ret, 7);
    EXPECT_EQ(0, strncmp(buf, "success", ret));
}



#define RETURN_IF_FAILED(func)                             \
    if (0 != (func)) {                                     \
        status = Status::failure;                          \
        LOG_ERROR_RETURN(0, , "Failed to perform " #func); \
    }
constexpr char http_response_data[] = "HTTP/1.1 200 4TEST\r\n"
                                      "Transfer-Encoding: chunked\r\n"
                                    //   "Content-Length: 5\r\n"
                                      "\r\n"
                                      "C\r\n"
                                      "first chunk \r\n"
                                      "D\r\n"
                                      "second chunk \r\n"
                                      "0\r\n"
                                      "\r\n";
int chunked_handler(void*, ISocketStream* sock) {
    EXPECT_NE(nullptr, sock);
    LOG_DEBUG("Accepted");
    char recv[4096];
    sock->recv(recv, 4096);
    LOG_DEBUG("RECV `", recv);
    sock->write(http_response_data, sizeof(http_response_data) - 1);
    LOG_DEBUG("SEND `", http_response_data);
    return 0;
}
constexpr char header_data[] = "HTTP/1.1 200 ok\r\n"
                               "Transfer-Encoding: chunked\r\n"
                               "\r\n";
int chunked_handler_complict(void*, ISocketStream* sock) {
    EXPECT_NE(nullptr, sock);
    LOG_DEBUG("Accepted");
    char recv[4096];
    sock->recv(recv, 4096);
    LOG_DEBUG("RECV `", recv);
    auto ret = sock->write(header_data, sizeof(header_data) - 1);
    EXPECT_EQ(sizeof(header_data) - 1, ret);
    //-----------------------
    ret = sock->write("2710\r\n", 6);
    char space_buf[10000];
    memset(space_buf, 'a', sizeof(space_buf));
    ret = sock->write(space_buf, 10000);
    EXPECT_EQ(ret, 10000);
    sock->write("\r\n", 2);
    sock->write("FFA\r\n", 5);
    ret = sock->write(space_buf, 4090);
    EXPECT_EQ(ret, 4090);
    sock->write("\r\n", 2);
    sock->write("FF6\r\n", 5);
    ret = sock->write(space_buf, 4086);
    EXPECT_EQ(ret, 4086);
    sock->write("\r\n", 2);
    sock->write("400\r\n", 5);
    ret = sock->write(space_buf, 1024);
    EXPECT_EQ(ret, 1024);
    sock->write("\r\n", 2);
    sock->write("0\r\n\r\n", 5);
    return 0;
}

std::string std_data;
const size_t std_data_size = 64 * 1024;
/*
static int digtal_num(int n) {
    int ret = 0;
    do {
        ++ret;
        n /= 10;
    } while (n);
    return ret;
}
*/
void chunked_send(int offset, int size, ISocketStream* sock) {
    char s[10];
    auto len = snprintf(s, sizeof(s), "%x\r\n", size);
    sock->write(s, len);
    auto ret = sock->write(std_data.data() + offset, size);
    EXPECT_EQ(ret, size);
    sock->write("\r\n", 2);
}
std::vector<int> rec;
int chunked_handler_pt(void*, ISocketStream* sock) {
    EXPECT_NE(nullptr, sock);
    char recv[4096];
    auto len = sock->recv(recv, 4096);
    if (len < 0)
        LOG_ERRNO_RETURN(0, -1, "failed to read from socket ");
    LOG_DEBUG("Accepted");
    EXPECT_GT(len, 0);
    auto ret = sock->write(header_data, sizeof(header_data) - 1);
    EXPECT_EQ(sizeof(header_data) - 1, ret);
    size_t offset = 0;
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

TEST(http_client, chunked) {
    auto server = new_tcp_socket_server();
    DEFER({ delete server; });
    server->set_handler({nullptr, &chunked_handler});
    auto ret = server->bind_v4localhost();
    if (ret < 0) LOG_ERROR(VALUE(errno));
    ret |= server->listen(100);
    if (ret < 0) LOG_ERROR(VALUE(errno));
    EXPECT_EQ(0, ret);
    LOG_INFO("Bind at `, Ready to accept", server->getsockname());
    server->start_loop();
    photon::thread_sleep(1);
    auto client = new_http_client();
    DEFER(delete client);
    auto url = to_url(server, "/");
    auto op = client->new_operation(Verb::GET, url);
    DEFER(client->destroy_operation(op));
    std::string buf;

    op->call();
    EXPECT_EQ(200, op->status_code);
    buf.resize(30);
    ret = op->resp.read((void*)buf.data(), 30);
    EXPECT_EQ(25, ret);
    buf.resize(25);
    EXPECT_EQ(true, buf == "first chunk second chunk ");
    LOG_DEBUG(VALUE(buf));

    server->set_handler({nullptr, &chunked_handler_complict});
    auto opc = client->new_operation(Verb::GET, url);
    DEFER(client->destroy_operation(opc));
    opc->call();
    EXPECT_EQ(200, opc->status_code);
    buf.resize(20000);
    ret = opc->resp.read((void*)buf.data(), 20000);
    EXPECT_EQ(10000 + 4090 + 4086 + 1024, ret);
    size_t i, cnt;
    for (i = cnt = 0; i < 10000 + 4090 + 4086 + 1024; i++)
        cnt += (buf[i] == 'a');
    EXPECT_EQ(i, cnt);

    std_data.resize(std_data_size);
    int num = 0;
    for (auto &c : std_data) {
        c = '0' + ((++num) % 10);
    }
    srand(time(0));
    server->set_handler({nullptr, &chunked_handler_pt});
    for (auto tmp = 0; tmp < 20; tmp++) {
        auto op_test = client->new_operation(Verb::GET, url);
        DEFER(client->destroy_operation(op_test));
        op_test->call();
        EXPECT_EQ(200, op_test->status_code);
        buf.resize(std_data_size);
        memset((void*)buf.data(), '0', std_data_size);
        ret = op_test->resp.read((void*)buf.data(), std_data_size);
        EXPECT_EQ(std_data_size, ret);
        EXPECT_EQ(true, buf == std_data);
        if (std_data_size != ret || buf != std_data) {
            std::cout << std::endl;
            std::cout << "n=" << rec.size() << std::endl;
            for (auto &a : rec) {
                std::cout << a << ",";
                std::cout << std::endl;
            }
            std::cout << std::endl;
            std::cout << "buffer = \n" << buf << std::endl;
            break;
        }
        op_test->resp.close();
        LOG_INFO("random chunked test ` passed", tmp);
    }
}

int wa_test[] = {5265,
6392,
4623,
7688,
7533,
4084,
8560,
7043,
7374,
4487,
2195,
292};

int chunked_handler_debug(void*, ISocketStream* sock) {
    EXPECT_NE(nullptr, sock);
    LOG_DEBUG("Accepted");
    char recv[4096];
    auto len = sock->recv(recv, 4096);
    EXPECT_GT(len, 0);
    auto ret = sock->write(header_data, sizeof(header_data) - 1);
    EXPECT_EQ(sizeof(header_data) - 1, ret);
    auto offset = 0;
    for (auto &a : wa_test) {
        chunked_send(offset, a, sock);
        offset += a;
    }
    sock->write("0\r\n\r\n", 5);
    return 0;
}

TEST(http_client, debug) {
    auto server = new_tcp_socket_server();
    DEFER({ delete server; });
    server->set_handler({nullptr, &chunked_handler_debug});
    auto ret = server->bind_v4localhost();
    // auto ret = server->bind(ep.port, ep.addr);
    if (ret < 0) LOG_ERROR(ERRNO());
    ret |= server->listen(100);
    if (ret < 0) LOG_ERROR(ERRNO());
    EXPECT_EQ(0, ret);
    LOG_INFO("Ready to accept");
    server->start_loop();
    photon::thread_sleep(1);

    std_data.resize(std_data_size);
    int num = 0;
    for (auto &c : std_data) {
        c = '0' + ((++num) % 10);
    }

    auto client = new_http_client();
    DEFER(delete client);
    auto op_test = client->new_operation(Verb::GET, to_url(server, "/"));
    DEFER(client->destroy_operation(op_test));
    op_test->call();
    EXPECT_EQ(200, op_test->status_code);
    std::string buf;
    buf.resize(std_data_size);
    memset((void*)buf.data(), '0', std_data_size);
    ret = op_test->resp.read((void*)buf.data(), std_data_size);
    EXPECT_EQ(std_data_size, ret);
    EXPECT_TRUE(buf == std_data);
    for (auto i: xrange(buf.size())) {
        if (buf[i] != std_data[i]) {
            LOG_ERROR("first occurrence of difference at: ", i);
            break;
        }
    }
}
int sleep_handler(void*, ISocketStream* sock) {
    photon::thread_sleep(3);
    return 0;
}
int dummy_body_writer(void* self, IStream* stream) { return 0; }

TEST(http_client, server_no_resp) {
    auto server = new_tcp_socket_server();
    DEFER(delete server);
    server->set_handler({nullptr, &sleep_handler});
    server->bind_v4localhost();
    server->listen();
    server->start_loop();

    auto client = new_http_client();
    DEFER(delete client);
    auto op = client->new_operation(Verb::GET, to_url(server, "/wtf"));
    DEFER(client->destroy_operation(op));
    op->req.headers.content_length(0);
    client->call(op);
    EXPECT_EQ(-1, op->status_code);
}

TEST(http_client, partial_body) {
    system("mkdir -p /tmp/photon-http-client-test/");
    system("echo \"this is a http_client request body text for socket stream\" > /tmp/photon-http-client-test/file");
    DEFER(system("rm -rf /tmp/photon-http-client-test/"));

    auto tcpserver = new_tcp_socket_server();
    DEFER(delete tcpserver);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    auto server = new_http_server();
    DEFER(delete server);
    auto fs = photon::fs::new_localfs_adaptor("/tmp/photon-http-client-test/");
    DEFER(delete fs);
    auto fs_handler = new_fs_handler(fs);
    DEFER(delete fs_handler);
    server->add_handler(fs_handler);
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();

    auto target_get = to_url(tcpserver, "/file");
    auto client = new_http_client();
    DEFER(delete client);
    auto op = client->new_operation(Verb::GET, target_get);
    DEFER(client->destroy_operation(op));
    op->req.headers.content_length(0);
    client->call(op);
    EXPECT_EQ(sizeof(socket_buf), op->resp.resource_size());
    std::string buf;
    buf.resize(10);
    op->resp.read((void*)buf.data(), 10);
    LOG_DEBUG(VALUE(buf));
    EXPECT_EQ(true, buf == "this is a ");
    op->resp.read((void*)buf.data(), 10);
    LOG_DEBUG(VALUE(buf));
    EXPECT_EQ(true, buf == "http_clien");
}


TEST(http_client, vcpu) {
    system("mkdir -p /tmp/ease_ut/http_test/");
    system("echo \"this is a http_client request body text for socket stream\" > /tmp/ease_ut/http_test/ease-httpclient-gettestfile");
    auto tcpserver = new_tcp_socket_server();
    tcpserver->setsockopt<int>(IPPROTO_TCP, TCP_NODELAY, 1);
    tcpserver->bind_v4localhost();
    tcpserver->listen();
    DEFER(delete tcpserver);
    auto server = new_http_server();
    DEFER(delete server);
    auto fs = photon::fs::new_localfs_adaptor("/tmp/ease_ut/http_test/");
    DEFER(delete fs);
    auto fs_handler = new_fs_handler(fs);
    DEFER(delete fs_handler);
    server->add_handler(fs_handler);
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();
    auto target = to_url(tcpserver, "/ease-httpclient-gettestfile");
    auto client = new_http_client();
    DEFER(delete client);

    photon::semaphore sem(0);
    constexpr int vcpu_num = 16;
    std::thread th[vcpu_num];
    for (int i = 0; i < vcpu_num; i++) {
        th[i] = std::thread([&] {
            photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
            DEFER({
                photon::fini();
                sem.signal(1);
            });

            for (int round = 0; round < 10; round++) {
                auto op = client->new_operation(Verb::GET, target);
                DEFER(client->destroy_operation(op));
                op->req.headers.content_length(0);
                int ret = client->call(op);
                GTEST_ASSERT_EQ(0, ret);

                char resp_body_buf[1024];
                EXPECT_EQ(sizeof(socket_buf), op->resp.resource_size());
                ret = op->resp.read(resp_body_buf, sizeof(socket_buf));
                EXPECT_EQ(sizeof(socket_buf), ret);
                resp_body_buf[sizeof(socket_buf) - 1] = '\0';
                LOG_DEBUG(resp_body_buf);
                EXPECT_EQ(0, strcmp(resp_body_buf, socket_buf));
            }
        });
    }

    sem.wait(vcpu_num);
    for (int i = 0; i < vcpu_num; i++)
        th[i].join();
}

TEST(DISABLED_http_client, ipv6) {  // make sure runing in a ipv6-ready environment
    auto client = new_http_client();
    DEFER(delete client);
    // here is an ipv6-only website
    auto op = client->new_operation(Verb::GET, "http://test6.ustc.edu.cn");
    DEFER(client->destroy_operation(op));
    op->call();
    EXPECT_EQ(200, op->resp.status_code());
}

TEST(http_client, unix_socket) {
    const char* uds_path = "test-http-client.sock";

    auto http_server = new_http_server();
    DEFER(delete http_server);
    http_server->add_handler(new SimpleHandler, true, "/simple-api");

    auto socket_server = new_uds_server(true);
    DEFER(delete socket_server);

    socket_server->set_handler(http_server->get_connection_handler());
    ASSERT_EQ(0, socket_server->bind(uds_path));
    ASSERT_EQ(0, socket_server->listen());
    ASSERT_EQ(0, socket_server->start_loop(false));

    auto client = new_http_client();
    DEFER(delete client);

    Client::OperationOnStack<> op(client, Verb::GET, "http://localhost/simple-api");
    int ret = op.call(uds_path);
    ASSERT_EQ(0, ret);
    ASSERT_EQ(200, op.resp.status_code());

    char buf[UINT16_MAX];
    ssize_t n = op.resp.read(buf, op.resp.body_size());
    ASSERT_EQ(n, (ssize_t) op.resp.body_size());
    LOG_INFO(buf);

    // A wrong hostname or HTTPS doesn't have effect on unix socket
    Client::OperationOnStack<> op2(client, Verb::GET, "https://www.wrong.hostname/simple-api");
    ret = op2.call(uds_path);
    ASSERT_EQ(0, ret);
    ASSERT_EQ(200, op2.resp.status_code());
}

int ua_check_handler(void*, Request &req, Response &resp, std::string_view) {
    auto ua = req.headers["User-Agent"];
    LOG_DEBUG(VALUE(ua));
    EXPECT_EQ(ua, "TEST_UA");
    resp.set_result(200);
    std::string str = "success";
    resp.headers.content_length(7);
    resp.write((void*)str.data(), str.size());
    return 0;
}

TEST(http_client, user_agent) {
    auto tcpserver = new_tcp_socket_server();
    DEFER(delete tcpserver);
    tcpserver->bind(18731);
    tcpserver->listen();
    auto server = new_http_server();
    DEFER(delete server);
    server->add_handler({nullptr, &ua_check_handler});
    tcpserver->set_handler(server->get_connection_handler());
    tcpserver->start_loop();

    std::string target_get = "http://localhost:18731/file";
    auto client = new_http_client();
    client->set_user_agent("TEST_UA");
    DEFER(delete client);
    auto op = client->new_operation(Verb::GET, target_get);
    DEFER(client->destroy_operation(op));
    op->req.headers.content_length(0);
    client->call(op);
    EXPECT_EQ(op->status_code, 200);
    std::string buf;
    buf.resize(op->resp.headers.content_length());
    op->resp.read((void*)buf.data(), op->resp.headers.content_length());
    LOG_DEBUG(VALUE(buf));
    EXPECT_EQ(true, buf == "success");
}

TEST(url, url_escape_unescape) {
    EXPECT_EQ(
        url_escape("?a=x:b&b=cd&c= feg&d=2/1[+]@alibaba.com&e='!bad';"),
        "%3Fa%3Dx%3Ab%26b%3Dcd%26c%3D%20feg%26d%3D2%2F1%5B%2B%5D%40alibaba.com%26e%3D%27%21bad%27%3B"
    );
    auto x = url_unescape("%3Fa%3Dx%3Ab%26b%3Dcd%26c%3D%20feg%26d%3D2%2F1%5B%2B%5D%40alibaba.com%26e%3D%27%21bad%27%3B");
    EXPECT_EQ(
        url_unescape("%3Fa%3Dx%3Ab%26b%3Dcd%26c%3D%20feg%26d%3D2%2F1%5B%2B%5D%40alibaba.com%26e%3D%27%21bad%27%3B"),
        "?a=x:b&b=cd&c= feg&d=2/1[+]@alibaba.com&e='!bad';"
    );
}

TEST(url, path_fix) {
    static const char url0[] = "http://xxx.com/yyy?a=b&c=d";
    URL u0(url0);
    EXPECT_EQ(u0.target(), "/yyy?a=b&c=d");
    EXPECT_EQ(u0.path(), "/yyy");
    EXPECT_EQ(u0.query(), "a=b&c=d");

    static const char url1[] = "http://xxx.com";
    URL u1(url1);
    EXPECT_EQ(u1.target(), "/");
    EXPECT_EQ(u1.path(), "/");
    EXPECT_EQ(u1.query(), "");

    static const char url2[] = "http://xxx.com?a=b";
    URL u2(url2);
    EXPECT_EQ(u2.target(), "/?a=b");
    EXPECT_EQ(u2.path(), "/");
    EXPECT_EQ(u2.query(), "a=b");
}

TEST(url, utils) {
    estring_view u1 = "http://www.taobao.com", u2 = "https://www.taobao.com";
    estring_view u3 = "HTTPS://www.taobao.com/", u4 = "www.taobao.com";
    ASSERT_EQ(1, what_protocol(u1));
    ASSERT_EQ(2, what_protocol(u2));
    ASSERT_EQ(2, what_protocol(u3));
    ASSERT_EQ(0, what_protocol(u4));
    URL url1("http://www.taobao.com:80"), url2("https://www.taobao.com:443");
    URL url3("http://www.taobao.com:8080"), url4("https://www.taobao.com:4443");
    ASSERT_EQ(false, need_optional_port(url1));
    ASSERT_EQ(false, need_optional_port(url2));
    ASSERT_EQ(true, need_optional_port(url3));
    ASSERT_EQ(true, need_optional_port(url4));
    estring_view ul1 = "http://www.taobao.com?a=1&b=2";
    estring_view ul2 = "http%3A%2F%2Fwww.taobao.com%3Fa%3D1%26b%3D2";
    ASSERT_EQ(0, url_escape(ul1).compare(ul2));
    ASSERT_EQ(0, url_unescape(ul2).compare(ul1));
}

// Only for manual test
// TEST(http_client, proxy) {
//     auto client = new_http_client();
//     DEFER(delete client);
//     client->set_proxy("http://localhost:8899/");
//     auto op = client->new_operation(Verb::delete_, "https://domain:1234/targetName");
//     DEFER(op->destroy());
//     LOG_DEBUG(VALUE(op->req.whole()));
//     op->req.redirect(Verb::GET, "baidu.com", true);
//     LOG_DEBUG(VALUE(op->req.whole()));
//     op->call();
//     EXPECT_EQ(200, op->status_code);
// }

int main(int argc, char** arg) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
#ifdef __linux__
    if (et_poller_init() < 0) {
        LOG_ERROR("et_poller_init failed");
        exit(EAGAIN);
    }
    DEFER(et_poller_fini());
#endif
    set_log_output_level(ALOG_DEBUG);
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}
