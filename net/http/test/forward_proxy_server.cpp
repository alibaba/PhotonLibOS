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
#include <photon/io/signal.h>
#include <photon/thread/thread11.h>
#include <photon/fs/localfs.h>
#include <photon/photon.h>
#include <photon/common/alog-stdstring.h>
#include <photon/net/http/server.h>
#include <sys/prctl.h>
#include <malloc.h>

using namespace photon;
using namespace photon::net;
using namespace photon::net::http;

DEFINE_int32(port, 19876, "port");

static bool stop_flag = false;

static void stop_handler(int signal) { stop_flag = true; }

int main(int argc, char** argv) {
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    prctl(PR_SET_THP_DISABLE, 1);

    gflags::ParseCommandLineFlags(&argc, &argv, true);
    photon::init(INIT_EVENT_DEFAULT, INIT_IO_DEFAULT);
    DEFER(photon::fini());
    set_log_output_level(ALOG_INFO);

    photon::block_all_signal();
    photon::sync_signal(SIGINT, &stop_handler);
    photon::sync_signal(SIGTERM, &stop_handler);
    photon::sync_signal(SIGTSTP, &stop_handler);

    auto tcpserv = new_tcp_socket_server();
    tcpserv->setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
    tcpserv->bind(FLAGS_port);
    tcpserv->listen();
    DEFER(delete tcpserv);

    auto http_srv = new_http_server();
    DEFER(delete http_srv);
    http_srv->add_handler(new_default_forward_proxy_handler(30 * 1000 * 1000), true);
    tcpserv->set_handler(http_srv->get_connection_handler());
    tcpserv->start_loop();
    while (!stop_flag) {
        photon::thread_sleep(1);
    }
    LOG_INFO("server stopped");
}
