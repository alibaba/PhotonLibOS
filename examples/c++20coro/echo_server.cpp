#include <photon/net/socket.h>
#include <photon/photon.h>
#include <photon/thread/coro20.h>

#include <algorithm>
#include <ranges>

static uint64_t throughput = 0;

photon::coro::FixedGenerator<int> echo(photon::net::ISocketStream* sock) {
    char buffer[4096];
    for (;;) {
        auto ret = sock->recv(buffer, sizeof(buffer));
        if (ret <= 0) co_return ret;
        auto len = sock->write(buffer, ret);
        if (ret != len) co_return -1;
        co_yield ret;
    }
}

photon::coro::Coro<void> session(photon::net::ISocketStream* sock,
                                 photon::net::EndPoint sockname) {
    DEFER(delete sock);
    auto action = echo(sock);
    std::ranges::for_each(
        action | std::views::take_while([](int ret) { return ret > 0; }),
        [](int bytes) { throughput += bytes; });
    LOG_INFO("Done `", sockname);
    co_return;
}

photon::coro::FixedGenerator<photon::net::ISocketStream*> socket_accept(
    photon::net::ISocketServer* sock) {
    for (;;) {
        co_yield sock->accept();
    }
}

photon::coro::Coro<void> server(int port) {
    auto server = photon::net::new_tcp_socket_server();
    DEFER(delete server);
    server->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    server->bind(port);
    server->listen();
    std::ranges::for_each(socket_accept(server), [&](auto sess) {
        auto name = sess->getpeername();
        sess->timeout(10UL * 1024 * 1024);
        LOG_INFO("Accept ", name);
        photon::coro::async_run(session, sess, name);
    });
    co_return;
}

photon::coro::Coro<void> throughput_print() {
    std::ranges::for_each(photon::coro::timer(1'000'000), [&](int) {
        LOG_INFO("Recv ` MB/s", throughput / 1024 / 1024);
        throughput = 0;
    });
    co_return;
}

int main() {
    photon::init(photon::INIT_EVENT_EPOLL, 0);
    DEFER(photon::fini());
    photon::coro::async_run(throughput_print);
    photon::coro::run(server, 23721);
    return 0;
}