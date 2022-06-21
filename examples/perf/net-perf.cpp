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
#include <photon/io/signalfd.h>
#include <photon/common/io-alloc.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/net/socket.h>

// Common parameters
bool stop_test = false;
uint64_t qps = 0;
uint64_t time_cost = 0;
DEFINE_uint64(show_statistics_interval, 1, "interval seconds to show statistics");
DEFINE_uint64(num_threads, 32, "num of threads");

// Network parameters
DEFINE_bool(client, false, "client or server? default is server");
DEFINE_string(ip, "127.0.0.1", "ip");
DEFINE_uint64(port, 9527, "port");
DEFINE_uint64(buf_size, 4096, "buffer size");

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

static int echo_client() {
    photon::net::EndPoint ep{photon::net::IPAddr(FLAGS_ip.c_str()), (uint16_t) FLAGS_port};
    auto cli = photon::net::new_tcp_socket_client();
    if (cli == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "fail to create client")
    }
    auto conn = cli->connect(ep);
    if (conn == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "fail to connect")
    }

    auto run_echo_worker = [&]() -> int {
        AlignedAlloc alloc(512);
        void* buf = alloc.alloc(FLAGS_buf_size);
        DEFER(alloc.dealloc(buf));

        while (!stop_test) {
            auto start = std::chrono::system_clock::now();
            ssize_t ret;
            ret = conn->write(buf, FLAGS_buf_size);
            if (ret != (ssize_t) FLAGS_buf_size) {
                LOG_ERRNO_RETURN(0, -1, "write fail");
            }
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

    for (size_t i = 0; i < FLAGS_num_threads; i++) {
        photon::thread_create11(&decltype(run_echo_worker)::operator(), &run_echo_worker);
    }

    // Forever sleep until Ctrl + C
    photon::thread_sleep(-1);
    return 0;
}

static int echo_server() {
    // Server has configured gracefully termination by signal processing
    photon::sync_signal(SIGTERM, &handle_signal);
    photon::sync_signal(SIGINT, &handle_signal);

    auto server = photon::net::new_tcp_socket_server();
    if (server == nullptr) {
        LOG_ERRNO_RETURN(0, -1, "fail to create server")
    }

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

    auto handle = [&](photon::net::ISocketStream* arg) -> int {
        LOG_TEMP("Connection established");
        DEFER(LOG_TEMP("Connection shutdown"));
        auto sock = (photon::net::ISocketStream*) arg;

        AlignedAlloc alloc(512);
        void* buf = alloc.alloc(FLAGS_buf_size);
        DEFER(alloc.dealloc(buf));

        while (true) {
            ssize_t ret1, ret2;
            ret1 = sock->recv(buf, FLAGS_buf_size);
            if (ret1 <= 0) {
                LOG_ERRNO_RETURN(0, -1, "read fail");
            }
            // write = fully send
            ret2 = sock->write(buf, ret1);
            if (ret2 != ret1) {
                LOG_ERRNO_RETURN(0, -1, "write fail");
            }
            qps++;
        }
        return 0;
    };

    auto qps_th = photon::thread_create11(run_qps_loop);
    photon::thread_enable_join(qps_th);

    auto stop_th = photon::thread_create11(&decltype(stop_watcher)::operator(), &stop_watcher);
    photon::thread_enable_join(stop_th);

    server->set_handler(handle);
    server->bind(FLAGS_port, photon::net::IPAddr());
    server->listen(1024);
    server->start_loop(true);

    photon::thread_join((photon::join_handle*) qps_th);
    photon::thread_join((photon::join_handle*) stop_th);
    return 0;
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    set_log_output_level(ALOG_INFO);

    // If you have trouble upgrading the kernel, please switch the event_engine argument
    // from `photon::INIT_EVENT_IOURING` to `photon::INIT_EVENT_EPOLL`.
    int ret = photon::init(photon::INIT_EVENT_IOURING | photon::INIT_EVENT_SIGNALFD, 0);
    if (ret < 0) {
        LOG_ERROR_RETURN(0, -1, "failed to init photon environment");
    }
    DEFER(photon::fini());

    if (FLAGS_client) {
        echo_client();
    } else {
        echo_server();
    }
}