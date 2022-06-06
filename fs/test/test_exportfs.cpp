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

// #define protected public
// #define private public

#include "../exportfs.cpp"

#include <errno.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <type_traits>
#include <photon/common/alog.h>
#include "mock.h"
#include <atomic>

using namespace photon;
using namespace photon::fs;
using namespace testing;

constexpr uint64_t magic = 150820;
static std::atomic<int> work(0);

template<typename T, uint64_t val>
int callback(void*, AsyncResult<T>* ret) {
    EXPECT_EQ(val, ret->result);
    LOG_DEBUG("DONE `", VALUE(ret->operation));
    work--;
    return 0;
}

template<uint64_t val>
int callbackf(void*, AsyncResult<IAsyncFile*>* ret) {
    auto pt = static_cast<ExportAsAsyncFile*>(ret->result);
    EXPECT_EQ(val, reinterpret_cast<uint64_t>(pt->m_file));
    LOG_DEBUG("DONE `", VALUE(ret->operation));
    work--;
    return 0;
}

template<uint64_t val>
int callbackd(void*, AsyncResult<AsyncDIR*>* ret) {
    auto pt = static_cast<ExportAsAsyncDIR*>(ret->result);
    EXPECT_EQ(val, reinterpret_cast<uint64_t>(pt->m_dirp));
    LOG_DEBUG("DONE `", VALUE(ret->operation));
    work--;
    return 0;
}

template<uint64_t val>
int callbackent(void*, AsyncResult<dirent*>* ret) {
    EXPECT_EQ(val, reinterpret_cast<uint64_t>(ret->result));
    LOG_DEBUG("DONE `", VALUE(ret->operation));
    work--;
    return 0;
}

int callbackvoid(void*, AsyncResult<void>* ret) {
    LOG_DEBUG("DONE `", VALUE(ret->operation));
    work--;
    return 0;
}


#define CALL_TEST0(obj, method, cb) {               \
    work++;                                         \
    LOG_DEBUG("START `->`()", #obj, #method);       \
    obj->method(cb);                                \
    LOG_DEBUG("WAITING `->`()", #obj, #method);     \
}

#define CALL_TEST(obj, method, cb, ...) {           \
    work++;                                         \
    LOG_DEBUG("START `->`()", #obj, #method);       \
    obj->method(__VA_ARGS__, cb);                   \
    LOG_DEBUG("WAITING `->`()", #obj, #method);     \
}


TEST(ExportFS, basic) {
    photon::thread_init();
    photon::fd_events_init();
    exportfs_init();
    DEFER({
        exportfs_fini();
        photon::fd_events_fini();
        photon::thread_fini();
    });
    Mock::MockNullFile* mockfile = new Mock::MockNullFile();
    Mock::MockNullFileSystem* mockfs = new Mock::MockNullFileSystem();
    Mock::MockNullDIR* mockdir = new Mock::MockNullDIR();
    auto file = export_as_async_file(mockfile);
    auto fs = export_as_async_fs(mockfs);
    auto dir = export_as_async_dir(mockdir);
    DEFER({
        delete file;
        delete dir;
        delete fs;
    });

    EXPECT_EQ(nullptr, file->filesystem());

    EXPECT_CALL(*mockfile, read(_, _)).Times(AtLeast(1)).WillRepeatedly(ReturnArg<1>());
    EXPECT_CALL(*mockfile, readv(_, _)).Times(AtLeast(1)).WillRepeatedly(ReturnArg<1>());
    EXPECT_CALL(*mockfile, write(_, _)).Times(AtLeast(1)).WillRepeatedly(ReturnArg<1>());
    EXPECT_CALL(*mockfile, writev(_, _)).Times(AtLeast(1)).WillRepeatedly(ReturnArg<1>());
    EXPECT_CALL(*mockfile, pread(_, _, _)).Times(AtLeast(1)).WillRepeatedly(ReturnArg<1>());
    EXPECT_CALL(*mockfile, preadv(_, _, _)).Times(AtLeast(1)).WillRepeatedly(ReturnArg<1>());
    EXPECT_CALL(*mockfile, pwrite(_, _, _)).Times(AtLeast(1)).WillRepeatedly(ReturnArg<1>());
    EXPECT_CALL(*mockfile, pwritev(_, _, _)).Times(AtLeast(1)).WillRepeatedly(ReturnArg<1>());
    EXPECT_CALL(*mockfile, lseek(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfile, fsync()).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfile, fdatasync()).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfile, close()).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfile, fchmod(_)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfile, fchown(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfile, fstat(_)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfile, ftruncate(_)).Times(AtLeast(1)).WillRepeatedly(Return(0));


    Callback<AsyncResult<ssize_t>*> cbsst, cbsst0;
    cbsst.bind(nullptr, &callback<ssize_t, (ssize_t)magic>);
    cbsst0.bind(nullptr, &callback<ssize_t, 0>);
    Callback<AsyncResult<int>*> cbint;
    cbint.bind(nullptr, &callback<int, (int)0>);
    Callback<AsyncResult<off_t>*> cboff;
    cboff.bind(nullptr, &callback<off_t, 0>);
    const struct iovec* nulliov = nullptr;

    CALL_TEST(file, read, cbsst, nullptr, magic);
    CALL_TEST(file, readv, cbsst, nulliov, magic);
    CALL_TEST(file, write, cbsst, nullptr, magic);
    CALL_TEST(file, writev, cbsst, nulliov, magic);
    CALL_TEST(file, pread, cbsst, nullptr, magic, 0);
    CALL_TEST(file, preadv, cbsst, nulliov, magic, 0);
    CALL_TEST(file, pwrite, cbsst, nullptr, magic, 0);
    CALL_TEST(file, pwritev, cbsst, nulliov, magic, 0);
    CALL_TEST(file, lseek, cboff, 0L, 0L);
    CALL_TEST0(file, fsync, cbint);
    CALL_TEST0(file, fdatasync, cbint);
    CALL_TEST0(file, close, cbint);
    CALL_TEST(file, fchmod, cbint, 0);
    CALL_TEST(file, fchown, cbint, 0, 0);
    CALL_TEST(file, fstat, cbint, nullptr);
    CALL_TEST(file, ftruncate, cbint, 0);

    IFile* paf_magic = reinterpret_cast<IFile*>(magic);
    DIR* pad_magic = reinterpret_cast<DIR*>(magic);
    Callback<AsyncResult<IAsyncFile*>*> cbaf;
    cbaf.bind(nullptr, callbackf<magic>);
    Callback<AsyncResult<AsyncDIR*>*> cbad;
    cbad.bind(nullptr, callbackd<magic>);

    EXPECT_CALL(*mockfs, open(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(paf_magic));
    EXPECT_CALL(*mockfs, open(_, _, _)).Times(AtLeast(1)).WillRepeatedly(Return(paf_magic));
    EXPECT_CALL(*mockfs, creat(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(paf_magic));
    EXPECT_CALL(*mockfs, mkdir(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, rmdir(_)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, symlink(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, readlink(_, _, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, link(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, rename(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, unlink(_)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, chmod(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, chown(_, _, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, lchown(_, _, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, statfs(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, statvfs(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, stat(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, lstat(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, access(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, truncate(_, _)).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, syncfs()).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockfs, opendir(_)).Times(AtLeast(1)).WillRepeatedly(Return(pad_magic));

    CALL_TEST(fs, open, cbaf, "", 0);
    CALL_TEST(fs, open, cbaf, "", 0, 0);
    CALL_TEST(fs, creat, cbaf, "", 0);
    CALL_TEST(fs, mkdir, cbint, "", 0);
    CALL_TEST(fs, rmdir, cbint, "");
    CALL_TEST(fs, symlink, cbint, "", "");
    CALL_TEST(fs, readlink, cbsst0, "", nullptr, 0);
    CALL_TEST(fs, link, cbint, "", "");
    CALL_TEST(fs, rename, cbint, "", "");
    CALL_TEST(fs, unlink, cbint, "");
    CALL_TEST(fs, chmod, cbint, "", 0);
    CALL_TEST(fs, chown, cbint, "", 0, 0);
    CALL_TEST(fs, lchown, cbint, "", 0, 0);
    CALL_TEST(fs, statfs, cbint, "", nullptr);
    CALL_TEST(fs, statvfs, cbint, "", nullptr);
    CALL_TEST(fs, stat, cbint, "", nullptr);
    CALL_TEST(fs, lstat, cbint, "", nullptr);
    CALL_TEST(fs, access, cbint, "", 0);
    CALL_TEST(fs, truncate, cbint, "", 0);
    CALL_TEST0(fs, syncfs, cbint);
    CALL_TEST(fs, opendir, cbad, "");

    struct dirent* nullent = reinterpret_cast<dirent*>(magic);
    Callback<AsyncResult<dirent*>*> cbdirent;
    cbdirent.bind(nullptr, callbackent<magic>);
    Callback<AsyncResult<void>*> cbvoid;
    cbvoid.bind(nullptr, callbackvoid);
    Callback<AsyncResult<long>*> cblong;
    cblong.bind(nullptr, callback<long, 0>);

    EXPECT_CALL(*mockdir, closedir()).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockdir, next()).Times(AtLeast(1)).WillRepeatedly(Return(0));
    EXPECT_CALL(*mockdir, get()).Times(AtLeast(1)).WillRepeatedly(Return(nullent));
    EXPECT_CALL(*mockdir, rewinddir()).Times(AtLeast(1));
    EXPECT_CALL(*mockdir, seekdir(_)).Times(AtLeast(1));
    EXPECT_CALL(*mockdir, telldir()).Times(AtLeast(1)).WillRepeatedly(Return(0));

    CALL_TEST0(dir, closedir, cbint);
    CALL_TEST0(dir, next, cbint);
    CALL_TEST0(dir, get, cbdirent);
    CALL_TEST0(dir, rewinddir, cbvoid);
    CALL_TEST(dir, seekdir, cbvoid, 0);
    CALL_TEST0(dir, telldir, cblong);

    while (work > 0) thread_yield_to(nullptr);
}

TEST(ExportFS, init_fini_failed_situation) {
    auto _o_output = log_output;
    log_output = log_output_null;
    DEFER({
        log_output = _o_output;
    });
    auto ret = exportfs_fini();
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ENOSYS, errno);
    photon::thread_init();
    photon::fd_events_init();
    ret = exportfs_init();
    EXPECT_EQ(0, ret);
    ret = exportfs_init();
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EALREADY, errno);
    ret = exportfs_fini();
    EXPECT_EQ(0, ret);
    photon::fd_events_fini();
    // photon::thread_fini();
}

TEST(ExportFS, op_failed_situation) {
    auto _o_output = log_output;
    log_output = log_output_null;
    DEFER({
        log_output = _o_output;
    });
    // photon::thread_init();
    photon::fd_events_init();
    exportfs_init();
    DEFER({
        exportfs_fini();
        photon::fd_events_fini();
        // photon::thread_fini();
    });
    Mock::MockNullFile* mockfile = new Mock::MockNullFile;
    errno = 0;
    EXPECT_CALL(*mockfile, read(_, _))
        .WillRepeatedly(SetErrnoAndReturn(ENOSYS, -1)); // failure
    IAsyncFile* file = export_as_async_file(mockfile);
    DEFER({
        delete file;
    });

    auto action = [=](AsyncResult<ssize_t>* ret){
        EXPECT_EQ(ENOSYS, ret->error_number);
        errno = EDOM;
        return -1;
    };
    Callback<AsyncResult<ssize_t>*> fail_cb(action);
    file->read(nullptr, 0, fail_cb);
    while (EDOM != errno) photon::thread_yield();
    EXPECT_EQ(EDOM, errno);
}


#undef CALL_TEST
#undef CALL_TEST0

int main(int argc, char **argv)
{
    photon::thread_init();
    DEFER(photon::thread_fini());
    ::testing::InitGoogleTest(&argc, argv);
    int ret = RUN_ALL_TESTS();
    LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
