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

#include <sys/fcntl.h>
#include <netinet/tcp.h>
#include <chrono>
#include <gflags/gflags.h>
#include <photon/thread/thread11.h>
#include <photon/io/signal.h>
#include <photon/fs/localfs.h>
#include <photon/common/alog-stdstring.h>
#include <photon/io/fd-events.h>
#include <photon/net/http/server.h>

using namespace photon;

DEFINE_int32(port, 19876, "port");
DEFINE_int32(body_size, 4096, "http body size");
DEFINE_bool(serve_file, false, "serve static file");

static bool stop_flag = false;
static std::string data_str;
static uint64_t qps = 0;
static const char* file_name = "static-file.test";

static void stop_handler(int signal) { stop_flag = true; }

static void show_qps_loop() {
    while (!stop_flag) {
        photon::thread_sleep(1);
        LOG_INFO("qps: `", qps);
        qps = 0;
    }
}

class SimpleHandler : public net::http::HTTPHandler {
public:
    int handle_request(net::http::Request& req, net::http::Response& resp, std::string_view) {
        auto target = req.target();
        resp.set_result(200);
        resp.headers.content_length((size_t) FLAGS_body_size);
        resp.headers.insert("Server", "nginx/1.14.1");
        resp.headers.insert("Content-Type", "application/octet-stream");
        auto ret_w = resp.write((void*) data_str.data(), FLAGS_body_size);
        if (ret_w != (ssize_t) FLAGS_body_size) {
            LOG_ERRNO_RETURN(0, -1,
                             "send body failed, target: `, `", target, VALUE(ret_w));
        }
        qps++;
        return 0;
    }
};

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    photon::fd_events_init();
    DEFER(photon::fd_events_fini());
    set_log_output_level(ALOG_INFO);
    if (photon::sync_signal_init() < 0) {
        LOG_ERROR("photon::sync_signal_init failed");
        exit(EAGAIN);
    }
    DEFER(photon::sync_signal_fini());

    photon::block_all_signal();
    photon::sync_signal(SIGINT, &stop_handler);
    photon::sync_signal(SIGTERM, &stop_handler);
    photon::sync_signal(SIGTSTP, &stop_handler);
    data_str.resize(FLAGS_body_size);
    for (auto& c : data_str) c = '0';

    thread_create11(show_qps_loop);
    auto tcpserv = net::new_tcp_socket_server();
    tcpserv->bind(FLAGS_port);
    tcpserv->listen();
    DEFER(delete tcpserv);
    auto http_srv = net::http::new_http_server();
    DEFER(delete http_srv);
    SimpleHandler handler;
    auto fs = fs::new_localfs_adaptor(".");
    if (!fs) {
        LOG_ERRNO_RETURN(0, -1, "error fs");
    }
    DEFER(delete fs);
    auto file = fs->open(file_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!file) {
        LOG_ERRNO_RETURN(0, -1, "error file");
    }
    DEFER(delete file);
    for (int i = 0; i < FLAGS_body_size; ++i) {
        file->write("1", 1);
    }
    auto fs_handler = net::http::new_fs_handler(fs);
    DEFER(delete fs_handler);
    if (FLAGS_serve_file) {
        http_srv->add_handler(fs_handler);
    } else {
        http_srv->add_handler(&handler);
    }
    tcpserv->set_handler(http_srv->get_connection_handler());
    tcpserv->start_loop();
    while (!stop_flag) {
        photon::thread_sleep(1);
    }
    LOG_INFO("test stopped");
}
