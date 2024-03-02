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
#include <chrono>

#include <gflags/gflags.h>

#include <photon/photon.h>
#include <photon/io/signal.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>
#include <photon/common/alog.h>
#include <photon/net/socket.h>

DEFINE_uint64(show_statistics_interval, 1, "interval seconds to show statistics");
DEFINE_bool(client, false, "client or server? default is server");
DEFINE_string(client_mode, "streaming", "client mode. Choose between streaming or ping-pong");
DEFINE_uint64(client_connection_num, 100, "number of the connections of the client, only available in ping-pong mode");
DEFINE_string(ip, "127.0.0.1", "ip");
DEFINE_uint64(port, 9527, "port");
DEFINE_uint64(buf_size, 512, "buffer size");
DEFINE_uint64(vcpu_num, 1, "server vcpu num. Increase this value to enable multi-vcpu scheduling");

static int event_engine = 0;
static bool stop_test = false;
static uint64_t qps = 0;
static uint64_t time_cost = 0;

static void handle_signal(int sig) {
    LOG_INFO("Try to gracefully stop test ...");
    stop_test = true;
}

static void run_qps_loop() {
    while (!stop_test) {
        photon::thread_sleep(FLAGS_show_statistics_interval);
        LOG_INFO("qps: `", qps / FLAGS_show_statistics_interval);
        qps = 0;
    }
}

static void run_latency_loop() {
    while (!stop_test) {
        photon::thread_sleep(FLAGS_show_statistics_interval);
        uint64_t lat = (qps != 0) ? (time_cost / qps) : 0;
        LOG_INFO("latency: ` us", lat);
        qps = time_cost = 0;
    }
}

// Create coroutines for each of the connections, doing ping-pong send/recv
static int ping_pong_client() {
    photon::net::EndPoint ep{photon::net::IPAddr(FLAGS_ip.c_str()), (uint16_t) FLAGS_port};
    auto cli = photon::net::new_tcp_socket_client();
    // auto cli = photon::net::new_iouring_tcp_client();
    if (cli == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "fail to create client");
    }
    DEFER(delete cli);

    auto run_ping_pong_worker = [&]() -> int {
        char buf[FLAGS_buf_size];

        auto conn = cli->connect(ep);
        if (conn == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "fail to connect")
        }
        DEFER(delete conn);

        while (!stop_test) {
            auto start = std::chrono::system_clock::now();
            //  write equals to fully send
            ssize_t ret = conn->write(buf, FLAGS_buf_size);
            if (ret != (ssize_t) FLAGS_buf_size) {
                LOG_ERRNO_RETURN(0, -1, "write fail");
            }
            // read equals to fully recv
            ret = conn->read(buf, FLAGS_buf_size);
            if (ret != (ssize_t) FLAGS_buf_size) {
                LOG_ERRNO_RETURN(0, -1, "read fail");
            }
            auto end = std::chrono::system_clock::now();
            time_cost += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            qps++;
        }
        return 0;
    };

    photon::thread_create11(run_latency_loop);

    for (size_t i = 0; i < FLAGS_client_connection_num; i++) {
        photon::thread_create11(run_ping_pong_worker);
    }

    // Forever sleep until Ctrl + C
    photon::thread_sleep(-1);
    return 0;
}

// Create two coroutines, one for sending and the other for receiving
static int streaming_client() {
    photon::net::EndPoint ep{photon::net::IPAddr(FLAGS_ip.c_str()), (uint16_t) FLAGS_port};
    auto cli = photon::net::new_tcp_socket_client();
    // auto cli = photon::net::new_iouring_tcp_client();
    if (cli == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "fail to create client");
    }
    DEFER(delete cli);

    auto conn = cli->connect(ep);
    if (conn == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "fail to connect")
    }
    DEFER(delete conn);

    auto send = [&]() -> int {
        char buf[FLAGS_buf_size];
        while (!stop_test) {
            ssize_t ret = conn->write(buf, FLAGS_buf_size);
            if (ret != (ssize_t) FLAGS_buf_size) {
                LOG_ERRNO_RETURN(0, -1, "write fail");
            }
        }
        return 0;
    };
    auto recv = [&]() -> int {
        char buf[FLAGS_buf_size];
        while (!stop_test) {
            ssize_t ret = conn->read(buf, FLAGS_buf_size);
            if (ret != (ssize_t) FLAGS_buf_size) {
                LOG_ERRNO_RETURN(0, -1, "read fail");
            }
        }
        return 0;
    };
    photon::thread_create11(recv);
    photon::thread_create11(send);

    // Forever sleep until Ctrl + C
    photon::thread_sleep(-1);
    return 0;
}

static int echo_server() {
    // Server has configured gracefully termination by signal processing
    photon::block_all_signal();
    photon::sync_signal(SIGTERM, &handle_signal);
    photon::sync_signal(SIGINT, &handle_signal);

    // Create a work pool if enabling multi-vcpu scheduling
    photon::WorkPool* work_pool = nullptr;
    if (FLAGS_vcpu_num > 1) {
        work_pool = new photon::WorkPool(FLAGS_vcpu_num, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    }
    DEFER(delete work_pool);

    // Create socket server
    auto server = photon::net::new_tcp_socket_server();
    // auto server = photon::net::new_iouring_tcp_server();
    if (server == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "fail to create server")
    }
    DEFER(delete server);

    auto stop_watcher = [&] {
        while (true) {
            photon::thread_sleep(1);
            if (stop_test) {
                LOG_INFO("terminate server");
                server->terminate();
                break;
            }
        }
    };

    // Define handler for new connections (SocketStream)
    auto handler = [&](photon::net::ISocketStream* sock) -> int {
        if (FLAGS_vcpu_num > 1) {
            work_pool->thread_migrate();
        }
        char buf[FLAGS_buf_size];
        while (true) {
            ssize_t ret1, ret2;
            ret1 = sock->recv(buf, FLAGS_buf_size);
            if (ret1 <= 0) {
                LOG_ERRNO_RETURN(0, -1, "read fail", VALUE(ret1));
            }
            ret2 = sock->write(buf, ret1);
            if (ret2 != ret1) {
                LOG_ERRNO_RETURN(0, -1, "write fail", VALUE(ret2));
            }
            photon::thread_yield();
            qps++;
        }
        return 0;
    };

    auto qps_th = photon::thread_create11(run_qps_loop);
    photon::thread_enable_join(qps_th);

    auto stop_th = photon::thread_create11(stop_watcher);
    photon::thread_enable_join(stop_th);

    server->set_handler(handler);
    server->bind(FLAGS_port, photon::net::IPAddr());
    server->listen();
    server->start_loop(true);

    photon::thread_join((photon::join_handle*) qps_th);
    photon::thread_join((photon::join_handle*) stop_th);
    return 0;
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    set_log_output_level(ALOG_INFO);

    // Note Photon's default event engine will first try io_uring, then choose epoll if io_uring failed.
    // Running an io_uring program would need the kernel version to be greater than 5.8.
    // We encourage you to upgrade to the latest kernel so that you could enjoy the extraordinary performance.
    int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    if (ret < 0) {
        LOG_ERROR_RETURN(0, -1, "failed to init photon environment");
    }
    DEFER(photon::fini());

    if (FLAGS_client) {
        if (FLAGS_client_mode == "streaming") {
            streaming_client();
        } else if (FLAGS_client_mode == "ping-pong") {
            ping_pong_client();
        } else {
            LOG_ERROR_RETURN(0, -1, "unknown client mode");
        }
    } else {
        echo_server();
    }
}
