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

#include <string>
#include <unistd.h>
#include <cerrno>
#include <atomic>
#include <random>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <gflags/gflags.h>

#include <photon/io/fd-events.h>
#include <photon/thread/thread11.h>
#include <photon/net/socket.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/utility.h>
#include <photon/common/io-alloc.h>
#include <photon/rpc/rpc.h>
#include <photon/common/checksum/crc32c.h>
#include "zerocopy-common.h"

using namespace std;
using namespace photon;

DEFINE_int32(io_type, 0, "0: read, 1: write");

uint8_t** g_read_buffers = nullptr;
uint64_t g_rpc_count = 0;
uint64_t g_rpc_time_cost = 0;

void run_write_rpc(rpc::StubPool* pool, const net::EndPoint& ep) {
    rpc::Stub* stub = pool->get_stub(ep, false);
    if (stub == nullptr) {
        LOG_ERROR("cannot get stub");
        exit(1);
    }
    DEFER(pool->put_stub(ep, false));

    void* send_buf = malloc(FLAGS_buf_size);
    DEFER(free(send_buf));

    while (true) {
        timeval start = {}, end = {};
        gettimeofday(&start, nullptr);

        TestWriteProto::Request req;
        req.buf.assign(send_buf, FLAGS_buf_size);
        TestWriteProto::Response resp;

        int ret = stub->call<TestWriteProto>(req, resp);
        if (ret < 0) {
            LOG_ERROR("fail to call RPC");
            exit(-1);
        }

        gettimeofday(&end, nullptr);
        g_rpc_count++;
        g_rpc_time_cost += (end.tv_sec * 1000000 + end.tv_usec - start.tv_sec * 1000000 - start.tv_usec);
    }
}

void run_read_rpc(int index, rpc::StubPool* pool, const net::EndPoint& ep) {
    rpc::Stub* stub = pool->get_stub(ep, false);
    if (stub == nullptr) {
        LOG_ERROR("cannot get stub");
        exit(1);
    }
    DEFER(pool->put_stub(ep, false));

    while (true) {
        timeval start = {}, end = {};
        gettimeofday(&start, nullptr);

        size_t size = FLAGS_calculate_checksum ? FLAGS_buf_size + checksum_padding_size : FLAGS_buf_size;
        IOVector iovector;
        iovector.push_back(g_read_buffers[index], size);

        TestReadProto::Request req;
        req.file_index = index;
        TestReadProto::Response resp;
        resp.buf.assign(iovector.iovec(), iovector.iovcnt());
        int ret = stub->call<TestReadProto>(req, resp);
        if (ret < 0) {
            LOG_ERROR("fail to call RPC");
            exit(-1);
        }

        gettimeofday(&end, nullptr);
        g_rpc_count++;
        g_rpc_time_cost += (end.tv_sec * 1000000 + end.tv_usec - start.tv_sec * 1000000 - start.tv_usec);

        if (FLAGS_calculate_checksum) {
            uint32_t crc32_calculated = crc32c_extend(g_read_buffers[index], FLAGS_buf_size, 0);
            uint32_t crc32_received = *(uint32_t*) (g_read_buffers[index] + FLAGS_buf_size);
            if (crc32_calculated != crc32_received) {
                LOG_ERROR("checksum error: ` != `", crc32_calculated, crc32_received);
                exit(-1);
            }
        }
    }
}

void prepare_read_buffers() {
    size_t size = FLAGS_calculate_checksum ? FLAGS_buf_size + checksum_padding_size : FLAGS_buf_size;
    for (size_t i = 0; i < FLAGS_num_threads; i++) {
        void* buf = nullptr;
        int ret = posix_memalign(&buf, 4096, size);
        if (ret != 0) {
            LOG_FATAL("posix_memalign failed: error `", ERRNO());
            exit(-1);
        }
        g_read_buffers[i] = (uint8_t*) buf;
    }
}

void show_performance_statis() {
    while (true) {
        photon::thread_sleep(10);
        uint64_t lat = (g_rpc_count != 0) ? (g_rpc_time_cost / g_rpc_count) : 0;
        LOG_INFO("rpc latency ` us", lat);
        g_rpc_time_cost = g_rpc_count = 0;
    }
}

int main(int argc, char** argv) {
    set_log_output_level(ALOG_INFO);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());

    g_read_buffers = new uint8_t* [FLAGS_num_threads];   // 共 num_threads 个 read buffer
    DEFER(delete[] g_read_buffers);
    prepare_read_buffers();

    auto pool = rpc::new_stub_pool(60 * 1000 * 1000, 10 * 1000 * 1000, -1);
    DEFER(delete pool);

    photon::thread_create11(show_performance_statis);

    net::EndPoint ep{net::IPAddr(FLAGS_ip.c_str()), (uint16_t) FLAGS_port};

    for (size_t i = 0; i < FLAGS_num_threads; i++) {
        if (IOType(FLAGS_io_type) == IOType::READ) {
            photon::thread_create11(run_read_rpc, i, pool, ep);
        } else if (IOType(FLAGS_io_type) == IOType::WRITE) {
            photon::thread_create11(run_write_rpc, pool, ep);
        }
    }

    photon::thread_sleep(-1);
}
