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

#include "../socket.h"
#include "../tlssocket.h"
#include <photon/thread/thread.h>
#include <photon/io/fd-events.h>
#include <photon/common/alog.h>

using namespace photon;

int main(int argc, char** argv) {
    photon::thread_init();
    photon::fd_events_init();
    net::ssl_init("net/test/cert.pem", "net/test/key.pem", "Just4Test");
    DEFER({
        net::ssl_fini();
        photon::fd_events_fini();
        photon::thread_fini();
    });
    auto server = net::new_tls_socket_server();
    DEFER(delete server);

    auto logHandle = [&](net::ISocketStream* arg) {
        auto sock = (net::ISocketStream*) arg;
        char buff[4096];
        uint64_t recv_cnt = 0;
        ssize_t len = 0;
        uint64_t launchtime = photon::now;
        while ((len = sock->read(buff, 4096)) > 0) {
            recv_cnt += len;
        }
        LOG_INFO("Received ` bytes in ` seconds, throughput: `",
                 recv_cnt,
                 (photon::now - launchtime) / 1e6,
                 recv_cnt / ((photon::now - launchtime) / 1e6));
        return 0;
    };
    server->set_handler(logHandle);
    server->bind(31526, net::IPAddr());
    server->listen(1024);
    server->start_loop(true);
}
