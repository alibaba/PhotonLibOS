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

#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/photon.h>
#include <photon/thread/workerpool.h>
#include <photon/thread/thread11.h>
#include <sys/fcntl.h>

#include <thread>
#include <vector>


static constexpr size_t pool_size = 4;
static constexpr size_t data_size = 4UL * 1024 * 1024 * 1024;
static constexpr size_t concurrent = 64;
static constexpr size_t bs = 4UL * 1024;
static constexpr size_t turn = data_size / bs / concurrent;

photon::WorkPool *pool;

void* task(void* arg) {
    auto fs =
        photon::fs::new_localfs_adaptor(nullptr, photon::fs::ioengine_psync);
    auto buffer = aligned_alloc(4096, bs);
    DEFER(free(buffer));
    DEFER(delete fs);
    auto file = fs->open("/tmp/workpool_perf", O_RDONLY);
    DEFER(delete file);
    for (auto i = 0U; i < turn; i++) {
        pool->call([i, buffer, file, arg] {
            auto ret =
                file->pread(buffer, bs, i * bs + ((uint64_t)arg) * bs * turn);
            if (ret < 0) LOG_ERROR("Failed to read");
        });
    }
    return nullptr;
}

void* norm(void* arg) {
    auto fs =
        photon::fs::new_localfs_adaptor(nullptr, photon::fs::ioengine_libaio);
    DEFER(delete fs);
    auto buffer = aligned_alloc(4096, bs);
    DEFER(free(buffer));
    auto file = fs->open("/tmp/workpool_perf", O_RDONLY | O_DIRECT);
    DEFER(delete file);
    for (auto i = 0U; i < turn; i++) {
        auto ret =
            file->pread(buffer, bs, i * bs + ((uint64_t)arg) * bs * turn);
        if (ret < 0) LOG_ERROR("Failed to read");
    }
    return nullptr;
}

int main() {
    // system(
    //     "dd if=/dev/zero of=/tmp/workpool_perf bs=1G count=40; sysctl "
    //     "vm.drop_caches=3");
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_LIBAIO);
    DEFER(photon::fini());
    pool = new photon::WorkPool(pool_size, photon::INIT_EVENT_EPOLL, 64);
    DEFER(delete pool);
    std::vector<photon::join_handle*> jhs;
    auto start = photon::now;
    for (uint64_t i = 0; i < concurrent; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create(task, (void*)i)));
    }
    for (auto& x : jhs) {
        photon::thread_join(x);
    }
    auto end = photon::now;
    LOG_INFO("Read from zero `GB in ` paralles work in ` threads ",
             data_size / 1024 / 1024 / 1024, concurrent, pool_size,
             VALUE(end - start));
    jhs.clear();
    start = photon::now;
    for (uint64_t i = 0; i < concurrent; i++) {
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create(norm, (void*)i)));
    }
    for (auto& x : jhs) {
        photon::thread_join(x);
    }
    end = photon::now;
    LOG_INFO("Read from zero `GB in ` paralles work in 1 thread with libaio",
             data_size / 1024 / 1024 / 1024, concurrent, VALUE(end - start));
    return 0;
}