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
#include <sys/statfs.h>

#include <vector>
#include <random>
#include <gflags/gflags.h>

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/io-alloc.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread11.h>

const static size_t LAST_IO_BOUNDARY = 2 * 1024 * 1024;
static std::random_device rd;
static std::mt19937 gen(rd());
static uint64_t qps = 0;

DEFINE_uint64(io_depth, 128, "io depth");
DEFINE_string(disk_path, "", "disk path. For example, /dev/nvme2n1");
DEFINE_uint64(disk_size, 0, "disk size. For example, 1000000000000. No need to align. Can be approximate number");
DEFINE_uint64(io_size, 4096, "io size");
DEFINE_bool(io_uring, false, "test io_uring or aio");

#define ROUND_DOWN(N, S) ((N) & ~((S) - 1))

static uint64_t random(uint64_t N) {
    std::uniform_int_distribution<size_t> distrib(0, N);
    return distrib(gen);
}

static void show_qps_loop() {
    while (true) {
        photon::thread_sleep(1);
        LOG_INFO("QPS: `, BW: ` MB/s", qps, qps * FLAGS_io_size / 1024 / 1024);
        qps = 0;
    }
}

static void infinite_read(const uint64_t max_offset, photon::fs::IFile* src_file, IOAlloc* alloc) {
    size_t count = FLAGS_io_size;
    void* buf = alloc->alloc(count);
    while (true) {
        uint64_t offset = ROUND_DOWN(random(max_offset), count);
        int ret = src_file->pread(buf, FLAGS_io_size, offset);
        if (ret != (int) count) {
            LOG_ERROR("read fail, count `, offset `, ret `, errno `", count, offset, ret, ERRNO());
            exit(1);
        }
        qps++;
    }
}

int main(int argc, char** arg) {
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    log_output_level = ALOG_INFO;
    if (FLAGS_disk_path.empty()) {
        LOG_ERROR_RETURN(0, -1, "need disk path");
    }
    if (FLAGS_disk_size < 10'000'000'000UL) {
        LOG_ERROR_RETURN(0, -1, "need disk size");
    }
    LOG_INFO("Specify disk ` size `", FLAGS_disk_path.c_str(), FLAGS_disk_size);

    int ev_engine = FLAGS_io_uring ? photon::INIT_EVENT_IOURING : photon::INIT_EVENT_EPOLL;
    int io_engine = FLAGS_io_uring ? photon::INIT_IO_NONE : photon::INIT_IO_LIBAIO;
    int fs_io_engine = FLAGS_io_uring ? photon::fs::ioengine_iouring : photon::fs::ioengine_libaio;

    int ret = photon::init(ev_engine, io_engine, photon::PhotonOptions{.libaio_queue_depth = 512});
    if (ret != 0) {
        LOG_ERROR_RETURN(0, -1, "init failed");
    }

    photon::thread_create11(show_qps_loop);

    // Read only open with direct-IO
    int flags = O_RDONLY | O_DIRECT;
    auto file = photon::fs::open_localfile_adaptor(FLAGS_disk_path.c_str(), flags, 0644, fs_io_engine);
    if (!file) {
        LOG_ERROR_RETURN(0, -1, "open failed");
    }

    // The allocator is only for aio 4K mem align
    // io_uring doesn't require mem align
    AlignedAlloc io_alloc(4096);
    uint64_t max_offset = FLAGS_disk_size - LAST_IO_BOUNDARY;

    for (uint64_t i = 0; i < FLAGS_io_depth; i++) {
        photon::thread_create11(infinite_read, max_offset, file, &io_alloc);
    }
    photon::thread_sleep(-1);
}