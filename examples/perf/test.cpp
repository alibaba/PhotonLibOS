#include <atomic>
#include <thread>
#include <gflags/gflags.h>

#include <photon/photon.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/net/socket.h>
#include <photon/net/basic_socket.h>
#include "../../net/base_socket.h"

DEFINE_uint64(thread_num, 24, "server thread number");
DEFINE_string(ip, "127.0.0.1", "ip");
DEFINE_uint64(port, 9527, "port");
DEFINE_uint64(buf_size, 512, "buffer size");

static bool stop_test = false;
static std::atomic<uint64_t> qps{0};

static void run_qps_loop() {
    while (!stop_test) {
        photon::thread_sleep(1);
        LOG_INFO("qps: `", qps.load());
        qps = 0;
    }
}

static int client() {
    photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE);
    auto cli = photon::net::new_tcp_socket_client();

    for (int i = 0; i < FLAGS_thread_num; ++i) {
        photon::thread_create11([&, i]{
            photon::net::EndPoint ep(FLAGS_ip.c_str(), FLAGS_port + i);
            LOG_INFO("Connect to ", ep);
            auto conn = cli->connect(ep);
            char buf[FLAGS_buf_size];
            while (!stop_test) {
                // photon::thread_usleep(100);
                ssize_t ret = conn->write(buf, FLAGS_buf_size);
                if (ret != (ssize_t) FLAGS_buf_size) {
                    LOG_ERROR("fail to write");
                }
                photon::thread_yield();
            }
        });
    }
    photon::thread_sleep(-1);
    return 0;
}

static int server(int index) {
    photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE);

    photon::net::EndPoint ep("0.0.0.0", FLAGS_port + index);
    photon::net::sockaddr_storage storage(ep);
    int sock_fd = photon::net::socket(AF_INET, SOCK_STREAM, 0);
    int val = 1;
    socklen_t len_opt = sizeof(val);
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &val, len_opt);
    bind(sock_fd, storage.get_sockaddr(), storage.get_socklen());
    listen(sock_fd, 1024);
    socklen_t len;
    int conn_fd = photon::net::accept(sock_fd, storage.get_sockaddr(), &len);

    LOG_INFO("Server: Got new connection on fd `", conn_fd);

    auto engine = photon::new_epoll_cascading_engine();
    photon::CascadingEventEngine::Event ev{conn_fd, photon::EVENT_READ, (void*) (uintptr_t) conn_fd};
    engine->add_interest(ev);

    void* data[100];
    char buf[FLAGS_buf_size];
    while (true) {
        ssize_t n = engine->wait_for_events(data, 100);
        for (int i = 0; i < n; ++i) {
            int fd = (int) (uintptr_t) data[i];
            photon::net::read_n(fd, buf, sizeof(buf));
            qps++;
        }
    }
    return 0;
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    set_log_output_level(ALOG_INFO);

    int ret = photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE);
    if (ret < 0) {
        LOG_ERROR_RETURN(0, -1, "failed to init photon environment");
    }
    DEFER(photon::fini());

    for (int i = 0; i < FLAGS_thread_num; ++i) {
        new std::thread(server, i);
    }
    photon::thread_sleep(1);

    new std::thread(client);

    photon::thread_create11(run_qps_loop);
    photon::thread_sleep(-1);
}
