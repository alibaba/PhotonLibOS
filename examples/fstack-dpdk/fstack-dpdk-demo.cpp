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
#include <photon/photon.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/net/socket.h>

static const size_t buf_size = 512;
static const uint16_t port = 80;

int main() {
    int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_FSTACK_DPDK);
    if (ret < 0) {
        LOG_ERROR_RETURN(0, -1, "failed to init photon environment");
    }
    DEFER(photon::fini());

    // DPDK will start running from here, as an I/O engine.
    // Although busy polling, the scheduler will ensure that all threads in current vcpu
    // have a chance to run, when their events arrived.

    photon::thread_create11([] {
        auto file = photon::fs::open_localfile_adaptor("test_file", O_CREAT | O_TRUNC | O_WRONLY, 0644);
        static const char* str = "x ";
        while (true) {
            // Write some bytes to a file, every 1 second
            photon::thread_sleep(1);
            LOG_INFO("write once");
            file->write(str, strlen(str));
            file->fdatasync();
        }
    });

    auto handler = [&](photon::net::ISocketStream* sock) -> int {
        char buf[buf_size];
        while (true) {
            ssize_t ret1, ret2;
            ret1 = sock->recv(buf, buf_size);
            if (ret1 <= 0) {
                LOG_ERRNO_RETURN(0, -1, "read fail", VALUE(ret1));
            }
            ret2 = sock->write(buf, ret1);
            if (ret2 != ret1) {
                LOG_ERRNO_RETURN(0, -1, "write fail", VALUE(ret2));
            }
        }
        return 0;
    };

    auto server = photon::net::new_fstack_dpdk_socket_server();
    server->set_handler(handler);
    server->bind(port, photon::net::IPAddr());
    server->listen();
    server->start_loop(false);

    photon::thread_sleep(-1UL);
}
