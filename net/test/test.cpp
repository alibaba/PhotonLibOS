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
#include <sys/stat.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/net/socket.h>
#ifdef ENABLE_CURL
#include <photon/net/curl.h>
#endif
#include <photon/net/utils.h>
#include <photon/net/iostream.h>
#include <photon/net/security-context/tls-stream.h>
#include "../../test/gtest.h"

#define protected public
#define private public
#include "../kernel_socket.cpp"
#undef protected
#undef private
#include "cert-key.cpp"

using namespace photon;
using namespace net;

char uds_path[] = "/tmp/udstest.sock";

void handler(ISocketStream* sock) {
    ASSERT_NE(nullptr, sock);
    LOG_DEBUG("Accepted");
    photon::thread_yield();
    char recv[256];
    auto len = sock->recv(recv, 256);
    LOG_DEBUG("RECV `", recv);
    sock->send(recv, len);
    LOG_DEBUG("SEND `", recv);
}

void uds_server() {
    auto sock = new_uds_server(true);
    DEFER({ delete (sock); });
    ASSERT_EQ(0, sock->bind(uds_path));
    ASSERT_EQ(0, sock->listen(100));
    char path[PATH_MAX];
    ASSERT_EQ(0, sock->getsockname(path, PATH_MAX));
    EXPECT_EQ(0, strcmp(path, uds_path));
    LOG_DEBUG("Listening `", path);
    handler(sock->accept());
    photon::thread_yield_to(nullptr);
}

void uds_client() {
    photon::thread_yield_to(nullptr);
    auto cli = new_uds_client();
    DEFER({ delete cli; });
    LOG_DEBUG("Connecting");
    auto sock = cli->connect(uds_path);
    DEFER(delete sock);
    LOG_DEBUG(VALUE(sock), VALUE(errno));
    char path[PATH_MAX];
    sock->getpeername(path, PATH_MAX);
    EXPECT_EQ(0, strcmp(path, uds_path));
    LOG_DEBUG("Connected `", path);
    char buff[] = "Hello";
    char recv[256];
    ssize_t ret = sock->send("Hello", 5);
    ASSERT_EQ(ret, 5);
    LOG_DEBUG("SEND `", buff);
    ret = sock->recv(recv, 5);
    ASSERT_EQ(ret, 5);
    LOG_DEBUG("RECV `", recv);
    EXPECT_EQ(0, memcmp(recv, buff, 5));
}

TEST(Socket, UDM_basic) {
    remove(uds_path);
    auto jh1 = photon::thread_enable_join(photon::thread_create11(uds_server));
    auto jh2 = photon::thread_enable_join(photon::thread_create11(uds_client));
    photon::thread_join(jh1);
    photon::thread_join(jh2);
    EXPECT_EQ(-1, remove(uds_path));
}

EndPoint ep;

void tcp_server() {
    auto sock = new_tcp_socket_server();
    DEFER({ delete sock; });
    auto ret = sock->bind_v4localhost();
    // auto ret = sock->bind(ep.port, ep.addr);
    ret |= sock->listen(100);
    LOG_DEBUG(VALUE(ret), VALUE(errno));
    ep = sock->getsockname();
    // EXPECT_TRUE(ep == epget);
    LOG_DEBUG("Listening `", ep);
    handler(sock->accept());
    photon::thread_yield_to(nullptr);
}

void tcp_client() {
    photon::thread_yield_to(nullptr);
    auto cli = new_tcp_socket_client();
    DEFER({ delete cli; });
    auto sock = cli->connect(ep);
    ASSERT_NE(sock, nullptr);
    DEFER(delete sock);
    LOG_DEBUG(VALUE(sock), VALUE(errno));
    EndPoint epget = sock->getpeername();
    LOG_DEBUG("Connected `", epget);
    EXPECT_EQ(ep.port, epget.port);
    char buff[] = "Hello";
    char recv[256];
    sock->send("Hello", 5);
    LOG_DEBUG("SEND `", buff);
    sock->recv(recv, 5);
    LOG_DEBUG("RECV `", recv);
    EXPECT_EQ(0, memcmp(recv, buff, 5));
}

TEST(Socket, TCP_basic) {
    remove(uds_path);
    auto jh1 = photon::thread_enable_join(photon::thread_create11(tcp_server));
    auto jh2 = photon::thread_enable_join(photon::thread_create11(tcp_client));
    photon::thread_join(jh1);
    photon::thread_join(jh2);
    remove(uds_path);
}

TEST(Socket, sockopt) {
    auto cli = new_tcp_socket_client();
    DEFER({ delete cli; });
    struct timeval timeo;
    timeo.tv_sec = 3600;
    timeo.tv_usec = 0;
    auto ret = cli->setsockopt(SOL_SOCKET, SO_SNDTIMEO, &timeo, sizeof(timeo));
    EXPECT_EQ(0, ret);
    struct timeval timeo_out {0, 0};
    socklen_t len_out = sizeof(timeo_out);
    ret = cli->getsockopt(SOL_SOCKET, SO_SNDTIMEO, &timeo_out, &len_out);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(timeo.tv_sec, timeo_out.tv_sec);
    EXPECT_EQ(timeo.tv_usec, timeo_out.tv_usec);
    EXPECT_EQ((socklen_t)sizeof(timeo), len_out);
}

class LogOutputTest final : public ILogOutput {
public:
    size_t _log_len;
    char _log_buf[4096];
    void write(int, const char* begin, const char* end) override {
        _log_len = end - begin;
        EXPECT_TRUE(_log_len < sizeof(_log_buf));
        _log_len--;
        memcpy(_log_buf, begin, _log_len);
        _log_buf[_log_len] = '\0';
    }
    int get_log_file_fd() override { return -1; }

    uint64_t set_throttle(uint64_t) override { return -1; }
    uint64_t get_throttle() override { return -1; }

    void destruct() override {}
} log_output_test;

TEST(Socket, endpoint) {
    EndPoint ep;
    struct in_addr inaddr;
    struct sockaddr_in saddrin;
    inet_aton("12.34.56.78", &inaddr);
    saddrin.sin_family = AF_INET;
    saddrin.sin_port = htons(4321);
    saddrin.sin_addr = inaddr;

    photon::net::sockaddr_storage s(saddrin);
    ep = s.to_endpoint();
    IPAddr addr12345678("12.34.56.78");
    EXPECT_TRUE(ep == EndPoint(addr12345678, 4321));

    auto rsai = (sockaddr_in*) s.get_sockaddr();
    EXPECT_EQ(saddrin.sin_addr.s_addr, rsai->sin_addr.s_addr);
    EXPECT_EQ(saddrin.sin_family, rsai->sin_family);
    EXPECT_EQ(saddrin.sin_port, rsai->sin_port);

    log_output = &log_output_test;
    LOG_DEBUG(ep);
    EXPECT_NE(nullptr, strstr(log_output_test._log_buf, "12.34.56.78:4321"));
    LOG_DEBUG(ep.addr);
    EXPECT_NE(nullptr, strstr(log_output_test._log_buf, "12.34.56.78"));
    log_output = log_output_stdout;

    EndPoint epfsv1("12.34.56.78:4321"), epfsv2("12.34.56.78", 4321);
    EXPECT_EQ(epfsv1.addr, addr12345678);
    EXPECT_EQ(epfsv1.port, 4321);
    EXPECT_EQ(epfsv1, epfsv2);

    std::vector<EndPoint> addrs;
    parse_address_list("1.1.1.1:1,2.2.2.2:2,3.3.3.3:3,4.4.4.4", &addrs, 4);
    EXPECT_EQ(addrs[0], EndPoint("1.1.1.1:1"));
    EXPECT_EQ(addrs[1], EndPoint("2.2.2.2:2"));
    EXPECT_EQ(addrs[2], EndPoint("3.3.3.3:3"));
    EXPECT_EQ(addrs[3], EndPoint("4.4.4.4:4"));
}

TEST(Socket, timeout) {
    errno = 0;
    auto cli = new_tcp_socket_client();
    auto* serv = new_tcp_socket_server();
    DEFER({
        delete cli;
        delete serv;
    });
    serv->bind_v4localhost();
    serv->listen(100);
    cli->timeout(1024UL * 1024);  // 1-sec;
    EXPECT_EQ(1024UL * 1024, cli->timeout());
    auto sock = cli->connect(serv->getsockname());
    DEFER(delete sock);
    EXPECT_NE(nullptr, sock);
    char buff[128];
    auto now = photon::now;
    auto ret = sock->recv(buff, 128);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ETIMEDOUT, errno);
    EXPECT_GE(photon::now - now, 1000 * 1000UL);
}

void prepare(char* snd, char* recv, struct iovec& siov, struct iovec& riov,
             size_t len = 128) {
    memset(recv, 0, len);
    siov.iov_base = snd;
    siov.iov_len = len;
    riov.iov_base = recv;
    riov.iov_len = len;
}

TEST(Socket, iov) {
    auto cli = new_tcp_socket_client();
    auto serv = new_tcp_socket_server();
    DEFER({
        delete cli;
        delete serv;
    });
    serv->bind_v4localhost();
    serv->listen(100);
    auto sock = cli->connect(serv->getsockname());
    DEFER(delete sock);
    char buff[128] = {0};
    char recv[128] = {0};
    strcpy(buff, "hello");
    struct iovec iov {
        buff, 128
    };
    struct iovec riov {
        recv, 128
    };
    const struct iovec* piov = &iov;
    const struct iovec* priov = &riov;
    auto ep = sock->getsockname();
    EndPoint epc;
    auto ss = serv->accept(&epc);
    EXPECT_TRUE(ep == epc);
    prepare(buff, recv, iov, riov);
    ss->send(&iov, 1);
    sock->recv(&riov, 1);
    EXPECT_EQ(0, memcmp(buff, recv, 128));
    prepare(buff, recv, iov, riov);
    ss->writev(&iov, 1);
    sock->readv(&riov, 1);
    EXPECT_EQ(0, memcmp(buff, recv, 128));
    prepare(buff, recv, iov, riov);
    ss->writev(piov, 1);
    sock->readv(priov, 1);
    EXPECT_EQ(0, memcmp(buff, recv, 128));
    memset(recv, 0, sizeof(recv));
    prepare(buff, recv, iov, riov);
    ss->write(buff, 128);
    sock->read(recv, 128);
    EXPECT_EQ(0, memcmp(buff, recv, 128));
}

#ifdef __linux__
TEST(ETServer, listen_twice) {
    auto server = net::new_et_tcp_socket_server();
    DEFER(delete server);
    server->bind_v4localhost();
    server->listen();
    int ret, err;
    ret = server->start_loop();
    EXPECT_EQ(0, ret);
    ret = server->start_loop();
    err = errno;
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EALREADY, err);
    server->terminate();
    ret = server->start_loop();
    EXPECT_EQ(0, ret);
}

EndPoint epet;

void et_tcp_server() {
    auto sock = new_et_tcp_socket_server();
    DEFER({ delete sock; });
    auto ret = sock->bind_v4localhost();
    LOG_DEBUG("before Listening");
    ret |= sock->listen(100);
    LOG_DEBUG(VALUE(ret), VALUE(errno));
    epet = sock->getsockname();
    LOG_DEBUG("Listening `", epet);
    handler(sock->accept());
    photon::thread_yield_to(nullptr);
}

void et_tcp_client() {
    photon::thread_yield_to(nullptr);
    auto cli = new_et_tcp_socket_client();
    DEFER({ delete cli; });
    LOG_DEBUG("Connecting");
    auto sock = cli->connect(epet);
    DEFER(delete sock);
    LOG_DEBUG(VALUE(sock), VALUE(errno));
    char buff[] = "Hello";
    char recv[256];
    sock->send("Hello", 5);
    LOG_DEBUG("SEND `", buff);
    sock->recv(recv, 5);
    LOG_DEBUG("RECV `", recv);
    EXPECT_EQ(0, memcmp(recv, buff, 5));
}

TEST(ETSocket, TCP_basic) {
    remove(uds_path);
    auto jh1 = photon::thread_enable_join(photon::thread_create11(et_tcp_server));
    auto jh2 = photon::thread_enable_join(photon::thread_create11(et_tcp_client));
    photon::thread_join(jh1);
    photon::thread_join(jh2);
    remove(uds_path);
}

TEST(ETSocket, timeout) {
    errno = 0;
    auto cli = new_et_tcp_socket_client();
    auto* serv = new_et_tcp_socket_server();
    DEFER({
        delete cli;
        delete serv;
    });
    serv->bind_v4localhost();
    serv->listen(100);
    cli->timeout(1024UL * 1024);  // 1-sec;
    EXPECT_EQ(1024UL * 1024, cli->timeout());
    auto sock = cli->connect(serv->getsockname());
    DEFER(delete sock);
    EXPECT_NE(nullptr, sock);
    char buff[128];
    auto now = photon::now;
    auto ret = sock->recv(buff, 128);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ETIMEDOUT, errno);
    EXPECT_GE(photon::now - now, 1000 * 1000UL);
}

void ETSocket_iov_test_cli_connect(ISocketStream** sock, ISocketClient* cli, ISocketServer* serv) {
    LOG_DEBUG("enter tmp_thread");
    *sock = cli->connect(serv->getsockname());
    LOG_DEBUG("leave tmp_thread");
}

TEST(ETSocket, iov) {
    LOG_DEBUG("test ETSocket.iov");
    auto cli = new_et_tcp_socket_client();
    auto serv = new_et_tcp_socket_server();
    DEFER({
        delete cli;
        delete serv;
    });
    serv->bind_v4localhost();
    serv->listen(100);
    // serv->start_loop();
    ISocketStream* sock;
    auto th = photon::thread_create11(ETSocket_iov_test_cli_connect, &sock, cli, serv);
    auto jh1 = photon::thread_enable_join(th);
    photon::thread_yield_to(th);
    // LOG_DEBUG("connected");
    DEFER(delete sock);
    char buff[128] = {0};
    char recv[128] = {0};
    strcpy(buff, "hello");
    struct iovec iov {
        buff, 128
    };
    struct iovec riov {
        recv, 128
    };
    const struct iovec* piov = &iov;
    const struct iovec* priov = &riov;
    EndPoint epc;
    LOG_DEBUG("before Accept");
    auto ss = serv->accept(&epc);
    LOG_DEBUG("after Accept");
    photon::thread_join(jh1);
    auto ep = sock->getsockname();
    EXPECT_TRUE(ep == epc);
    prepare(buff, recv, iov, riov);
    ss->send(&iov, 1);
    sock->recv(&riov, 1);
    EXPECT_EQ(0, memcmp(buff, recv, 128));
    prepare(buff, recv, iov, riov);
    ss->writev(&iov, 1);
    sock->readv(&riov, 1);
    EXPECT_EQ(0, memcmp(buff, recv, 128));
    prepare(buff, recv, iov, riov);
    ss->writev(piov, 1);
    sock->readv(priov, 1);
    EXPECT_EQ(0, memcmp(buff, recv, 128));
    memset(recv, 0, sizeof(recv));
    prepare(buff, recv, iov, riov);
    ss->write(buff, 128);
    sock->read(recv, 128);
    EXPECT_EQ(0, memcmp(buff, recv, 128));
}
#endif

TEST(Socket, autoremove) {
    static const char path[] = "/tmp/testnosock";
    // 1. do not remove file if the file is not socket
    remove(path);
    auto fd = open(path, O_RDWR | O_CREAT, 0777);
    if (fd != -1) close(fd);
    auto sock = new_uds_server(true);
    auto ret = sock->bind(path);
    EXPECT_EQ(-1, ret);
    remove(path);

    // 2. do not remove if autoremove is false
    auto sock_noar = new_uds_server();
    ret = sock_noar->bind(path);
    EXPECT_EQ(0, ret);
    delete sock_noar;
    struct stat statbuf;
    ret = stat(path, &statbuf);
    EXPECT_EQ(0, ret);
    EXPECT_NE(0, S_ISSOCK(statbuf.st_mode));

    // 3. do remove when binding
    ret = sock->bind(path);
    EXPECT_EQ(0, ret);

    // 4. do remove when closing
    delete sock;
    ret = stat(path, &statbuf);
    EXPECT_EQ(-1, ret);
}

TEST(Socket, faults) {
    auto o_log_output = log_output;
    log_output = log_output_null;
    DEFER({ log_output = o_log_output; });
    auto sock = new_uds_server(true);
    DEFER({ delete sock; });
    // 1. long path ---- path string longer than 108 bytes
    char extreme_long_path[] =
        "/tmp/"
        "ahbfdjksalfhdjksalfhjdklsahfjdkslahfjdkslahfjdkslahfjkdlsahjfkdlsahfjk"
        "dlsahfjkdlahfjkdlashfjkdlsahfjdkslahf";
    auto ret = sock->bind(extreme_long_path);
    EXPECT_EQ(-1, ret);
    auto scl = new_uds_client();
    DEFER(delete scl);
    auto ts = scl->connect(extreme_long_path);
    EXPECT_EQ(nullptr, ts);
    // 2. connect failed in unix sock;
    char file_not_exists[] = "/tmp/somehow_i_have_never_create_this_socket";
    ts = scl->connect(file_not_exists);
    EXPECT_EQ(nullptr, ts);
}

void test_server_start_and_terminate(bool blocking) {
    auto server = net::new_tcp_socket_server();
    DEFER(delete server);
    auto th = photon::thread_create11([&]{
        server->bind_v4localhost();
        server->listen();
        server->start_loop(blocking);
    });
    photon::thread_enable_join(th);
    photon::thread_usleep(200'000);
    server->terminate();
    photon::thread_join((photon::join_handle*) th);
}

TEST(TCPServer, start_and_terminate_blocking) {
    test_server_start_and_terminate(true);
}

TEST(TCPServer, start_and_terminate_nonblocking) {
    test_server_start_and_terminate(false);
}

TEST(TCPServer, listen_twice) {
    auto server = net::new_tcp_socket_server();
    DEFER(delete server);
    server->bind_v4localhost();
    server->listen();
    int ret, err;
    ret = server->start_loop();
    EXPECT_EQ(0, ret);
    ret = server->start_loop();
    err = errno;
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EALREADY, err);
    server->terminate();
    ret = server->start_loop();
    EXPECT_EQ(0, ret);
}

TEST(TLSSocket, basic) {
    photon::condition_variable recved;

    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    ASSERT_NE(ctx, nullptr);
    DEFER(delete ctx);

    auto server = net::new_tls_server(ctx, net::new_tcp_socket_server(), true);
    DEFER(delete server);

    server->bind_v4localhost();
    server->timeout(10UL * 1024 * 1024);

    auto logHandle = [&](ISocketStream* sock) {
        char buff[4096];
        ssize_t len;
        len = sock->read(buff, 6);
        EXPECT_EQ(6, len);
        LOG_DEBUG(ALogString(buff, len));
        recved.notify_all();
        return 0;
    };
    server->set_handler(logHandle);
    int ret = server->listen();
    EXPECT_EQ(0, ret);
    server->start_loop();
    auto cli = net::new_tls_client(ctx, net::new_tcp_socket_client(), true);
    DEFER(delete cli);
    cli->timeout(10 * 1024 * 1024);
    auto sock = cli->connect(server->getsockname());
    DEFER(delete sock);
    EXPECT_EQ(0, ret);
    LOG_DEBUG(ERRNO());
    ret = sock->send("Hello\n", 6);
    EXPECT_EQ(6, ret);
    recved.wait_no_lock();
}

void test_log_sockaddr_in() {
    struct sockaddr_in myaddr;
    myaddr.sin_family = AF_INET;
    myaddr.sin_port = htons(3490);
    inet_aton("63.161.169.137", &myaddr.sin_addr);
    LOG_DEBUG(myaddr);
}

void test_log_sockaddr() {
    struct sockaddr myaddr0;
    myaddr0.sa_family = 0;

    auto myaddr = (struct sockaddr_in&)myaddr0;
    myaddr.sin_family = AF_INET;
    myaddr.sin_port = htons(5678);
    inet_aton("1.2.3.4", &myaddr.sin_addr);
    LOG_DEBUG(myaddr);
}

bool server_down = false;
photon::thread* server_thread = nullptr;

void* serve_connection(void* arg) {
    auto fd = (int)(uint64_t)arg;
    while (true) {
        char buf[1024];
        auto ret = net::read(fd, buf, sizeof(buf));
        if (ret <= 0) LOG_ERRNO_RETURN(0, nullptr, "failed to photon::read()");

        auto retw = net::write_n(fd, buf, ret);
        if (retw < ret) {
            LOG_ERRNO_RETURN(0, nullptr, "failed to photon::write_n()");
        } else {
            if (memcmp(buf, "quit", 4) == 0) {
                LOG_DEBUG("server receive 'quit'");
                close(fd);
                server_down = true;
                break;
            }
        }
    }
    return nullptr;
}

uint16_t _srvport = 0;

int test_socket_server() {
    server_thread = photon::CURRENT;
    int fd = net::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) LOG_ERRNO_RETURN(0, -1, "failed to photon::socket()");

    int state = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &state, sizeof(state));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    // addr.sin_port = htons(12888);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int ret = bind(fd, (sockaddr*)&addr, sizeof(addr));
    if (ret < 0) LOG_ERRNO_RETURN(0, -1, "failed to bind() to ", addr);

    socklen_t len = sizeof(addr);
    ret = ::getsockname(fd, (sockaddr*)&addr, &len);
    if (ret < 0) LOG_ERRNO_RETURN(0, -1, "failed to getsockname()");
    assert(len == sizeof(addr));
    _srvport = ntohs(addr.sin_port);

    ret = listen(fd, 50);
    if (ret < 0) LOG_ERRNO_RETURN(0, -1, "failed to listen()");

    LOG_INFO("Start listening at ", addr);
    while (true) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        LOG_DEBUG("before accept");
        int64_t cfd = net::accept(fd, (sockaddr*)&addr, &len);
        LOG_DEBUG("after accept");
        if (cfd < 0) {
            return -1;
        }

        LOG_INFO("new connection from ", addr);
        photon::thread_create(&serve_connection, (void*)cfd);

        if (server_down) {
            break;
        }
    }
    return 0;
}

TEST(ConnectTest, HandleNoneZeroInput) {
    struct sockaddr_in addr;
    bzero(&addr, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(_srvport);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int fd = net::socket(AF_INET, SOCK_STREAM, 0);

    photon::thread_usleep(1000 * 200);
    puts("before connect()");
    int ret =
        net::connect(fd, (struct sockaddr*)(&addr), sizeof(struct sockaddr));
    puts("after connect()");
    EXPECT_EQ(ret, 0);
    if (ret < 0) return;

    char buf[] = "oqi3njsdy9314l;kdsfvk23;ie";
    ret = net::write_n(fd, buf, sizeof(buf));
    EXPECT_EQ((size_t)ret, sizeof(buf));

    char buf2[sizeof(buf)]{};
    ret = net::read_n(fd, buf2, sizeof(buf2));
    EXPECT_EQ((size_t)ret, sizeof(buf2));
    EXPECT_TRUE(memcmp(buf, buf2, sizeof(buf)) == 0);

    ret = net::write_n(fd, "quit", 4);
    EXPECT_EQ(ret, 4);
    photon::thread_usleep(1000 * 200);
    close(fd);
}

template <typename Writer>
void test_writer(Writer& writer) {
    ssize_t ret;
    ret = writer.write("123", 3);
    EXPECT_EQ(3, ret);
    ret = writer.write("456", 3);
    EXPECT_EQ(3, ret);
    EXPECT_EQ(0, strncmp("123456", writer.alog_string().s, 6));
}

#ifdef ENABLE_CURL
TEST(writers, multiple_segment) {
    StringWriter sw;
    test_writer(sw);
    char buffer[256];
    BufWriter bw(buffer, 256);
    test_writer(bw);
    BufferWriter<256> bfw;
    test_writer(bfw);
}
#endif

void* start_server(void*) {
    test_socket_server();
    return nullptr;
}

TEST(utils, gethostbyname) {
    net::IPAddr localhost("127.0.0.1");
    net::IPAddr addr;
    int ret = net::gethostbyname("localhost", &addr);
    ASSERT_GE(ret, 1);
    LOG_DEBUG(VALUE(localhost), VALUE(addr));
    EXPECT_TRUE(localhost.is_localhost() && addr.is_localhost());

    std::vector<net::IPAddr> addrs;
    net::gethostbyname("localhost", addrs);
    EXPECT_GT(addrs.size(), 0);
    for (auto &x : addrs) {
        LOG_INFO(VALUE(x));
        EXPECT_TRUE(x.is_localhost());
    }

    net::IPAddr host = net::gethostbypeer("localhost");
    EXPECT_TRUE(host.is_localhost());
}

TEST(utils, resolver) {
    auto *resolver = new_default_resolver();
    DEFER(delete resolver);
    net::IPAddr addr = resolver->resolve("localhost");
    if (addr.is_ipv4()) {
        EXPECT_EQ(net::IPAddr::V4Loopback(), addr);
    } else {
        EXPECT_EQ(net::IPAddr::V6Loopback(), addr);
    }
}

TEST(utils, resolver_filter) {
    auto *resolver = new_default_resolver();
    DEFER(delete resolver);
    auto filter = [&](net::IPAddr addr_) -> bool {
        return !addr_.is_ipv4();
    };
    auto addr = resolver->resolve_filter("localhost", filter);
    ASSERT_TRUE(!addr.is_ipv4());
}

TEST(utils, resolver_discard) {
    auto *resolver = new_default_resolver();
    DEFER(delete resolver);
    (void) resolver->resolve("localhost");
    resolver->discard_cache("non-exist-host.com");
    resolver->discard_cache("localhost");
}

#ifdef __linux__
TEST(ZeroCopySocket, basic) {
    if (!zerocopy_available()) {
        return;
    }

    EndPoint ep_src(IPAddr("127.0.0.1"), 13659), ep_dst;
    const size_t size = 8192;
    char send_buf[size], recv_buf[size];
    memset(send_buf, 'x', size);
    bool ok = false;
    ISocketServer* server;

    auto handler = [&](ISocketStream* stream) -> int {
        if (stream->recv(recv_buf, size) != (ssize_t) size) {
            ok = false;
            LOG_ERRNO_RETURN(0, -1, "recv fail");
        }
        ok = true;
        return 0;
    };

    auto run_server = [&] {
        server = new_zerocopy_tcp_server();
        DEFER(delete server);
        ASSERT_EQ(server->bind_v4localhost(), 0);
        ep_dst = server->getsockname();
        server->set_handler(handler);
        ASSERT_EQ(server->listen(), 0);
        ASSERT_EQ(server->start_loop(true), 0);
    };

    auto server_th = photon::thread_create11(run_server);
    photon::thread_enable_join(server_th);
    photon::thread_sleep(1);

    auto client = new_tcp_socket_client();
    DEFER(delete client);
    auto conn = client->connect(ep_dst, &ep_src);
    ASSERT_NE(conn, nullptr);
    DEFER(delete conn);

    ssize_t ret = conn->send(send_buf, size);
    ASSERT_EQ(ret, size);

    photon::thread_sleep(1);
    ASSERT_TRUE(ok);
    ASSERT_EQ(memcmp(send_buf, recv_buf, size), 0);

    server->terminate();
    photon::thread_join((join_handle*) server_th);
}
#endif

const static char LINE[] = "hello iostream over socket stream!";

void iostream_uds_server() {
    auto server = new_uds_server(true);
    DEFER({ delete (server); });
    ASSERT_EQ(0, server->bind(uds_path));
    ASSERT_EQ(0, server->listen(100));
    auto connection = server->accept();
    EXPECT_TRUE(connection);
    auto ios = new_iostream(connection, true);
    DEFER(delete ios);
    EXPECT_TRUE(ios);
    char line[4096];
    ios->getline(line, sizeof(line));
    LOG_DEBUG("got line: '`'", line);
    *ios << 123456;
    ASSERT_STREQ(line, LINE);
}

TEST(iostream, UDS) {
    remove(uds_path);
    thread_create11(iostream_uds_server);
    thread_yield();
    // ASSERT_EQ(::access(uds_path, F_OK, AT_EACCESS), 0);
    auto cli = new_uds_client();
    DEFER({ delete cli; });
    auto sock = cli->connect(uds_path);
    EXPECT_TRUE(sock);
    auto ios = new_iostream(sock, true);
    DEFER(delete ios);
    *ios << LINE << std::endl;
    uint64_t x;
    *ios >> x;
    ASSERT_EQ(x, 123456);
    remove(uds_path);
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
    ::testing::InitGoogleTest(&argc, arg);

    test_log_sockaddr_in();
    test_log_sockaddr();
    photon::thread_create(&start_server, nullptr);

    int ret = RUN_ALL_TESTS();
    LOG_DEBUG("test result: ", ret);
    server_down = true;
    photon::thread_interrupt(server_thread);
    return ret;
}
