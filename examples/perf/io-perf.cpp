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
#include <thread>
#include <chrono>
#include <gflags/gflags.h>

#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/io-alloc.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>

const static size_t LAST_IO_BOUNDARY = 2 * 1024 * 1024;
static std::atomic<uint64_t> qps{0};

DEFINE_uint64(io_depth, 128, "io depth");
DEFINE_string(disk_path, "", "disk path. For example, /dev/nvme2n1");
DEFINE_uint64(disk_size, 0, "disk size. For example, 1000000000000. No need to align. Can be approximate number");
DEFINE_uint64(io_size, 4096, "io size");
DEFINE_bool(io_uring, false, "test io_uring or aio");
DEFINE_bool(use_workpool, false, "dispatch read tasks to multi vCPU by using workpool");
DEFINE_uint64(vcpu_num, 4, "vCPU num of the workpool");

#define ROUND_DOWN(N, S) ((N) & ~((S) - 1))

static uint64_t random(uint64_t N) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    return gen() % N;
}

static void show_qps_loop() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        LOG_INFO("QPS: `, BW: ` MB/s", qps.load(), qps.load() * FLAGS_io_size / 1024 / 1024);
        qps.store(0, std::memory_order_relaxed);
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
        qps.fetch_add(1, std::memory_order_relaxed);
    }
}

static void infinite_read_by_work_pool(const uint64_t max_offset, photon::fs::IFile* src_file,
                                       IOAlloc* alloc, photon::WorkPool* work_pool) {
    size_t count = FLAGS_io_size;
    void* buf = alloc->alloc(count);
    while (true) {
        photon::semaphore sem;
        work_pool->async_call(new auto([&] {
            uint64_t offset = ROUND_DOWN(random(max_offset), count);
            int ret = src_file->pread(buf, FLAGS_io_size, offset);
            if (ret != (int) count) {
                LOG_ERROR("read fail, count `, offset `, ret `, errno `", count, offset, ret, ERRNO());
                exit(1);
            }
            qps.fetch_add(1, std::memory_order_relaxed);
            sem.signal(1);
        }));
        sem.wait(1);
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

    photon::PhotonOptions opt;
    opt.use_pooled_stack_allocator = true;
    opt.libaio_queue_depth = 512;
    int ret = photon::init(ev_engine, io_engine, opt);
    if (ret != 0) {
        LOG_ERROR_RETURN(0, -1, "init failed");
    }

    new std::thread(show_qps_loop);

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

    photon::WorkPool* work_pool = nullptr;
    if (FLAGS_use_workpool) {
        work_pool = new photon::WorkPool(FLAGS_vcpu_num, ev_engine, io_engine, 0);
        for (uint64_t i = 0; i < FLAGS_io_depth; i++) {
            photon::thread_create11(infinite_read_by_work_pool, max_offset, file, &io_alloc, work_pool);
        }
    } else {
        for (uint64_t i = 0; i < FLAGS_io_depth; i++) {
            photon::thread_create11(infinite_read, max_offset, file, &io_alloc);
        }
    }

    photon::thread_sleep(-1);
}