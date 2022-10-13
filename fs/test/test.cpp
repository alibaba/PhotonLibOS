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

#define protected public
#define private public

#include "../subfs.cpp"
#include "../localfs.cpp"
#include "../virtual-file.cpp"
#include "../path.cpp"
#include "../async_filesystem.cpp"
#include "../xfile.cpp"
#include "../aligned-file.cpp"

#undef private
#undef protected

#include <thread>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#ifdef __linux__
#include <malloc.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <random>

#include <photon/common/iovector.h>
#include <photon/common/enumerable.h>
#include <photon/fs/path.h>
#include <photon/fs/localfs.h>
#include <photon/fs/range-split.h>
#include <photon/fs/range-split-vi.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/xfile.h>
#include <photon/fs/aligned-file.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>
#include <photon/thread/thread11.h>

#include "mock.h"

using namespace std;
using namespace photon;
using namespace photon::fs;

TEST(Path, split)
{
    static const char* paths[]={
        "/asdf/jkl/bmp/qwer/x.jpg",
        "/kqw/wek///kjas/nn",
        "asdf",
        "/",
        "/qwer/jkl/",
        "/asdf",
    };

    vector<vector<string>> std_results = {
        {"asdf", "jkl", "bmp", "qwer", "x.jpg"},
        {"kqw", "wek", "kjas", "nn"},
        {"asdf"},
        {},
        {"qwer", "jkl"},
        {"asdf"},
    };

    vector<vector<string>> std_dir_results = {
        {"asdf", "jkl", "bmp", "qwer"},
        {"kqw", "wek", "kjas"},
        {},
        {},
        {"qwer", "jkl"},
        {},
    };

    auto it = std_results.begin();
    auto itd = std_dir_results.begin();
    for (auto p: paths)
    {
        Path path(p);
        vector<string> sp;
        for (auto x: path)
            sp.push_back(std::string(x));
        EXPECT_EQ(sp, *it);
        ++it;

        sp.clear();
        for (auto x: path.directory())
            sp.push_back(std::string(x));
        EXPECT_EQ(sp, *itd);
        ++itd;

        for (auto it = path.begin(); it!=path.end(); ++it)
            cout << *it << ", ";
        cout << endl;
    }
}

TEST(Path, xnames)
{
    static const char* paths[][3]={
        {"/asdf/jkl/bmp/qwer/x.jpg", "/asdf/jkl/bmp/qwer/", "x.jpg"},
        {"/x.jpg", "/", "x.jpg"},
        {"x.jpg", "", "x.jpg"},
        {"/kqw/wek///kjas/nn", "/kqw/wek///kjas/", "nn"},
        {"/kqw/wek///kjas/nn/", "/kqw/wek///kjas/", "nn"},
        {"/kqw/wek///kjas/nn///", "/kqw/wek///kjas/", "nn"},
    };
    for (auto p: paths)
    {
        Path path(p[0]);
        EXPECT_EQ(path.dirname(), p[1]);
        EXPECT_EQ(path.basename(), p[2]);
    }
}

TEST(Path, level_valid_ness)
{
    static pair<const char*, bool> cases[] = {
        {"/asdf/jkl/bmp/qwer/x.jpg", true},
        {"/x.jpg/../../x.jpg", false},
        {"asdf/../../x.jpg", false},
        {"../asdf", false},
    };
    for (auto& c: cases)
    {
        EXPECT_EQ(path_level_valid(c.first), c.second);
    }
}

TEST(string_view, equality) // dependented by `Path`
{
    fs::string_view a(nullptr, 0UL);
    fs::string_view b((char*)234, 0UL);
    EXPECT_EQ(a, b);
}

TEST(Tree, node)
{
    static const char* items[] = {"asdf", "jkl", "qwer", "zxcv", };
    static const char* subnodes[] = {">asdf", ">jkl", ">qwer", ">zxcv", };
    fs::string_view k1234 = "1234";
    auto v1234 = (void*)23456;
    uint64_t F = 2314, f;
    Tree::Node node;

    f = F;
    for (auto x: items)
        node.creat(x, (void*)f++);
    node.creat(k1234, (void*)1234);
    node.creat(k1234, (void*)2345);
    node.creat(k1234, v1234);
    EXPECT_EQ(node.size(), 5);

    for (auto x: subnodes)
        node.mkdir(x);

    EXPECT_EQ(node.size(), 9);

    f = F;
    void* v;
    for (auto x: items)
    {
        node.read(x, &v);
        EXPECT_EQ(v, (void*)f++);
        node.unlink(x);
        EXPECT_FALSE(node.is_file(x));
    }

    for (auto x: subnodes)
    {
        EXPECT_TRUE(node.is_dir(x));
        node.rmdir(x);
        EXPECT_FALSE(node.is_dir(x));
    }

    node.read(k1234, &v);
    EXPECT_EQ(v, (void*)1234);
}

TEST(Tree, tree)
{
    static const char* paths[] = {
        "/autobuild.sh",
//        "/docker.image",
        "/docker.image/.DS_Store",
        "/docker.image/a.out",
//        "/docker.image/a.out.dSYM",
//        "/docker.image/a.out.dSYM/Contents",
        "/docker.image/a.out.dSYM/Contents/Info.plist",
//        "/docker.image/a.out.dSYM/Contents/Resources",
//        "/docker.image/a.out.dSYM/Contents/Resources/DWARF",
        "/docker.image/a.out.dSYM/Contents/Resources/DWARF/a.out",
        "/docker.image/cxxopts.h",
        "/docker.image/dkimage.cpp",
        "/docker.image/dkimage.o",
        "/docker.image/filelog.cpp",
        "/docker.image/filelog.h",
        "/docker.image/filesystem.h",
        "/docker.image/fuse_adaptor.cpp",
        "/docker.image/fuse_adaptor.h",
        "/docker.image/localfs.cpp",
        "/docker.image/localfs.h",
        "/docker.image/logger.cpp",
        "/docker.image/logger.h",
        "/docker.image/main.cpp",
        "/docker.image/Makefile",
        "/docker.image/metar.cpp",
//        "/docker.image/overlayfs",
        "/docker.image/overlayfs/alicpp",
        "/docker.image/overlayfs/autobuild.sh",
//        "/docker.image/overlayfs/src",
        "/docker.image/overlayfs/src/common.h",
        "/docker.image/overlayfs/src/log.h",
        "/docker.image/overlayfs/src/main.cpp",
        "/docker.image/overlayfs/src/overlayfs.cpp",
        "/docker.image/overlayfs/src/overlayfs.h",
        "/docker.image/overlayfs/src/ovldir.h",
        "/docker.image/overlayfs/src/ovlfile.h",
        "/docker.image/overlayfs/src/strings.h",
        "/docker.image/overlayfs/src/super.h",
        "/docker.image/overlayfs/src/TARGETS",
        "/docker.image/overlayfs/TARGETS",
//        "/docker.image/overlayfs/unittest",
        "/docker.image/overlayfs/unittest/ovl_dir_test.cpp",
        "/docker.image/overlayfs/unittest/ovl_fs_test.cpp",
        "/docker.image/overlayfs/unittest/ovl_mock.h",
        "/docker.image/overlayfs/unittest/ovl_strings_test.cpp",
        "/docker.image/overlayfs/unittest/ovl_test.cpp",
        "/docker.image/overlayfs/unittest/ovl_test.h",
        "/docker.image/overlayfs/unittest/ovl_test_main.cpp",
        "/docker.image/overlayfs/unittest/TARGETS",
        "/docker.image/pangu2_errcode_tt.cpp",
        "/docker.image/pangu2_errcode_tt.h",
        "/docker.image/pangu2_fs.cpp",
        "/docker.image/pangu2_fs.h",
        "/docker.image/path.h",
        "/docker.image/strbuffer.h",
        "/docker.image/stream.h",
//        "/docker.image/tarfs",
        "/docker.image/tarfs/tar_dir.cpp",
        "/docker.image/tarfs/tar_dir.h",
        "/docker.image/tarfs/tar_file.cpp",
        "/docker.image/tarfs/tar_file.h",
        "/docker.image/tarfs/tar_parser.cpp",
        "/docker.image/tarfs/tar_parser.h",
        "/docker.image/tarfs/tarfs.cpp",
        "/docker.image/tarfs/tarfs.h",
        "/docker.image/tarfs/TARGETS",
//        "/docker.image/tarfs/test",
        "/docker.image/tarfs/test/local_file.cpp",
        "/docker.image/tarfs/test/local_file.h",
        "/docker.image/tarfs/test/tar_header_summary.cpp",
        "/docker.image/tarfs/test/tar_header_summary.h",
        "/docker.image/tarfs/test/tarfs_test.cpp",
        "/docker.image/tarfs/test/tarfs_test.h",
        "/docker.image/tarfs/test/TARGETS",
        "/docker.image/tarfs/test/TARGETS_backup",
        "/docker.image/tarfs/test/test_main.cpp",
        "/docker.image/TARGETS",
        "/docker.image/test.cpp",
        "/docker.image/tree.h",
        "/docker.image/vfs.cpp",
        "/docker.image/vfs.h",
        "/docker.image/vlog.cpp",
        "/docker.image/vlog.h",
//        "/docker.image.xcodeproj",
        "/docker.image.xcodeproj/project.pbxproj",
//        "/docker.image.xcodeproj/project.xcworkspace",
        "/docker.image.xcodeproj/project.xcworkspace/contents.xcworkspacedata",
//        "/docker.image.xcodeproj/project.xcworkspace/xcuserdata",
//        "/docker.image.xcodeproj/project.xcworkspace/xcuserdata/lihuiba.xcuserdatad",
        "/docker.image.xcodeproj/project.xcworkspace/xcuserdata/lihuiba.xcuserdatad/UserInterfaceState.xcuserstate",
//        "/docker.image.xcodeproj/xcuserdata",
//        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad",
//        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad/xcdebugger",
        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad/xcdebugger/Breakpoints_v2.xcbkptlist",
//        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad/xcschemes",
        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad/xcschemes/docker.image.xcscheme",
        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad/xcschemes/metar 2.xcscheme",
        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad/xcschemes/xcschememanagement.plist",
//        "/pangu2_sdk",
        "/pangu2_sdk/README",
        "/TARGETS",
    };

    static const char* dirs[] = {
        "/docker.image",
        "/docker.image/a.out.dSYM",
        "/docker.image/a.out.dSYM/Contents",
        "/docker.image/a.out.dSYM/Contents/Resources",
        "/docker.image/a.out.dSYM/Contents/Resources/DWARF",
        "/docker.image/overlayfs",
        "/docker.image/overlayfs/src",
        "/docker.image/overlayfs/unittest",
        "/docker.image/tarfs",
        "/docker.image/tarfs/test",
        "/docker.image.xcodeproj",
        "/docker.image.xcodeproj/project.xcworkspace",
        "/docker.image.xcodeproj/project.xcworkspace/xcuserdata",
        "/docker.image.xcodeproj/project.xcworkspace/xcuserdata/lihuiba.xcuserdatad",
        "/docker.image.xcodeproj/xcuserdata",
        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad",
        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad/xcdebugger",
        "/docker.image.xcodeproj/xcuserdata/lihuiba.xcuserdatad/xcschemes",
        "/pangu2_sdk",
    };

    Tree::Node node;
    auto F = (void*)831;
    for (auto x: paths)
    {
        auto ret = node.creat(x, F, true);
        EXPECT_EQ(ret, 0);
    }

    for (auto x: dirs)
    {
        auto ret = node.is_dir(x);
        EXPECT_TRUE(ret);
    }

    for (auto x: paths)
    {
        void* v;
        auto ret = node.read(x, &v);
        EXPECT_EQ(ret, 0);
        EXPECT_EQ(v, F);
    }
}

namespace photon {
namespace fs
{
    class ExampleAsyncFile : public IAsyncFile
    {
    public:
        const static ssize_t SIZE0 = 0;
        virtual IAsyncFileSystem* filesystem() override
        {
            return nullptr;
        }
        OVERRIDE_ASYNC(ssize_t, read, void *buf, size_t count)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(ssize_t, readv, const struct iovec *iov, int iovcnt)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(ssize_t, write, const void *buf, size_t count)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(ssize_t, writev, const struct iovec *iov, int iovcnt)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(ssize_t, pread, void *buf, size_t count, off_t offset)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(ssize_t, preadv, const struct iovec *iov, int iovcnt, off_t offset)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(ssize_t, pwrite, const void *buf, size_t count, off_t offset)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(ssize_t, pwritev, const struct iovec *iov, int iovcnt, off_t offset)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(off_t, lseek, off_t offset, int whence)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC0(int, fsync)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC0(int, fdatasync)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC0(int, close)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, fchmod, mode_t mode)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, fchown, uid_t owner, gid_t group)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, fstat, struct stat *buf)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, ftruncate, off_t length)
        {
            callback_umimplemented(done);
        }
    };

    class ExampleAsyncFileSystem: public IAsyncFileSystem
    {
    public:
        OVERRIDE_ASYNC(IAsyncFile*, open, const char *pathname, int flags)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(IAsyncFile*, open, const char *pathname, int flags, mode_t mode)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(IAsyncFile*, creat, const char *pathname, mode_t mode)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, mkdir, const char *pathname, mode_t mode)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, rmdir, const char *pathname)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, symlink, const char *oldname, const char *newname)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(ssize_t, readlink, const char *path, char *buf, size_t bufsiz)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, link, const char *oldname, const char *newname)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, rename, const char *oldname, const char *newname)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, unlink, const char *filename)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, chmod, const char *pathname, mode_t mode)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, chown, const char *pathname, uid_t owner, gid_t group)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, lchown, const char *pathname, uid_t owner, gid_t group)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, statfs, const char *path, struct statfs *buf)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, statvfs, const char *path, struct statvfs *buf)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, stat, const char *path, struct stat *buf)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, lstat, const char *path, struct stat *buf)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, access, const char *pathname, int mode)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, truncate, const char *path, off_t length)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, utime, const char *path, const struct utimbuf *file_times)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, utimes, const char *path, const struct timeval times[2])
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, lutimes, const char *path, const struct timeval times[2])
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(int, mknod, const char *path, mode_t mode, dev_t dev)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC0(int, syncfs)
        {
            callback_umimplemented(done);
        }
        OVERRIDE_ASYNC(AsyncDIR*, opendir, const char *name)
        {
            callback_umimplemented(done);
        }
    };

    IAsyncFile* new_example_async_file()
    {
        return new ExampleAsyncFile;
    }
    IAsyncFileSystem* new_example_async_file_system()
    {
        return new ExampleAsyncFileSystem;
    }
}
}

int async_callback(void* ptr, AsyncResult<ssize_t>* ar)
{
    auto host = ptr;
    auto object = ar->object;
    auto operation = ar->operation;
    auto error_number = ar->error_number;
    auto result = ar->result;
    LOG_DEBUG(VALUE(host), VALUE(object), VALUE(operation), VALUE(error_number), VALUE(result));
    return 0;
}

TEST(AsyncFS, AsyncFS)
{
    auto f = fs::new_example_async_file();
    DEFER(delete f);
    f->pread(nullptr, 100, 200, {nullptr, &async_callback});

    class TestAsyncFS
    {
    public:
        int async_callback(AsyncResult<ssize_t>* ar)
        {
            return ::async_callback(this, ar);
        }
        void do_test(IAsyncFile* f)
        {
            f->pread(nullptr, 200, 300, {this, &TestAsyncFS::async_callback});
        }
    };

    TestAsyncFS tafs;
    tafs.do_test(f);
}

class AFile : public ExampleAsyncFile
{
public:
    OVERRIDE_ASYNC(ssize_t, pread, void *buf, size_t count, off_t offset)
    {
        std::thread t([=]()
        {
            ::sleep(1);
            callback_umimplemented<ssize_t>(done);
        });
        t.detach();
    }

    OVERRIDE_ASYNC(ssize_t, pwrite, const void *buf, size_t count, off_t offset)
    {
        LOG_DEBUG("into afile pwrite `", timeout);

        std::thread t([=]()
        {
            ::usleep(timeout);
            //only return count/2 for timeout fired
            callback(done, UINT32_MAX, (ssize_t)count/2, 0);
        });
        t.detach();
    }
};

IAsyncFile *exampleAfile = new AFile;

class ExampleAsyncDir: public AsyncDIR {
    OVERRIDE_ASYNC0(int, closedir) {
    }
    OVERRIDE_ASYNC0(dirent*, get) {
        std::thread t([=]()
        {
            ::usleep(timeout);
            AsyncResult<dirent*> r;
            r.object = this;
            r.operation = AsyncDIR::OPID_GETDIR;
            r.error_number = ENOSYS;
            r.result = nullptr;
            done(&r);
        });
        t.detach();
    }
    OVERRIDE_ASYNC0(int, next) {
    }
    OVERRIDE_ASYNC0(void, rewinddir) {
    }
    OVERRIDE_ASYNC(void, seekdir, long loc) {
    }
    OVERRIDE_ASYNC0(long, telldir){
    }
};


class AFS : public ExampleAsyncFileSystem {
public:
    OVERRIDE_ASYNC(IAsyncFile*, open, const char *pathname, int flags)
    {
        std::thread t([=]()
        {
            callback(done, UINT32_MAX, exampleAfile, 0);
        });
        t.detach();
    }

    OVERRIDE_ASYNC(AsyncDIR*, opendir, const char *name)
    {
        std::thread t([=]()
        {
            ::usleep(timeout);
            if (name[0] == 's') {
                AsyncDIR *d = new ExampleAsyncDir;
                callback(done, UINT32_MAX, d, 0);
            } else {
                AsyncResult<AsyncDIR*> r;
                r.object = this;
                r.operation = UINT32_MAX;
                r.error_number = ETIMEDOUT;
                r.result = nullptr;
                done(&r);
            }
        });
        t.detach();
    }
};



TEST(AsyncFS, Adaptor)
{
    // fd_events_init();
    auto af = new AFile;
    auto f = new_async_file_adaptor(af, 1000);
    errno = 4123786;
    ssize_t ret = f->pread(nullptr, 100, 200);
    EXPECT_EQ(ret, -1);
    EXPECT_EQ(errno, ENOSYS);
    delete f;
    // fd_events_fini();
}

TEST(AsyncFS, Timeout)
{
    // fd_events_init();
    auto af = new AFile;
    uint64_t timeout = 2*1000*1000;
    auto f = new_async_file_adaptor(af, timeout);
    errno = 4123786;
    ssize_t ret = f->pwrite(nullptr, 100, 200);
    EXPECT_EQ(ret, 50);
    delete f;

    auto afs = new AFS;
    auto fs1 = new_async_fs_adaptor(afs, timeout);
    auto *f1 = fs1->open("noname", 0);
    ssize_t ret1 = f1->pwrite(nullptr, 200, 200);
    EXPECT_EQ(ret1, 100);
    auto dfailed = fs1->opendir("fail");
    EXPECT_EQ(dfailed, nullptr);
    EXPECT_EQ(errno, ETIMEDOUT);

    auto d = fs1->opendir("success");
    auto d1 = d->get();
    EXPECT_EQ(d1, nullptr);
    EXPECT_EQ(errno, ENOSYS);
    // fd_events_fini();
}

#if defined(__linux__)
// Mock a failed memory allocation test case
#if !__GLIBC_PREREQ(2, 34)
void (*old_free)(void *ptr, const void *caller);
void *(*old_malloc)(size_t size, const void *caller);

void *my_malloc(size_t size, const void *caller);
void my_free(void *ptr, const void *caller);

void malloc_hook() {
    __malloc_hook = my_malloc;
    __free_hook = my_free;
}

void malloc_unhook() {
    __malloc_hook = old_malloc;
    __free_hook = old_free;
}

void init_hook() {
    old_malloc = __malloc_hook;
    old_free = __free_hook;
}

void *my_malloc(size_t size, const void *caller) {
    return nullptr;
}

void my_free(void *ptr, const void *caller) {
    malloc_unhook();
    free(ptr);
}
#endif
#endif // defined(__linux__)

TEST(range_split, sub_range)
{
    // EXPECT_FALSE(FileSystem::sub_range());
    auto sr = fs::sub_range(0, 0, 0);
    EXPECT_FALSE(sr);
    sr.assign(0, 233, 1024);
    EXPECT_TRUE(sr);
    EXPECT_EQ(233, sr.begin());
    EXPECT_EQ(233+1024, sr.end());
    sr.clear();
    EXPECT_FALSE(sr);
    sr.assign(1, 233, 1024);
}

TEST(range_split, range_split_simple_case)
{
    fs::range_split split(42, 321, 32); // offset 42, length 321, interval 32
    // it should be split into [begin, end) as [42, 64)+[64, 76) +... +[352,363)
    // with abegin, aend as 0, 11
    // 11 parts in total
    EXPECT_FALSE(split.small_note);
    EXPECT_EQ(42, split.begin);
    EXPECT_EQ(363, split.end);
    EXPECT_EQ(1, split.abegin);
    EXPECT_EQ(12, split.aend);
    EXPECT_EQ(2, split.apbegin);
    EXPECT_EQ(split.apend,11);
    EXPECT_EQ(32, split.aligned_begin_offset());
    EXPECT_EQ(384, split.aligned_end_offset());
    auto p = split.all_parts();
    EXPECT_EQ(1, p.begin()->i);
    EXPECT_EQ(10, p.begin()->begin());
    EXPECT_EQ(32, p.begin()->end());
    EXPECT_EQ(12, p.end()->i);
    int cnt = 1;
    for (auto &rs: p) {
        EXPECT_EQ(cnt++, rs.i);
        if (rs != p.begin() && rs != p.end()) {
            EXPECT_EQ(0, rs.begin());
            EXPECT_EQ(32, rs.end());
        }
    }
    split = fs::range_split(2, 12, 24);
    EXPECT_TRUE(split.small_note);
    EXPECT_FALSE(split.preface);
    EXPECT_FALSE(split.postface);
    for (auto it = p.begin();it != p.end(); ++it) {
        EXPECT_EQ(it, it);
    }
}

TEST(range_split, range_split_aligned_case)
{
    fs::range_split split(32, 321, 32); // offset 32, length 321, interval 32
    // it should be split into [begin, end) as [32, 64)+[64, 76) +... +[352,353)
    // with abegin, aend as 0, 11
    // 11 parts in total
    EXPECT_EQ(32, split.begin);
    EXPECT_EQ(353, split.end);
    EXPECT_EQ(1, split.abegin);
    EXPECT_EQ(12, split.aend);
    EXPECT_EQ(1, split.apbegin);
    EXPECT_EQ(11, split.apend);
    auto p = split.all_parts();
    EXPECT_FALSE(split.is_aligned());
    EXPECT_TRUE(split.is_aligned(128));
    EXPECT_TRUE(split.is_aligned_ptr((const void*)(uint64_t(65536))));
    EXPECT_EQ(1, p.begin()->i);
    EXPECT_EQ(0, p.begin()->begin());
    EXPECT_EQ(32, p.begin()->end());
    EXPECT_EQ(12, p.end()->i);
    EXPECT_EQ(352, split.aligned_length());
    auto q = split.aligned_parts();
    int cnt = 1;
    for (auto &rs: q) {
        EXPECT_EQ(cnt++, rs.i);
        EXPECT_EQ(0, rs.begin());
        EXPECT_EQ(32, rs.end());
    }
    split = fs::range_split(0, 23, 24);
    EXPECT_TRUE(split.postface);
    split = fs::range_split(1, 23, 24);
    EXPECT_TRUE(split.preface);
    split = fs::range_split(0, 24, 24);
    EXPECT_FALSE(split.preface);
    EXPECT_FALSE(split.postface);
    EXPECT_FALSE(split.small_note);
    EXPECT_TRUE(split.aligned_parts().begin()->i + 1 == split.aligned_parts().end().i);
}

TEST(range_split, random_test) {
    uint64_t begin=rand(), length=rand(), interval=rand();
    LOG_DEBUG("begin=", begin, " length=", length, " interval=", interval);
    fs::range_split split(begin, length, interval);
    EXPECT_EQ(split.begin, begin);
    EXPECT_EQ(split.end, length + begin);
    EXPECT_EQ(split.interval, interval);
}


TEST(range_split_power2, basic) {
    fs::range_split_power2 split(42, 321, 32);
    for (auto &rs: split.all_parts()) {
        LOG_DEBUG(rs.i, ' ', rs.begin(), ' ', rs.end());
    }
    EXPECT_FALSE(split.small_note);
    EXPECT_EQ(42, split.begin);
    EXPECT_EQ(363, split.end);
    EXPECT_EQ(1, split.abegin);
    EXPECT_EQ(12, split.aend);
    EXPECT_EQ(2, split.apbegin);
    EXPECT_EQ(11, split.apend);
    EXPECT_EQ(32, split.aligned_begin_offset());
    EXPECT_EQ(384, split.aligned_end_offset());
    auto p = split.all_parts();
    EXPECT_EQ(p.begin()->i, 1);
    EXPECT_EQ(p.begin()->begin(), 10);
    EXPECT_EQ(p.begin()->end(), 32);
    EXPECT_EQ(p.end()->i, 12);
    int cnt = 1;
    for (auto &rs: p) {
        EXPECT_EQ(rs.i, cnt++);
        if (rs != p.begin() && rs != p.end()) {
            EXPECT_EQ(rs.begin(), 0);
            EXPECT_EQ(rs.end(), 32);
        }
    }
}

TEST(range_split_power2, random_test) {
    uint64_t offset, length, interval;
    offset = rand();
    length = rand();
    auto interval_shift = rand()%32 + 1;
    interval = 1<<interval_shift;
    fs::range_split_power2 split(offset, length, interval);
    EXPECT_EQ(offset, split.begin);
    EXPECT_EQ(offset + length, split.end);
    EXPECT_EQ(interval, split.interval);
}

void not_ascend_death()
{
    uint64_t kpfail[] = {0, 32, 796, 128, 256, 512, UINT64_MAX};
    fs::range_split_vi splitfail(12, 321, kpfail, 7);
}
TEST(range_split_vi, basic) {
    uint64_t kp[] = {0, 32, 64, 128, 256, 512, UINT64_MAX};
    fs::range_split_vi split(12, 321, kp, 7);
    uint64_t *it = kp;
    EXPECT_EQ(12, split.begin);
    EXPECT_EQ(333, split.end);
    EXPECT_TRUE(split.is_aligned((uint64_t)0));
    EXPECT_FALSE(split.is_aligned(1));
    EXPECT_TRUE(split.is_aligned(128));
    for (auto p : split.all_parts()) {
        LOG_DEBUG(p.i, ' ', p.begin(), ' ', p.end());
        EXPECT_EQ(*it == 0?12:0, p.begin());
        EXPECT_EQ(*it == 256? 321-256+12 :(*(it+1) - *it), p.end());
        it++;
    }
    uint64_t kpfail[] = {0, 32, 796, 128, 256, 512, UINT64_MAX};
    EXPECT_FALSE(split.ascending(kpfail, 7));
    EXPECT_DEATH(
        not_ascend_death(),
        ".*range-split-vi.h.*"
    );
}

TEST(range_split_vi, left_side_aligned) {
    uint64_t kp[] = {0, 32, 64, 128, 256, 512, UINT64_MAX};
    fs::range_split_vi split(0, 256, kp, 7);
    uint64_t *it = kp;
    EXPECT_EQ(0, split.begin);
    EXPECT_EQ(256, split.end);
    EXPECT_TRUE(split.is_aligned((uint64_t)0));
    EXPECT_FALSE(split.is_aligned(1));
    EXPECT_TRUE(split.is_aligned(128));
    for (auto p : split.all_parts()) {
        LOG_DEBUG(p.i, ' ', p.begin(), ' ', p.end());
        EXPECT_EQ(0, p.begin());
        EXPECT_EQ((*(it+1) - *it), p.end());
        it++;
    }
}

TEST(LocalFileSystem, basic) {
    std::unique_ptr<IFileSystem> fs(new_localfs_adaptor("/tmp/"));
    std::unique_ptr<IFile> lf(fs->open("test_local_fs", O_RDWR | O_CREAT, 0755));
    DEFER(lf->close(););
    lf->pwrite("HELLO", 5, 0);
    lf->fsync();
}

std::unique_ptr<char[]> random_block(uint64_t size) {
    std::unique_ptr<char[]> buff(new char[size]);
    char * p = buff.get();
    while (size--) {
        *(p++) = rand() % UCHAR_MAX;
    }
    return std::move(buff);
}

void random_content_rw_test(uint64_t test_block_size, uint64_t test_block_num, fs::IFile* file) {
    vector<std::unique_ptr<char[]>> rand_data;
    file->lseek(0, SEEK_SET);
    for (auto i = 0; i< test_block_num; i++) {
        rand_data.emplace_back(std::move(random_block(test_block_size)));
        char * buff = rand_data.back().get();
        file->write(buff, test_block_size);
    }
    file->fsync();
    file->lseek(0, SEEK_SET);
    char buff[test_block_size];
    for (const auto &data : rand_data) {
        file->read(buff, test_block_size);
        EXPECT_EQ(0, memcmp(data.get(), buff, test_block_size));
    }
}

void sequence_content_rw_test (uint64_t test_block_size, uint64_t test_block_num, const char* test_seq, fs::IFile* file) {
    char data[test_block_size];
    file->lseek(0, SEEK_SET);
    for (auto i = 0; i< test_block_num; i++) {
        memset(data, test_seq[i], test_block_size);
        file->write(data, test_block_size);
    }
    file->fdatasync();
    file->lseek(0, SEEK_SET);
    char buff[test_block_size];
    for (auto i = 0; i< test_block_num; i++) {
        file->read(buff, test_block_size);
        memset(data, *(test_seq++), test_block_size);
        EXPECT_EQ(0, memcmp(data, buff, test_block_size));
    }
}

void xfile_fstat_test(uint64_t fsize, fs::IFile* file) {
    struct stat st;
    file->fstat(&st);
    EXPECT_EQ(fsize, st.st_size);
}

void xfile_not_impl_test(fs::IFile* file) {
    auto retp = file->filesystem();
    EXPECT_EQ(nullptr, retp);
    EXPECT_EQ(ENOSYS, errno);
    auto ret = file->ftruncate(1024);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ENOSYS, errno);
    ret = file->fchmod(0755);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ENOSYS, errno);
    ret = file->fchown(0, 0);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ENOSYS, errno);
    errno = 0;
}

TEST(XFile, fixed_size_linear_file_basic) {
    const uint64_t test_file_num = 10;
    const uint64_t test_block_size = 3986;
    std::unique_ptr<IFileSystem> fs(new_localfs_adaptor("/tmp/"));
    IFile* lf[test_file_num];
    for (int i=0;i<test_file_num;i++) {
        lf[i] = (fs->open(("test_fixed_size_linear_file_" + std::to_string(i)).c_str(), O_RDWR | O_CREAT, 0666));
    }
    std::unique_ptr<IFile> xf(new_fixed_size_linear_file(test_block_size, lf, test_file_num, true));
    DEFER(
        xf->close();
    );
    xfile_fstat_test(test_file_num*test_block_size, xf.get());
    xfile_not_impl_test(xf.get());
    sequence_content_rw_test(test_block_size, test_file_num, "abcdefghijklmn", xf.get());
    random_content_rw_test(test_block_size, test_file_num, xf.get());
    xf->lseek(test_block_size*test_file_num, SEEK_SET);
    char buff[test_block_size];
    log_output = log_output_null;
    DEFER({
        log_output = log_output_stdout;
    });
    auto ret = xf->write(buff, test_block_size);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EIO, errno);
    errno = 0;
    ret = xf->pwrite(buff, test_block_size, test_file_num*test_block_size - 1);
    EXPECT_EQ(1, ret);
    EXPECT_EQ(0, errno);
}

TEST(XFile, linear_file_basic) {
    errno = 0;
    const uint64_t test_file_num = 4;
    const uint64_t test_file_size[] = {4096, 10240, 32768, 65536};
    const uint64_t test_block_size = 4096;
    uint64_t file_max_limit = 0;
    for (auto x : test_file_size) file_max_limit += x;
    std::unique_ptr<IFileSystem> fs(new_localfs_adaptor("/tmp/"));
    IFile* lf[test_file_num];
    for (int i=0;i<test_file_num;i++) {
        lf[i] = (fs->open(("test_linear_file_" + std::to_string(i)).c_str(), O_RDWR | O_CREAT, 0666));
        lf[i]->ftruncate(test_file_size[i]);
    }
    std::unique_ptr<IFile> xf(new_linear_file(lf, test_file_num, true));
    DEFER(
            xf->close();
         );
    uint64_t fsize = 0;
    for (auto &x : test_file_size) {
        fsize+=x;
    }
    xfile_fstat_test(fsize, xf.get());
    xfile_not_impl_test(xf.get());
    sequence_content_rw_test(test_block_size, test_file_num, "abcdefghijklmn", xf.get());
    random_content_rw_test(test_block_size, 27, xf.get()); //27*4096 is a little larger than sum of underlay files' size
    xf->lseek(4096*27, SEEK_SET);
    char buff[4096];
    log_output = log_output_null;
    DEFER({
        log_output = log_output_stdout;
    });
    auto ret = xf->write(buff, 4096);
    EXPECT_EQ(2048, ret);
    EXPECT_EQ(0, errno);
    ret = xf->pwrite(buff, 4096, file_max_limit);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EIO, errno);
}

TEST(XFile, stripe_file_basic) {
    const uint64_t test_file_num = 10;
    const uint64_t test_file_size = 4096;
    const uint64_t test_block_size = 128;
    std::unique_ptr<IFileSystem> fs(new_localfs_adaptor("/tmp/"));
    IFile* lf[test_file_num];
    for (int i=0;i<test_file_num;i++) {
        lf[i] = (fs->open(("test_stripe_file_" + std::to_string(i)).c_str(), O_RDWR | O_CREAT, 0666));
        lf[i]->ftruncate(test_file_size);
    }
    std::unique_ptr<IFile> xf(new_stripe_file(test_block_size, lf, 10, false));
    DEFER(
            xf->close();
         );
    xfile_fstat_test(test_file_num*test_file_size, xf.get());
    xfile_not_impl_test(xf.get());
    sequence_content_rw_test(test_file_size, test_file_num, "abcdefghijklmn", xf.get());
    random_content_rw_test(test_file_size, test_file_num, xf.get());
    char buff[test_block_size];
    log_output = log_output_null;
    DEFER({
        log_output = log_output_stdout;
    });
    auto ret = xf->pwrite(buff, test_block_size, test_file_num*test_file_size);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EIO, errno);
    errno = 0;
    ret = xf->pwrite(buff, test_block_size, test_file_num*test_file_size - 1);
    EXPECT_EQ(1, ret);
    EXPECT_EQ(0, errno);
}

TEST(XFile, error_stiuation) {
    log_output = log_output_null;
    DEFER({
        log_output = log_output_stdout;
    });
    using namespace testing;
    std::unique_ptr<IFileSystem> fs(new_localfs_adaptor("/tmp/"));
    IFile* normal_file = fs->open("test_normal_file", O_RDWR | O_CREAT, 0666);
    normal_file->ftruncate(4096);
    struct stat fake_stat, zero_stat, frage_stat, different_size_stat;
    normal_file->fstat(&fake_stat);
    memcpy(&zero_stat, &fake_stat, sizeof(fake_stat));
    memcpy(&frage_stat, &fake_stat, sizeof(fake_stat));
    memcpy(&different_size_stat, &fake_stat, sizeof(fake_stat));
    zero_stat.st_size=0;
    frage_stat.st_size=4095;
    different_size_stat.st_size=5120;
    PMock::MockNullFile nop;
    //fstat在LinearFile和StripeFile创建时会访问，因此需要先模拟为正常以成功打开文件，再模拟失败
    EXPECT_CALL(nop, fstat(_)).Times(AtLeast(1))
        .WillOnce(Return(-1))
        .WillOnce(DoAll(SetArgPointee<0>(zero_stat), Return(0))) //stripe_file construct
        .WillOnce(DoAll(SetArgPointee<0>(frage_stat), Return(0))) //stripe_file construct
        .WillOnce(DoAll(SetArgPointee<0>(different_size_stat), Return(0))) //stripe_file construct
        .WillOnce(DoAll(SetArgPointee<0>(fake_stat), Return(0))) //stripe_file construct
        .WillOnce(Return(-1))
        .WillOnce(Return(-1))
        .WillOnce(DoAll(SetArgPointee<0>(fake_stat), Return(0))) //linear_file create
        .WillOnce(Return(-1))
        .WillRepeatedly(DoAll(SetArgPointee<0>(fake_stat), Return(0)));

    ON_CALL(nop, write(_, _)).WillByDefault(Return(-1));
    EXPECT_CALL(nop, pwrite(_, _, _)).Times(AtLeast(1))
        .WillOnce(Return(-1))
        .WillOnce(Return(-1))
        .WillOnce(Return(-1));

    IFile* lf[] = {&nop, normal_file};
    IFile* xf=new_stripe_file(1024, lf, 2, false);
    EXPECT_EQ(nullptr, xf); //fstat -1

    xf=new_stripe_file(1024, lf, 2, false);
    EXPECT_EQ(nullptr, xf); //st_size == 0

    xf=new_stripe_file(1024, lf, 2, false);
    EXPECT_EQ(nullptr, xf); //st_size == 4095

    xf=new_stripe_file(1024, lf, 2, false);
    EXPECT_EQ(nullptr, xf); //st_size == 8192

    xf=new_stripe_file(1024, lf, 2, false);
    char buff[4096];
    auto ret = xf->write(buff, 4096);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(-1, xf->fstat(&fake_stat));
    xf->close();
    delete xf;

    xf = new_linear_file(lf, 2, false);
    EXPECT_EQ(nullptr, xf);

    xf = new_linear_file(lf, 2, false);
    ret = xf->write(buff, 4096);
    EXPECT_EQ(-1, ret);
    xf->close();
    delete xf;

    xf = new_fixed_size_linear_file(4096, lf, 2, false);
    ret = xf->write(buff, 4096);
    EXPECT_EQ(-1, ret);
    xf->close();
    delete xf;

    xf = new_linear_file(nullptr, 0, false);
    EXPECT_EQ(nullptr, xf);
    xf = new_fixed_size_linear_file(1, nullptr, 0, false);
    EXPECT_EQ(nullptr, xf);
    xf = new_fixed_size_linear_file(0, lf, 2, false);
    EXPECT_EQ(nullptr, xf);
    xf = new_stripe_file(1024, nullptr, 0, false);
    EXPECT_EQ(nullptr, xf);
    xf = new_stripe_file(1025, lf, 2, false);
    EXPECT_EQ(nullptr, xf);

#if defined(__linux__)
#if !__GLIBC_PREREQ(2, 34)
    init_hook();
    malloc_hook();
    IFile* rtp = new_linear_file(lf, 2, false);
    malloc_unhook();
    EXPECT_EQ(nullptr, rtp);
#endif
#endif // defined(__linux__)
}

void fill_random_buff(char * buff, size_t length) {
    for (size_t i = 0; i< length; i++) {
        buff[i] = rand() % UCHAR_MAX;
    }
}

void pread_pwrite_test(IFile *target, IFile *standard) {
    constexpr int max_file_size = 65536;
    constexpr int max_piece_length = 16384;
    constexpr int test_round = 10;
    char data[max_piece_length];
    char buff[max_piece_length];
    for (int i = 0;i < test_round; i++) {
        off_t off = rand() % max_file_size / getpagesize() * getpagesize();
        size_t len = rand() % max_piece_length / getpagesize() * getpagesize();
        if (off+len > max_file_size) {
            continue;
        }
        fill_random_buff(buff, len);
        target->pwrite(buff, len, off);
        standard->pwrite(buff, len, off);
        target->pread(data, len, off);
        EXPECT_EQ(0, memcmp(data, buff, len));
    }
    for (off_t off = 0; off < max_file_size; off+=max_piece_length) {
        auto len = target->pread(buff, max_piece_length, off);
        auto ret = standard -> pread(data, max_piece_length, off);
        EXPECT_EQ(len, ret);
        EXPECT_EQ(0, memcmp(data, buff, ret));
    }
    for (int i = 0;i < test_round; i++) {
        off_t off = rand() % max_file_size;
        size_t len = rand() % max_piece_length;
        if (off+len > max_file_size) {
            len = max_file_size - off;
        }
        fill_random_buff(buff, len);
        target->pwrite(buff, len, off);
        standard->pwrite(buff, len, off);
        target->pread(data, len, off);
        EXPECT_EQ(0, memcmp(data, buff, len));
    }
    for (off_t off = 0; off < max_file_size; off+=max_piece_length) {
        target->pread(buff, max_piece_length, off);
        standard -> pread(data, max_piece_length, off);
        EXPECT_EQ(0, memcmp(data, buff, max_piece_length));
    }
    struct stat stat;
    target->fstat(&stat);
    auto tsize = stat.st_size;
    standard->fstat(&stat);
    auto rsize = stat.st_size;
    EXPECT_EQ(rsize, tsize);
}

TEST(AlignedFileAdaptor, basic) {
    IFileSystem *fs = new_localfs_adaptor("/tmp/");
    IFile *normal_file = fs->open("test_aligned_file_normal", O_RDWR | O_CREAT, 0666);
    normal_file->ftruncate(65536);
    DEFER(normal_file->close());
    IFile *underlay_file = fs->open("test_aligned_file_aligned", O_RDWR | O_CREAT, 0666);
    underlay_file->ftruncate(65536);
    std::unique_ptr<IFile> aligned_file(new_aligned_file_adaptor(underlay_file, getpagesize(), true, false));
    DEFER(aligned_file->close());
    pread_pwrite_test(aligned_file.get(), normal_file);
    std::unique_ptr<IFile> aligned_file_2(new_aligned_file_adaptor(underlay_file, getpagesize(), false, true));
    DEFER(aligned_file_2->close());
    pread_pwrite_test(aligned_file_2.get(), normal_file);
}

TEST(AlignedFileAdaptor, err_situation) {
    log_output = log_output_null;
    DEFER({
        log_output = log_output_stdout;
    });
    using namespace testing;
    IFileSystem *fs = new_localfs_adaptor("/tmp/");
    IFile *underlay_file = fs->open("test_aligned_file_aligned", O_RDWR | O_CREAT, 0666);
    underlay_file->ftruncate(65536);
    IFile *wrong_size_aligned_file = new_aligned_file_adaptor(underlay_file, getpagesize() - 1, true, false);
    EXPECT_EQ(nullptr, wrong_size_aligned_file);
    EXPECT_EQ(EINVAL, errno);
    PMock::MockNullFile mock_file;
    EXPECT_CALL(mock_file, pread(_, _, _))
        .Times(AtLeast(1))
        .WillOnce(Return(-1))
        .WillOnce(Return(-1))
        .WillOnce(Return(getpagesize()))
        .WillOnce(Return(-1))
        .WillRepeatedly(Return(-1));
    IFile *aligned_unreadable = new_aligned_file_adaptor(&mock_file, getpagesize(), true, false);
    char buff[4096];
    auto ret = aligned_unreadable->pread(buff, 4096, 0);
    EXPECT_EQ(-1, ret);
    ret = aligned_unreadable->pwrite(buff, 128, 1);
    EXPECT_EQ(-1, ret);
    ret = aligned_unreadable->pwrite(buff, 128, 1);
    EXPECT_EQ(-1, ret);
}

TEST(range_split_vi, special_case) {
    uint64_t offset = 10601376;
    uint64_t len = 2256;
    vector<uint64_t> kp{0, offset, offset+len, UINT64_MAX};
    range_split split(10601376, 2256, 4096);
    ASSERT_TRUE(split.small_note);
    auto cnt = 0;
    for (auto &part : split.aligned_parts()) {
        auto iovsplit = range_split_vi(
                split.multiply(part.i, 0),
                part.length,
                &kp[0],
                kp.size()
        );
        cnt++;
        ASSERT_LT(1, cnt);
    }
}

TEST(AlignedFile, pwrite_at_tail) {
    IFileSystem *fs = new_localfs_adaptor();
    IFileSystem *afs = new_aligned_fs_adaptor(fs, 4096, true, true);
    IFile *file = afs->open("/tmp/ease_aligned_file_test.data", O_CREAT | O_TRUNC | O_RDWR, 0644);
    auto ret = file->pwrite("wtf", 3, 0);
    EXPECT_EQ(3, ret);
    delete file;
    struct stat stat;
    afs->stat("/tmp/ease_aligned_file_test.data", &stat);
    EXPECT_EQ(3, stat.st_size);
    delete afs;
}

inline static void SetupTestDir(const std::string& dir) {
  std::string cmd = std::string("rm -r ") + dir;
  system(cmd.c_str());
  cmd = std::string("mkdir -p ") + dir;
  system(cmd.c_str());
}

TEST(Walker, basic) {
  std::string root("/tmp/ease_walker");
  SetupTestDir(root);
  auto srcFs = new_localfs_adaptor(root.c_str(), ioengine_psync);
  DEFER(delete srcFs);

  for (auto file : enumerable(Walker(srcFs, ""))) {
    EXPECT_FALSE(true);
  }

  std::string file1("/testFile");
  std::system(std::string("touch " + root + file1).c_str());
  for (auto file : enumerable(Walker(srcFs, ""))) {
    EXPECT_EQ(0, strcmp(file.data(), file1.c_str()));
  }
  for (auto file : enumerable(Walker(srcFs, "/"))) {
    EXPECT_EQ(0, strcmp(file.data(), file1.c_str()));
  }

  std::string file2("/dir1/dir2/dir3/dir4/dirFile2");
  std::system(std::string("mkdir -p " + root + "/dir1/dir2/dir3/dir4/").c_str());
  std::system(std::string("touch " + root + file2).c_str());
  int count = 0;
  for (auto file : enumerable(Walker(srcFs, "/"))) {
    if (file.back() == '2') {
      EXPECT_EQ(0, strcmp(file.data(), file2.c_str()));
    } else {
      EXPECT_EQ(0, strcmp(file.data(), file1.c_str()));
    }
    count++;
  }
  EXPECT_EQ(2, count);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    photon::vcpu_init();
    photon::fd_events_init();
    DEFER({
        photon::fd_events_fini();
        photon::vcpu_fini();
    });
    int ret = RUN_ALL_TESTS();
    LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
