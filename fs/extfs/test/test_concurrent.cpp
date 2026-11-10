/*
Copyright 2023 The Photon Authors

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
#include "../extfs.h"
#include <atomic>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/fs/localfs.h>
#include <photon/thread/std-compat.h>
#include "../../../test/gtest.h"

namespace {

constexpr uint64_t kImageSize = 64UL << 20;
constexpr int kRounds = 4;
constexpr size_t kBlockSize = 64 << 10;
constexpr int kBlocks = 8;

// Each coroutine owns one image: mkfs -> mount -> write its own pattern ->
// read back and verify. The two coroutines run on different vCPUs, so a
// mis-bound backend shows up as the peer's pattern.
int run_round(int id, int round) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/extfs_concurrent_%d.img", id);
    auto image = photon::fs::open_localfile_adaptor(
        path, O_RDWR | O_CREAT | O_TRUNC, 0644, 0);
    if (!image) return -1;
    DEFER(delete image);
    if (image->ftruncate(kImageSize) < 0) return -1;
    if (photon::fs::make_extfs(image) < 0) return -1;

    auto fs = photon::fs::new_extfs(image, false);
    if (!fs) return -1;
    DEFER(delete fs);

    char name[32];
    snprintf(name, sizeof(name), "/payload-%d-%d", id, round);
    auto file = fs->open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (!file) return -1;
    DEFER(delete file);

    char expected = 'A' + (id * kRounds + round) % 26;
    std::vector<char> buf(kBlockSize);
    for (int b = 0; b < kBlocks; b++) {
        memset(buf.data(), expected, buf.size());
        if (file->pwrite(buf.data(), buf.size(), (off_t)b * kBlockSize) !=
            (ssize_t)buf.size())
            return -1;
    }

    for (int b = 0; b < kBlocks; b++) {
        memset(buf.data(), 0, buf.size());
        if (file->pread(buf.data(), buf.size(), (off_t)b * kBlockSize) !=
            (ssize_t)buf.size())
            return -1;
        for (size_t i = 0; i < buf.size(); i++) {
            if (buf[i] != expected) {
                LOG_ERROR("content mismatch, id=` round=` block=` offset=`",
                          id, round, b, i);
                return -1;
            }
        }
    }
    return 0;
}

void worker(int id, std::atomic<int>* failures) {
    for (int r = 0; r < kRounds; r++) {
        if (run_round(id, r) != 0) {
            LOG_ERROR("round failed, id=` round=`", id, r);
            failures->fetch_add(1);
            return;
        }
    }
}

}  // namespace

TEST(ExtfsConcurrent, different_images_do_not_interfere) {
    int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    ASSERT_EQ(0, ret);
    DEFER(photon::fini());

    ret = photon_std::work_pool_init(2, photon::INIT_EVENT_DEFAULT,
                                     photon::INIT_IO_NONE);
    ASSERT_EQ(0, ret);
    DEFER(photon_std::work_pool_fini());

    std::atomic<int> failures{0};
    std::vector<photon_std::thread> threads;
    for (int i = 0; i < 2; i++) threads.emplace_back(worker, i, &failures);
    for (auto& th : threads) th.join();

    EXPECT_EQ(0, failures.load());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
