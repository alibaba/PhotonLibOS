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

#include <atomic>
#include <gflags/gflags.h>

#include <photon/photon.h>
#include <photon/io/signal.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>
#include <photon/common/alog.h>
#include <photon/net/socket.h>

DEFINE_uint64(show_statistics_interval, 1, "interval seconds to show statistics");
DEFINE_bool(client, false, "client or server? default is server");
DEFINE_uint64(client_connection_num, 8, "io depth per connection");
DEFINE_uint64(client_thread_num, 8, "client std thread number");
DEFINE_string(ip, "127.0.0.1", "ip");
DEFINE_uint64(port, 9527, "port");
DEFINE_uint64(buf_size, 512, "buffer size");

static int event_engine = 0;
static bool stop_test = false;
static std::atomic<uint64_t> qps{0};
static std::atomic<uint64_t> time_cost{0};

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

static int ping_pong_client() {
    photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE);

    auto cli = photon::net::new_tcp_socket_client();
    photon::net::EndPoint ep(photon::net::IPAddr(FLAGS_ip.c_str()), (uint16_t) FLAGS_port);
    LOG_INFO("Connect to ", ep);
    auto conn = cli->connect(ep);

    auto run_ping_pong_worker = [&]() -> int {
        char buf[FLAGS_buf_size];
        while (!stop_test) {
            auto start = std::chrono::system_clock::now();
            ssize_t ret = conn->write(buf, FLAGS_buf_size);
            if (ret != (ssize_t) FLAGS_buf_size) {
                LOG_ERROR("fail to write");
            }
            ret = conn->read(buf, FLAGS_buf_size);
            if (ret != (ssize_t) FLAGS_buf_size) {
                LOG_ERROR("fail to read");
            }
            auto end = std::chrono::system_clock::now();
            time_cost += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            qps++;
        }
        return 0;
    };

    for (size_t i = 0; i < FLAGS_client_connection_num; i++) {
        photon::thread_create11(run_ping_pong_worker);
    }

    photon::thread_sleep(-1);
    return 0;
}


static int echo_server() {
    photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE);
    auto server = photon::net::new_tcp_socket_server();

    auto handler = [&](photon::net::ISocketStream* sock) -> int {
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

    server->set_handler(handler);
    server->bind(FLAGS_port);
    server->listen();
    server->start_loop(true);
    return 0;
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    set_log_output_level(ALOG_INFO);

    int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    if (ret < 0) {
        LOG_ERROR_RETURN(0, -1, "failed to init photon environment");
    }
    DEFER(photon::fini());

    if (FLAGS_client) {
        photon::thread_create11(run_latency_loop);
        for (int i = 0; i < FLAGS_client_thread_num; ++i) {
            new std::thread(ping_pong_client);
        }
    } else {
        photon::thread_create11(run_qps_loop);
        new std::thread(echo_server);
    }
    photon::thread_sleep(-1);
}
