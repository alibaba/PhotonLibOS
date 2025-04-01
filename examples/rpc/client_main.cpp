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

#include <chrono>
#include <gflags/gflags.h>
#include <photon/common/alog-stdstring.h>
#include <photon/io/signal.h>
#include <photon/photon.h>
#include <photon/thread/thread11.h>
#include <photon/net/socket.h>

#include "client.h"

DEFINE_int32(port, 0, "server port");
DEFINE_string(host, "127.0.0.1", "server ip");
DEFINE_bool(echo_perf, false, "Instead of a standalone demo, run echo performance test and show statistics");
DEFINE_uint64(buf_size, 4096, "buffer size for echo perf test");
DEFINE_uint64(depth, 4, "io-depth for echo perf test");

static bool running = true;
static photon::join_handle* bg_thread;
static photon::net::EndPoint ep;
static uint64_t qps = 0;
static uint64_t time_cost = 0;

void show_statistics() {
    uint64_t lat = (qps != 0) ? (time_cost / qps) : 0;
    LOG_INFO("QPS: `, latency: ` us", qps, lat);
    qps = time_cost = 0;
}

void heartbeat(ExampleClient* client) {
    auto start = photon::now;
    auto ret = client->RPCHeartbeat(ep);
    auto end = photon::now;
    if (ret > 0) {
        LOG_INFO("heartbeat round trip `us", end - start);
    } else {
        LOG_INFO("heartbeat failed in `us", end - start);
    }
}

void* run_bg_thread(void* arg) {
    auto client = (ExampleClient*)arg;
    while (running) {
        if (FLAGS_echo_perf) {
            show_statistics();
        } else {
            heartbeat(client);
        }
        photon::thread_sleep(1);
    }
    return nullptr;
}

void run_some_task_and_stop(ExampleClient* client) {
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

    // Sleep 1s and stop
    photon::thread_sleep(1);
    running = false;
}

void run_echo_perf_forever(ExampleClient* client) {
    photon::semaphore sem;
    for (size_t i = 0; i < FLAGS_depth; i++) {
        photon::thread_create11([&] {
            AlignedAlloc alloc(512);
            void* send_buf = alloc.alloc(FLAGS_buf_size);
            void* recv_buf = alloc.alloc(FLAGS_buf_size);
            DEFER(alloc.dealloc(send_buf));
            DEFER(alloc.dealloc(recv_buf));

            while (running) {
                auto start = std::chrono::system_clock::now();

                client->RPCEchoPerf(ep, send_buf, recv_buf, FLAGS_buf_size);

                auto end = std::chrono::system_clock::now();
                time_cost += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                qps++;
            }
            sem.signal(1);
        });
    }
    sem.wait(FLAGS_depth);
}

void handle_term(int) {
    running = false;
    photon::thread_interrupt((photon::thread*)bg_thread);
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_log_output_level(ALOG_INFO);
    srand(time(NULL));
    photon::init();
    DEFER(photon::fini());

    photon::sync_signal(SIGTERM, &handle_term);
    photon::sync_signal(SIGINT, &handle_term);

    ExampleClient client;
    ep = photon::net::EndPoint(FLAGS_host.c_str(), FLAGS_port);

    bg_thread = photon::thread_enable_join(photon::thread_create(run_bg_thread, &client));

    if (FLAGS_echo_perf) {
        run_echo_perf_forever(&client);
    } else {
        run_some_task_and_stop(&client);
    }
    photon::thread_join(bg_thread);

    return 0;
}