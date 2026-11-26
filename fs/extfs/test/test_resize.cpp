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
#include <fcntl.h>
#include <errno.h>
#include <string>
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <photon/fs/forwardfs.h>
#include <photon/common/alog.h>
#include "../../../test/gtest.h"

#define IMAGE_SIZE (64UL << 20)
#define FILE_SIZE (32UL << 20)

// wraps the image IFile and injects read failures on demand
class FaultFile : public photon::fs::ForwardFileBase<photon::fs::IFile> {
public:
    int read_errno = 0;
    FaultFile(photon::fs::IFile *file) : ForwardFileBase(file) {}
    ssize_t pread(void *buf, size_t count, off_t offset) override {
        if (read_errno) {
            errno = read_errno;
            return -1;
        }
        return m_file->pread(buf, count, offset);
    }
};

class ExtfsResize : public ::testing::Test {
public:
    const char *image_path = "/tmp/extfs_resize.img";
    photon::fs::IFile *image = nullptr;

    void SetUp() override {
        image = photon::fs::open_localfile_adaptor(image_path, O_CREAT | O_TRUNC | O_RDWR, 0666);
        ASSERT_NE(nullptr, image);
        ASSERT_EQ(0, image->ftruncate(IMAGE_SIZE));
        ASSERT_EQ(0, photon::fs::make_extfs(image));
    }
    void TearDown() override {
        delete image;
        ::unlink(image_path);
    }
    // write a marker file so shrinking has real data to account for
    void write_marker() {
        auto extfs = photon::fs::new_extfs(image, false);
        ASSERT_NE(nullptr, extfs);
        DEFER(delete extfs);
        auto file = extfs->open("/f", O_RDWR | O_CREAT, 0666);
        ASSERT_NE(nullptr, file);
        DEFER(delete file);
        std::string data;
        while (data.size() < FILE_SIZE) data.append("abcdefghijklmnopqrstuvwxyz0123456789");
        data.resize(FILE_SIZE);
        ASSERT_EQ((ssize_t)data.size(), file->pwrite(data.data(), data.size(), 0));
    }
    void check_marker() {
        auto extfs = photon::fs::new_extfs(image, false);
        ASSERT_NE(nullptr, extfs);
        DEFER(delete extfs);
        auto file = extfs->open("/f", O_RDONLY);
        ASSERT_NE(nullptr, file);
        DEFER(delete file);
        std::string data;
        while (data.size() < FILE_SIZE) data.append("abcdefghijklmnopqrstuvwxyz0123456789");
        data.resize(FILE_SIZE);
        std::string buf(FILE_SIZE, '\0');
        ASSERT_EQ((ssize_t)FILE_SIZE, file->pread(&buf[0], FILE_SIZE, 0));
        ASSERT_EQ(data, buf);
    }
};

TEST_F(ExtfsResize, grow) {
    write_marker();
    uint64_t size = IMAGE_SIZE * 2;
    ASSERT_EQ(0, photon::fs::resize_extfs(image, &size));
    EXPECT_EQ(IMAGE_SIZE * 2, size);
    ASSERT_EQ(0, image->ftruncate(size));
    check_marker();
}

TEST_F(ExtfsResize, shrink_too_small_reports_minimum) {
    write_marker();
    uint64_t size = 1UL << 20;
    auto ret = photon::fs::resize_extfs(image, &size);
    EXPECT_EQ(EXT2_ET_TOOSMALL, ret);
    // the minimum must cover the 32MB of data plus metadata, and cannot
    // exceed the current size
    EXPECT_GT(size, FILE_SIZE);
    EXPECT_LE(size, IMAGE_SIZE);
}

TEST_F(ExtfsResize, conflicting_64bit_flags) {
    uint64_t size = IMAGE_SIZE * 2;
    auto ret = photon::fs::resize_extfs(image, &size, 0x0400 | 0x0800);
    EXPECT_EQ(EXT2_ET_INVALID_ARGUMENT, ret);
}

TEST_F(ExtfsResize, null_new_size) {
    EXPECT_EQ(EXT2_ET_INVALID_ARGUMENT, photon::fs::resize_extfs(image, nullptr));
}

TEST_F(ExtfsResize, io_error_propagates) {
    write_marker();
    FaultFile fault(image);
    fault.read_errno = EIO;
    uint64_t size = IMAGE_SIZE * 2;
    // the failure hits before any metadata is written, so the image stays
    // mountable afterwards
    EXPECT_EQ(EIO, photon::fs::resize_extfs(&fault, &size));
    fault.read_errno = 0;
    check_marker();
}

int main(int argc, char **argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER(photon::fini(););
    set_log_output_level(1);

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}