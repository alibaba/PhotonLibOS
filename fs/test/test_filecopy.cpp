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

#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/wait.h>

#include <photon/fs/filecopy.h>
#include <photon/fs/localfs.h>
#include <photon/thread/thread.h>
#include <photon/common/alog.h>
#include <photon/io/aio-wrapper.h>
#include <photon/io/fd-events.h>

using namespace photon;

TEST(filecopy, simple_localfile_copy) {
    auto fs = fs::new_localfs_adaptor("/tmp");
    auto f1 = fs->open("test_filecopy_src", O_RDONLY);
    auto f2 = fs->open("test_filecopy_dst", O_RDWR | O_CREAT | O_TRUNC, 0644);
    auto ret = fs::filecopy(f1, f2);
    delete f2;
    delete f1;
    delete fs;
    EXPECT_EQ(500 * 4100, ret);
    ret = system("diff -b /tmp/test_filecopy_src /tmp/test_filecopy_dst");
    EXPECT_EQ(0, WEXITSTATUS(ret));
}

#ifdef __linux__
TEST(filecopy, libaio_localfile_copy) {
    photon::libaio_wrapper_init();
    DEFER(photon::libaio_wrapper_fini());
    auto fs = fs::new_localfs_adaptor("/tmp", fs::ioengine_libaio);
    auto f1 = fs->open("test_filecopy_src", O_RDONLY);
    auto f2 = fs->open("test_filecopy_dst2", O_RDWR | O_CREAT | O_TRUNC, 0644);
    auto ret = fs::filecopy(f1, f2);
    delete f2;
    delete f1;
    delete fs;
    EXPECT_EQ(500 * 4100, ret);
    ret = system("diff -b /tmp/test_filecopy_src /tmp/test_filecopy_dst2");
    EXPECT_EQ(0, WEXITSTATUS(ret));
}
#endif

int main(int argc, char **argv) {
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());

    ::testing::InitGoogleTest(&argc, argv);
    // 500 * 4100, make sure it have no aligned file length
    system("dd if=/dev/urandom of=/tmp/test_filecopy_src bs=500 count=4100");
    int ret = RUN_ALL_TESTS();
    LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
