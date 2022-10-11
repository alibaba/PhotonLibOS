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

#include <gflags/gflags.h>
#include <photon/common/alog-stdstring.h>
#include <photon/io/signal.h>
#include <photon/photon.h>

#include "client.h"

DEFINE_int32(port, 0, "server port");
DEFINE_string(host, "127.0.0.1", "server ip");

static bool running = true;
static photon::join_handle* heartbeat;
static photon::net::EndPoint ep;

void* heartbeat_thread(void* arg) {
    auto client = (ExampleClient*)arg;
    while (running) {
        auto start = photon::now;
        auto half = client->RPCHeartbeat(ep);
        auto end = photon::now;
        if (half > 0) {
            LOG_INFO("single trip `us, round trip `us", half - start,
                     end - start);
        } else {
            LOG_INFO("heartbeat failed in `us", end - start);
        }
        photon::thread_sleep(1);
    }
    return nullptr;
}

void run_some_task(ExampleClient* client) {
    auto echo = client->RPCEcho(ep, "Hello");
    LOG_INFO(VALUE(echo));

    IOVector iov;
    char writebuf[] = "write data like pwrite";
    iov.push_back(writebuf, sizeof(writebuf));
    auto tmpfile = "/tmp/test_file_" + std::to_string(rand());
    auto ret = client->RPCWrite(ep, tmpfile, iov.iovec(), iov.iovcnt());
    LOG_INFO("Write to tmpfile ` ret=`", tmpfile, ret);

    char readbuf[4096];
    struct iovec iovrd {
        .iov_base = readbuf, .iov_len = 4096,
    };
    ret = client->RPCRead(ep, tmpfile, &iovrd, 1);
    LOG_INFO("Read from tmpfile ` ret=`", tmpfile, ret);
    LOG_INFO(VALUE((char*)readbuf));
    client->RPCTestrun(ep);
}

void handle_term(int) {
    running = false;
    photon::thread_interrupt((photon::thread*)heartbeat);
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    srand(time(NULL));
    photon::init();
    DEFER(photon::fini());

    photon::sync_signal(SIGTERM, &handle_term);
    photon::sync_signal(SIGINT, &handle_term);

    ExampleClient client;
    ep = photon::net::EndPoint(photon::net::IPAddr(FLAGS_host.c_str()),
                               FLAGS_port);
    heartbeat = photon::thread_enable_join(
        photon::thread_create(heartbeat_thread, &client));
    run_some_task(&client);
    photon::thread_join(heartbeat);

    return 0;
}