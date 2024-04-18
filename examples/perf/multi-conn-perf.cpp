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

// This is a performance test for multiple connections and OS threads

#include <atomic>
#include <thread>
#include <gflags/gflags.h>

#include <photon/photon.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/net/socket.h>
#include <photon/net/basic_socket.h>
#include <photon/common/utility.h>

DEFINE_uint64(client_thread_num, 8, "client thread number");
DEFINE_uint64(server_thread_num, 8, "server thread number");
DEFINE_string(ip, "127.0.0.1", "ip");
DEFINE_uint64(port, 20000, "port");
DEFINE_uint64(buf_size, 512, "buffer size");
DEFINE_bool(cascading_engine, false, "Use cascading engine instead of master engine");
DEFINE_uint64(mode, 0, "0: standalone, 1: client, 2: server");

enum class Mode {
    Standalone,
    Client,
    Server,
};

static std::atomic<uint64_t> qps{0};

static void run_qps_loop() {
    while (true) {
        photon::thread_sleep(1);
        LOG_INFO("qps: `", qps.load());
        qps = 0;
    }
}

static void do_write(int server_index) {
    photon::net::EndPoint ep(FLAGS_ip.c_str(), FLAGS_port + server_index);
    auto cli = photon::net::new_tcp_socket_client();
    auto conn = cli->connect(ep);
    if (!conn) {
        LOG_ERROR("connect failed");
        exit(1);
    }

    LOG_INFO("Client `, Server `", conn->getsockname(), conn->getpeername());
    char buf[FLAGS_buf_size];
    while (true) {
        ssize_t ret = conn->write(buf, FLAGS_buf_size);
        if (ret != (ssize_t) FLAGS_buf_size)
            exit(1);
        photon::thread_yield();
    }
}

static int client(int client_index) {
    std::string name = "client_" + std::to_string(client_index);
    pthread_setname_np(pthread_self(), name.c_str());

    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);

    for (auto i: xrange(FLAGS_server_thread_num)) {
        photon::thread_create11(do_write, i);
    }
    photon::thread_sleep(-1);
    return 0;
}

static void server(int server_index) {
    std::string name = "server_" + std::to_string(server_index);
    pthread_setname_np(pthread_self(), name.c_str());

    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);

    photon::CascadingEventEngine* engine = nullptr;
    if (FLAGS_cascading_engine) {
        LOG_INFO("Run server with cascading engine ...");
        engine = photon::new_default_cascading_engine();
    } else {
        LOG_INFO("Run server with master engine ...");
    }

    photon::net::EndPoint ep("0.0.0.0", FLAGS_port + server_index);
    photon::net::sockaddr_storage storage(ep);
    int sock_fd = photon::net::socket(AF_INET, SOCK_STREAM, 0);
    int val = 1;
    socklen_t len_opt = sizeof(val);
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &val, len_opt);
    bind(sock_fd, storage.get_sockaddr(), storage.get_socklen());
    listen(sock_fd, 1024);
    socklen_t len;

    // Cascading engine loop
    if (FLAGS_cascading_engine) {
        photon::thread_create11([&] {
            char buf[FLAGS_buf_size];
            void* data[1024];
            while (true) {
                ssize_t num_events = engine->wait_for_events(data, 1024);
                for (int i = 0; i < num_events; ++i) {
                    int fd = (int) (uintptr_t) data[i];
                    ssize_t n = photon::net::read_n(fd, buf, sizeof(buf));
                    if (n != (ssize_t)sizeof(buf))
                        exit(1);
                    qps++;
                }
            }
        });
    }

    auto master_engine_loop = [&] (int _conn_fd) {
        char buf[FLAGS_buf_size];
        while (true) {
            ssize_t n = photon::net::read_n(_conn_fd, buf, sizeof(buf));
            if (n != (ssize_t)sizeof(buf))
                exit(1);
            qps++;
        }
    };

    // Socket accept loop
    while (true) {
        int conn_fd = photon::net::accept(sock_fd, storage.get_sockaddr(), &len);
        if (FLAGS_cascading_engine) {
            // Register conn_fd as the epoll data
            photon::CascadingEventEngine::Event ev{conn_fd, photon::EVENT_READ, (void*) (uintptr_t) conn_fd};
            engine->add_interest(ev);
        } else {
            // Master engine loop
            photon::thread_create11(master_engine_loop, conn_fd);
        }
    }
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    set_log_output_level(ALOG_INFO);

    int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    if (ret < 0) {
        LOG_ERROR_RETURN(0, -1, "failed to init photon environment");
    }
    DEFER(photon::fini());

    if (Mode(FLAGS_mode) == Mode::Standalone || Mode(FLAGS_mode) == Mode::Server) {
        photon::thread_create11(run_qps_loop);
        for (auto i: xrange(FLAGS_server_thread_num)) {
            std::thread(server, i).detach();
        }
    }

    if (Mode(FLAGS_mode) == Mode::Standalone || Mode(FLAGS_mode) == Mode::Client) {
        photon::thread_sleep(1);
        for (auto i: xrange(FLAGS_client_thread_num)) {
            std::thread(client, i).detach();
        }
    }
    photon::thread_sleep(-1);
}