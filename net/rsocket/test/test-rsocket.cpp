#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog.h>
#include <photon/net/rsocket/rsocket.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/timer.h>

DEFINE_string(host, "127.0.0.1", "rsocket server address");
DEFINE_int32(port, 95271, "rsocket port");
DEFINE_int32(jobs, 4, "number of jobs");
static uint64_t count = 0;

void* task(void* arg) {
    auto client = (photon::net::ISocketClient*)arg;
    char buffer[65536];

    auto sess = client->connect(photon::net::EndPoint(
        photon::net::IPAddr(FLAGS_host.c_str()), FLAGS_port));
    if (!sess) {
        LOG_ERRNO_RETURN(0, nullptr, "connect");
    }
    DEFER(delete sess);

    photon::Timeout tmo(5UL * 1024 * 1024);

    while (!tmo.expired()) {
        auto nsend = sess->send(buffer, sizeof(buffer));
        EXPECT_TRUE(nsend >= 0);
        if (nsend == 0) {
            break;
        }
        auto nrecv = sess->read(buffer, nsend);
        EXPECT_TRUE(nrecv >= 0);
        EXPECT_EQ(nsend, nrecv);
    }
    return nullptr;
}

int handler(void*, photon::net::ISocketStream* sock) {
    auto ep = sock->getpeername();
    LOG_INFO("Got connection `", ep);
    DEFER(LOG_INFO("Connection closed `", ep));
    char buffer[65536];
    for (;;) {
        auto nrecv = sock->recv(buffer, sizeof(buffer));
        if (nrecv <= 0) break;
        auto nsend = sock->write(buffer, nrecv);
        if (nsend != nrecv) break;
        count += nsend;
    }
    return 0;
}

uint64_t ontime(void*) {
    LOG_INFO("` MB/s", count / 1024 / 1024);
    count = 0;
    return 0;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    photon::net::EndPoint ep(FLAGS_host.c_str(), FLAGS_port);
    if (ep.addr.is_localhost()) {
        LOG_ERROR_RETURN(
            0, EINVAL, "May not support lo address, use IP of RDMA interface");
    }

    photon::init();
    DEFER(photon::fini());
    default_logger.log_level = ALOG_DEBUG;

    LOG_INFO(VALUE(FLAGS_host), VALUE(FLAGS_port), VALUE(FLAGS_jobs));

    auto server = photon::net::new_rsocket_server();
    if (!server) {
        LOG_ERRNO_RETURN(0, -1, "new_rsocket_client");
    }
    DEFER(delete server);

    auto ret =
        server->bind(photon::net::EndPoint(FLAGS_host.c_str(), FLAGS_port));
    if (ret < 0) {
        LOG_ERRNO_RETURN(0, -1, "bind");
    }

    ret = server->listen();
    if (ret < 0) {
        LOG_ERRNO_RETURN(0, -1, "listen");
    }

    server->set_handler({handler, nullptr});

    photon::Timer timer(1UL * 1000 * 1000, {ontime, nullptr});

    server->start_loop(false);

    auto client = photon::net::new_rsocket_client();
    if (!client) {
        LOG_ERRNO_RETURN(0, -1, "new_rsocket_client");
    }
    DEFER(delete client);

    photon::threads_create_join(FLAGS_jobs, task, (void*)client);

    return 0;
}
