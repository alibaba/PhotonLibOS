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
#include <photon/thread/thread.h>
#include <photon/io/fd-events.h>
#include <photon/net/security-context/tls-stream.h>
#include <photon/common/alog.h>

#include "cert-key.cpp"

using namespace photon;

int main(int argc, char** argv) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());

    auto ctx = net::new_tls_context(cert_str, key_str, passphrase_str);
    if (!ctx) return -1;
    DEFER(delete ctx);
    auto server = net::new_tls_server(ctx, net::new_tcp_socket_server(), true);
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
