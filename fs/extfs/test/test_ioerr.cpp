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
#define FILE_SIZE (1UL << 20)

// wraps the image IFile and injects I/O failures on demand
class FaultFile : public photon::fs::ForwardFileBase<photon::fs::IFile> {
public:
    int read_errno = 0;
    int write_errno = 0;
    bool short_read = false;
    bool short_write = false;
    FaultFile(photon::fs::IFile *file) : ForwardFileBase(file) {}
    ssize_t pread(void *buf, size_t count, off_t offset) override {
        if (read_errno) {
            errno = read_errno;
            return -1;
        }
        auto ret = m_file->pread(buf, count, offset);
        if (short_read && ret > 0) return ret - 1;
        return ret;
    }
    ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        if (write_errno) {
            errno = write_errno;
            return -1;
        }
        auto ret = m_file->pwrite(buf, count, offset);
        if (short_write && ret > 0) return ret - 1;
        return ret;
    }
};

class ExtfsIoError : public ::testing::Test {
public:
    const char *image_path = "/tmp/extfs_ioerr.img";
    photon::fs::IFile *image = nullptr;
    FaultFile *fault = nullptr;
    photon::fs::IFileSystem *extfs = nullptr;

    void SetUp() override {
        image = photon::fs::open_localfile_adaptor(image_path, O_CREAT | O_TRUNC | O_RDWR, 0666);
        ASSERT_NE(nullptr, image);
        ASSERT_EQ(0, image->ftruncate(IMAGE_SIZE));
        ASSERT_EQ(0, photon::fs::make_extfs(image));
        fault = new FaultFile(image);
        extfs = photon::fs::new_extfs(fault, false);
        ASSERT_NE(nullptr, extfs);
        auto file = extfs->open("/f", O_RDWR | O_CREAT, 0666);
        ASSERT_NE(nullptr, file);
        DEFER(delete file);
        std::string data;
        while (data.size() < FILE_SIZE) data.append("abcdefghijklmnopqrstuvwxyz0123456789");
        data.resize(FILE_SIZE);
        ASSERT_EQ((ssize_t)data.size(), file->pwrite(data.data(), data.size(), 0));
    }
    void TearDown() override {
        delete extfs;
        delete fault;
        delete image;
        ::unlink(image_path);
    }
};

// a failed block read must surface as -1 with errno set, never as a
// positive "short read" count (see the errcode-negation bug)
TEST_F(ExtfsIoError, read_failure) {
    auto file = extfs->open("/f", O_RDONLY);
    ASSERT_NE(nullptr, file);
    DEFER(delete file);
    char buf[4096];
    // sanity: readback works without injection; use the last block so the
    // blocks below stay uncached in the ext2_file_t handle
    ASSERT_EQ((ssize_t)sizeof(buf), file->pread(buf, sizeof(buf), FILE_SIZE - 4096));

    fault->read_errno = EIO;
    errno = 0;
    EXPECT_EQ(-1, file->pread(buf, sizeof(buf), 0));
    EXPECT_EQ(EIO, errno);

    // the backend errno is passed through, not flattened to EIO
    fault->read_errno = ENOSPC;
    errno = 0;
    EXPECT_EQ(-1, file->pread(buf, sizeof(buf), 4096));
    EXPECT_EQ(ENOSPC, errno);

    fault->read_errno = 0;
    fault->short_read = true;
    errno = 0;
    EXPECT_EQ(-1, file->pread(buf, sizeof(buf), 8192));
    EXPECT_EQ(EIO, errno);

    // disable injection so the fs can flush and close cleanly on teardown
    fault->short_read = false;
}

TEST_F(ExtfsIoError, write_failure) {
    auto file = extfs->open("/f", O_RDWR);
    ASSERT_NE(nullptr, file);
    DEFER(delete file);
    char buf[4096] = {0};

    fault->write_errno = EIO;
    errno = 0;
    EXPECT_EQ(-1, file->pwrite(buf, sizeof(buf), 0));
    EXPECT_EQ(EIO, errno);

    fault->write_errno = ENOSPC;
    errno = 0;
    EXPECT_EQ(-1, file->pwrite(buf, sizeof(buf), 4096));
    EXPECT_EQ(ENOSPC, errno);

    fault->write_errno = 0;
    fault->short_write = true;
    errno = 0;
    EXPECT_EQ(-1, file->pwrite(buf, sizeof(buf), 8192));
    EXPECT_EQ(EIO, errno);

    fault->short_write = false;
}

int main(int argc, char **argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER(photon::fini(););
    set_log_output_level(1);

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}