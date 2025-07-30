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
#ifdef PHOTON_ENABLE_RSOCKET
#include <photon/net/rsocket/rsocket.h>
#endif

DEFINE_uint64(show_statistics_interval, 1, "interval seconds to show statistics");
DEFINE_bool(client, false, "client or server? default is server");
DEFINE_string(client_mode, "pingpong", "client mode. Choose between streaming or pingpong");
DEFINE_uint64(client_connection_num, 100, "number of the connections of the client (shared by all vCPUs), only available in pingpong mode");
DEFINE_string(ip, "127.0.0.1", "ip");
DEFINE_uint64(port, 9527, "port");
DEFINE_uint64(buf_size, 512, "buffer size");
DEFINE_uint64(vcpu_num, 1, "vCPU number for both client and server");
DEFINE_string(socket_type, "tcp", "Support tcp/rsocket/io_uring");

static bool stop_test = false;
static std::atomic<uint64_t> qps = {};
static std::atomic<uint64_t> time_cost = {};

// Use WorkPool to enable multi vCPU for client
static photon::WorkPool* work_pool = nullptr;
static int event_engine = photon::INIT_EVENT_DEFAULT;

static void handle_signal(int sig) {
    LOG_INFO("Try to gracefully stop test ...");
    stop_test = true;
}

static void run_statistics_loop(bool show_latency) {
    while (!stop_test) {
        photon::thread_sleep(FLAGS_show_statistics_interval);
        uint64_t qps_val = qps.load();
        if (show_latency) {
            uint64_t lat = (qps_val != 0) ? (time_cost.load() / qps_val) : 0;
            LOG_INFO("qps: `, bw: ` MB/s, latency: ` us", qps_val / FLAGS_show_statistics_interval,
                     ((qps_val * FLAGS_buf_size) >> 20UL) / FLAGS_show_statistics_interval, lat);
        } else {
            LOG_INFO("qps: `, bw: ` MB/s", qps_val / FLAGS_show_statistics_interval,
                     ((qps_val * FLAGS_buf_size) >> 20UL) / FLAGS_show_statistics_interval);
        }
        qps = 0;
        time_cost = 0;
    }
}

// Create coroutines for each of the connections, doing pingpong send/recv
static int ping_pong_client() {
    if (FLAGS_vcpu_num > 1) {
        work_pool = new photon::WorkPool(FLAGS_vcpu_num, event_engine, photon::INIT_IO_NONE);
    }
    DEFER(delete work_pool);

    auto run_ping_pong_worker = [&](size_t conn_index) -> int {
        size_t vcpu_index = conn_index % FLAGS_vcpu_num;
        if (work_pool) {
            work_pool->thread_migrate(photon::CURRENT, vcpu_index);
        }
        // After the above call, this function begins to run in the specific vCPU

        photon::net::EndPoint ep(FLAGS_ip.c_str(), uint16_t(FLAGS_port + vcpu_index));
        photon::net::ISocketClient* cli;
        if (FLAGS_socket_type == "rsocket") {
#ifdef PHOTON_ENABLE_RSOCKET
            cli = photon::net::new_rsocket_client();
#else
            cli = nullptr;
#endif
        } else if (FLAGS_socket_type == "io_uring") {
#ifdef PHOTON_URING
            cli = photon::net::new_iouring_tcp_client();
#else
            cli = nullptr;
#endif
        } else {
            cli = photon::net::new_tcp_socket_client();
        }
        if (cli == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "fail to create client");
        }
        DEFER(delete cli);

        auto buf = malloc(FLAGS_buf_size);
        DEFER(free(buf));

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

    photon::thread_create11(run_statistics_loop, true);

    for (size_t i = 0; i < FLAGS_client_connection_num; i++) {
        photon::thread_create11(run_ping_pong_worker, i);
    }

    // Forever sleep until Ctrl + C
    photon::thread_sleep(-1);
    return 0;
}

// Create two coroutines, one for sending and the other for receiving
static int streaming_client() {
    photon::net::EndPoint ep{photon::net::IPAddr(FLAGS_ip.c_str()), (uint16_t) FLAGS_port};
    photon::net::ISocketClient* cli;
    if (FLAGS_socket_type == "rsocket") {
#ifdef PHOTON_ENABLE_RSOCKET
        cli = photon::net::new_rsocket_client();
#else
        cli = nullptr;
#endif
    } else if (FLAGS_socket_type == "io_uring") {
#ifdef PHOTON_URING
        cli = photon::net::new_iouring_tcp_client();
#else
        cli = nullptr;
#endif
    } else {
        cli = photon::net::new_tcp_socket_client();
    }
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
        auto buf = malloc(FLAGS_buf_size);
        DEFER(free(buf));
        while (!stop_test) {
            ssize_t ret = conn->write(buf, FLAGS_buf_size);
            if (ret != (ssize_t) FLAGS_buf_size) {
                LOG_ERRNO_RETURN(0, -1, "write fail");
            }
        }
        return 0;
    };
    auto recv = [&]() -> int {
        auto buf = malloc(FLAGS_buf_size);
        DEFER(free(buf));
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

    // Define handler for new connections.
    auto handler = [&](photon::net::ISocketStream* sock) -> int {
        auto buf = malloc(FLAGS_buf_size);
        DEFER(free(buf));
        while (!stop_test) {
            ssize_t ret1, ret2;
            ret1 = sock->read(buf, FLAGS_buf_size);
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

    std::vector<std::thread> vcpu_arr(FLAGS_vcpu_num);
    std::vector<photon::net::ISocketServer*> socket_server_arr(FLAGS_vcpu_num);

    // Create multiple vCPUs for server. Each of them listens to an individual port
    for (size_t i = 0; i < FLAGS_vcpu_num; ++i) {
        vcpu_arr[i] = std::thread([&, i]{
            photon::init(event_engine, photon::INIT_IO_NONE);
            DEFER(photon::fini());

            // Create socket server
            photon::net::ISocketServer* server;
            if (FLAGS_socket_type == "rsocket") {
#ifdef PHOTON_ENABLE_RSOCKET
                server = photon::net::new_rsocket_server();
#else
                server = nullptr;
#endif
            } else if (FLAGS_socket_type == "io_uring") {
#ifdef PHOTON_URING
                server = photon::net::new_iouring_tcp_server();
#else
                server = nullptr;
#endif
            } else {
                server = photon::net::new_tcp_socket_server();
            }
            if (server == nullptr) {
                LOG_ERRNO_RETURN(0, -1, "fail to create server")
            }
            DEFER(delete server);
            socket_server_arr[i] = server;

            server->set_handler(handler);
            server->setsockopt<int>(SOL_SOCKET, SO_REUSEPORT, 1);
            server->bind_v4any(uint16_t(FLAGS_port + i));
            server->listen();
            server->start_loop(true);
            return 0;
        });
    }

    auto stop_watcher = [&] {
        while (true) {
            photon::thread_sleep(1);
            if (stop_test) {
                LOG_INFO("terminate server");
                for (auto server : socket_server_arr) {
                    server->terminate();
                }
                break;
            }
        }
    };

    auto statistics_th = photon::thread_create11(run_statistics_loop, false);
    photon::thread_enable_join(statistics_th);

    auto stop_th = photon::thread_create11(stop_watcher);
    photon::thread_enable_join(stop_th);

    photon::thread_join((photon::join_handle*) statistics_th);
    photon::thread_join((photon::join_handle*) stop_th);

    for (auto& vcpu_th : vcpu_arr) {
        vcpu_th.join();
    }
    return 0;
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    set_log_output_level(ALOG_INFO);

    // Note Photon's default event engine in Linux is epoll.
    // Running an io_uring program would need the kernel version to be greater than 5.8.
    // We encourage you to upgrade to the latest kernel so that you could enjoy the extraordinary performance.
    if (FLAGS_socket_type == "iouring") {
        event_engine = photon::INIT_EVENT_IOURING;
    }
    int ret = photon::init(event_engine, photon::INIT_IO_NONE);
    if (ret < 0) {
        LOG_ERROR_RETURN(0, -1, "failed to init photon environment");
    }
    DEFER(photon::fini());

    if (FLAGS_client) {
        if (FLAGS_client_mode == "streaming") {
            streaming_client();
        } else if (FLAGS_client_mode == "pingpong") {
            ping_pong_client();
        } else {
            LOG_ERROR_RETURN(0, -1, "unknown client mode");
        }
    } else {
        echo_server();
    }
}
