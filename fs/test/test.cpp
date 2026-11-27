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
#include <functional>
#include <gmock/gmock.h>
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
#include <photon/thread/workerpool.h>
#include "../../test/gtest.h"
#include "mock.h"

using namespace std;
using namespace photon;
using namespace photon::fs;

#pragma GCC diagnostic ignored "-Wsign-compare"

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
    fs::string_view a(nullptr, 0ULL);
    fs::string_view b((char*)234, 0ULL);
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
    EXPECT_EQ(node.size(), 5ull);

    for (auto x: subnodes)
        node.mkdir(x);

    EXPECT_EQ(node.size(), 9ull);

    f = F;
    void* v;
    for (auto x: items)
    {
        node.read(x, &v);
        EXPECT_EQ(v, (void*)f); f++;
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

#if __cplusplus < 202000
#define CAPTURE =
#else
#define CAPTURE =,this
#endif
class AFile : public ExampleAsyncFile
{
public:
    OVERRIDE_ASYNC(ssize_t, pread, void *buf, size_t count, off_t offset)
    {
        std::thread t([CAPTURE]()
        {
            ::sleep(1);
            callback_umimplemented<ssize_t>(done);
        });
        t.detach();
    }

    OVERRIDE_ASYNC(ssize_t, pwrite, const void *buf, size_t count, off_t offset)
    {
        LOG_DEBUG("into afile pwrite `", timeout);

        std::thread t([CAPTURE]()
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
        std::thread t([CAPTURE]()
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
    OVERRIDE_ASYNC(void, seekdir, long long loc) {
    }
    OVERRIDE_ASYNC0(long long, telldir){
    }
};


class AFS : public ExampleAsyncFileSystem {
public:
    OVERRIDE_ASYNC(IAsyncFile*, open, const char *pathname, int flags)
    {
        std::thread t([CAPTURE]()
        {
            callback(done, UINT32_MAX, exampleAfile, 0);
        });
        t.detach();
    }

    OVERRIDE_ASYNC(AsyncDIR*, opendir, const char *name)
    {
        std::thread t([CAPTURE]()
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
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
#pragma GCC diagnostic pop

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
    EXPECT_EQ(233ull, sr.begin());
    EXPECT_EQ(233ull+1024ull, sr.end());
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
    EXPECT_EQ(42ull, split.begin);
    EXPECT_EQ(363ull, split.end);
    EXPECT_EQ(1ull, split.abegin);
    EXPECT_EQ(12ull, split.aend);
    EXPECT_EQ(2ull, split.apbegin);
    EXPECT_EQ(11ull, split.apend);
    EXPECT_EQ(32ull, split.aligned_begin_offset());
    EXPECT_EQ(384ull, split.aligned_end_offset());
    auto p = split.all_parts();
    EXPECT_EQ(1ull, p.begin()->i);
    EXPECT_EQ(10ull, p.begin()->begin());
    EXPECT_EQ(32ull, p.begin()->end());
    EXPECT_EQ(12ull, p.end()->i);
    uint64_t cnt = 1;
    for (auto &rs: p) {
        EXPECT_EQ(cnt++, rs.i);
        if (rs != p.begin() && rs != p.end()) {
            EXPECT_EQ(0ull, rs.begin());
            EXPECT_EQ(32ull, rs.end());
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
    EXPECT_EQ(32ull, split.begin);
    EXPECT_EQ(353ull, split.end);
    EXPECT_EQ(1ull, split.abegin);
    EXPECT_EQ(12ull, split.aend);
    EXPECT_EQ(1ull, split.apbegin);
    EXPECT_EQ(11ull, split.apend);
    auto p = split.all_parts();
    EXPECT_FALSE(split.is_aligned());
    EXPECT_TRUE(split.is_aligned(128));
    EXPECT_TRUE(split.is_aligned_ptr((const void*)(uint64_t(65536))));
    EXPECT_EQ(1ull, p.begin()->i);
    EXPECT_EQ(0ull, p.begin()->begin());
    EXPECT_EQ(32ull, p.begin()->end());
    EXPECT_EQ(12ull, p.end()->i);
    EXPECT_EQ(352ull, split.aligned_length());
    auto q = split.aligned_parts();
    uint64_t cnt = 1;
    for (auto &rs: q) {
        EXPECT_EQ(cnt++, rs.i);
        EXPECT_EQ(0ull, rs.begin());
        EXPECT_EQ(32ull, rs.end());
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
    EXPECT_EQ(42ull, split.begin);
    EXPECT_EQ(363ull, split.end);
    EXPECT_EQ(1ull, split.abegin);
    EXPECT_EQ(12ull, split.aend);
    EXPECT_EQ(2ull, split.apbegin);
    EXPECT_EQ(11ull, split.apend);
    EXPECT_EQ(32ull, split.aligned_begin_offset());
    EXPECT_EQ(384ull, split.aligned_end_offset());
    auto p = split.all_parts();
    EXPECT_EQ(p.begin()->i, 1ull);
    EXPECT_EQ(p.begin()->begin(), 10ull);
    EXPECT_EQ(p.begin()->end(), 32ull);
    EXPECT_EQ(p.end()->i, 12ull);
    uint64_t cnt = 1;
    for (auto &rs: p) {
        EXPECT_EQ(rs.i, cnt++);
        if (rs != p.begin() && rs != p.end()) {
            EXPECT_EQ(rs.begin(), 0ull);
            EXPECT_EQ(rs.end(), 32ull);
        }
    }
}

TEST(range_split_power2, random_test) {
    uint64_t offset = rand(), length = rand();
    uint64_t interval = 1ULL<< (rand()%32 + 1);
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
    EXPECT_EQ(12ull, split.begin);
    EXPECT_EQ(333ull, split.end);
    EXPECT_TRUE(split.is_aligned(0ull));
    EXPECT_FALSE(split.is_aligned(1ull));
    EXPECT_TRUE(split.is_aligned(128ull));
    for (auto p : split.all_parts()) {
        LOG_DEBUG(p.i, ' ', p.begin(), ' ', p.end());
        EXPECT_EQ(*it == 0ull ? 12ull : 0ull, p.begin());
        EXPECT_EQ(*it == 256ull ? 321ull-256ull+12ull : (*(it+1) - *it), p.end());
        it++;
    }
    uint64_t kpfail[] = {0, 32, 796, 128, 256, 512, UINT64_MAX};
    EXPECT_FALSE(split.ascending(kpfail, 7));
#ifndef NDEBUG
    EXPECT_DEATH(
        not_ascend_death(),
        ".*range-split-vi.h.*"
    );
#endif
}

TEST(range_split_vi, left_side_aligned) {
    uint64_t kp[] = {0, 32, 64, 128, 256, 512, UINT64_MAX};
    fs::range_split_vi split(0, 256, kp, 7);
    uint64_t *it = kp;
    EXPECT_EQ(0ull, split.begin);
    EXPECT_EQ(256ull, split.end);
    EXPECT_TRUE(split.is_aligned(0ull));
    EXPECT_FALSE(split.is_aligned(1ull));
    EXPECT_TRUE(split.is_aligned(128ull));
    for (auto p : split.all_parts()) {
        LOG_DEBUG(p.i, ' ', p.begin(), ' ', p.end());
        EXPECT_EQ(0ull, p.begin());
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

#ifdef __linux__
TEST(LocalFileSystem, aio_seek_data_hole) {
    const off_t bs = 4096;
    const char* path = "/tmp/photon_seek_data_hole";
    // Build a sparse file with a plain fd:
    //   data[0, bs)  hole[bs, 2*bs)  data[2*bs, 3*bs)
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0);
    DEFER(::unlink(path));
    std::unique_ptr<char[]> block(new char[bs]);
    memset(block.get(), 'A', bs);
    ASSERT_EQ(bs, ::pwrite(fd, block.get(), bs, 0));
    ASSERT_EQ(bs, ::pwrite(fd, block.get(), bs, 2 * bs));
    ::fsync(fd);

    // Wrap the fd in an AioFileAdaptor (posixaio).
    std::unique_ptr<IFile> f(new_localfile_adaptor(fd, ioengine_posixaio));
    ASSERT_NE(nullptr, f.get());

    // SEEK_DATA / SEEK_HOLE reach the kernel and sync the self-managed offset.
    off_t d = f->lseek(0, SEEK_DATA);
    EXPECT_EQ(0, d);
    EXPECT_EQ(d, f->lseek(0, SEEK_CUR));

    off_t h = f->lseek(0, SEEK_HOLE);
    EXPECT_EQ(bs, h);
    EXPECT_EQ(h, f->lseek(0, SEEK_CUR));

    EXPECT_EQ(3 * bs, f->lseek(2 * bs, SEEK_HOLE));

    // Plain whence values still follow the VirtualFile (m_offset) route.
    EXPECT_EQ(0, f->lseek(0, SEEK_SET));
    EXPECT_EQ(bs, f->lseek(bs, SEEK_CUR));
    EXPECT_EQ(3 * bs, f->lseek(0, SEEK_END));
}
#endif // __linux__

std::unique_ptr<char[]> random_block(uint64_t size) {
    std::unique_ptr<char[]> buff(new char[size]);
    char * p = buff.get();
    while (size--) {
        *(p++) = rand() % UCHAR_MAX;
    }
    return buff;
}

void random_content_rw_test(uint64_t test_block_size, uint64_t test_block_num, fs::IFile* file) {
    vector<std::unique_ptr<char[]>> rand_data;
    file->lseek(0, SEEK_SET);
    for (uint64_t i = 0; i < test_block_num; i++) {
        rand_data.emplace_back(random_block(test_block_size));
        char * buff = rand_data.back().get();
        file->write(buff, test_block_size);
    }
    file->fsync();
    file->lseek(0, SEEK_SET);
    auto buff = malloc(test_block_size);
    DEFER(free(buff));
    for (const auto &data : rand_data) {
        file->read(buff, test_block_size);
        EXPECT_EQ(0, memcmp(data.get(), buff, test_block_size));
    }
}

void sequence_content_rw_test (uint64_t test_block_size, uint64_t test_block_num, const char* test_seq, fs::IFile* file) {
    auto data = malloc(test_block_size);
    DEFER(free(data));
    file->lseek(0, SEEK_SET);
    for (auto i: xrange(test_block_num)) {
        memset(data, test_seq[i], test_block_size);
        file->write(data, test_block_size);
    }
    file->fdatasync();
    file->lseek(0, SEEK_SET);
    auto buff = malloc(test_block_size);
    DEFER(free(buff));
    for (uint64_t i = 0; i< test_block_num; i++) {
        file->read(buff, test_block_size);
        memset(data, *(test_seq++), test_block_size);
        EXPECT_EQ(0, memcmp(data, buff, test_block_size));
    }
}

void xfile_fstat_test(uint64_t fsize, fs::IFile* file) {
    struct stat st;
    file->fstat(&st);
    EXPECT_EQ(fsize, (uint64_t)st.st_size);
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
    for (auto i: xrange(test_file_num)) {
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
    auto buff = malloc(test_block_size);
    DEFER(free(buff));
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
    for (auto i: xrange(test_file_num)) {
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
    for (auto i: xrange(test_file_num)) {
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
    auto buff = malloc(test_block_size);
    DEFER(free(buff));
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

TEST(AlignedFile, basic) {
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

TEST(AlignedFile, invalid_alignment) {
    log_output = log_output_null;
    DEFER({
        log_output = log_output_stdout;
    });

    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);
    IFile* underlay_file = fs->open("test_aligned_file_aligned", O_RDWR | O_CREAT, 0666);
    ASSERT_NE(nullptr, underlay_file);
    DEFER(delete underlay_file);

    EXPECT_EQ(nullptr, new_aligned_file_adaptor(underlay_file, getpagesize() - 1, true, false));
    EXPECT_EQ(EINVAL, errno);
}

TEST(AlignedFile, read_failure) {
    log_output = log_output_null;
    DEFER({
        log_output = log_output_stdout;
    });
    using namespace testing;

    PMock::MockNullFile mock_file;
    struct stat mock_stat = {};
    ON_CALL(mock_file, fstat(_)).WillByDefault(DoAll(SetArgPointee<0>(mock_stat), Return(0)));
    EXPECT_CALL(mock_file, pread(_, _, _))
        .Times(AtLeast(1))
        .WillOnce(Return(-1))
        .WillOnce(Return(-1))
        .WillOnce(Return(getpagesize()))
        .WillOnce(Return(-1))
        .WillRepeatedly(Return(-1));
    IFile* aligned_unreadable = new_aligned_file_adaptor(&mock_file, getpagesize(), true, false);
    ASSERT_NE(nullptr, aligned_unreadable);
    DEFER(delete aligned_unreadable);

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
        (void)iovsplit;
        cnt++;
        ASSERT_LT(1, cnt);
    }
}

TEST(AlignedFile, pwrite_at_tail) {
    IFileSystem* fs = new_localfs_adaptor();
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);
    IFile* raw = fs->open("/tmp/ease_aligned_file_test.data", O_CREAT | O_TRUNC | O_RDWR, 0644);
    ASSERT_NE(nullptr, raw);
    DEFER(delete raw);
    IFile* file = new_aligned_file_adaptor(raw, 4096, true, false);
    ASSERT_NE(nullptr, file);
    DEFER(delete file);

    auto ret = file->pwrite("wtf", 3, 0);
    EXPECT_EQ(3, ret);
    struct stat stat = {};
    ASSERT_EQ(0, raw->fstat(&stat));
    EXPECT_EQ(3, stat.st_size);
}

// yields before every IO, so concurrent writers interleave deterministically
class YieldingFile : public ForwardFile {
public:
    YieldingFile(IFile* file, bool ownership) : ForwardFile(file), m_ownership(ownership) { }
    ~YieldingFile() override
    {
        if (m_ownership) delete m_file;
    }
    ssize_t pread(void* buf, size_t count, off_t offset) override
    {
        photon::thread_usleep(1000);
        return m_file->pread(buf, count, offset);
    }
    ssize_t pwrite(const void* buf, size_t count, off_t offset) override
    {
        photon::thread_usleep(1000);
        return m_file->pwrite(buf, count, offset);
    }
    int fstat(struct stat* buf) override
    {
        photon::thread_usleep(1000);
        return m_file->fstat(buf);
    }
    int ftruncate(off_t length) override
    {
        photon::thread_usleep(1000);
        return m_file->ftruncate(length);
    }
private:
    bool m_ownership;
};

class YieldingFS : public ForwardFS {
public:
    explicit YieldingFS(IFileSystem* fs) : ForwardFS(fs) { }
    IFile* open(const char* path, int flags, mode_t mode) override
    {
        auto file = m_fs->open(path, flags, mode);
        return file ? new YieldingFile(file, true) : nullptr;
    }
    IFile* open(const char* path, int flags) override
    {
        auto file = m_fs->open(path, flags);
        return file ? new YieldingFile(file, true) : nullptr;
    }
};

TEST(AlignedFile, concurrent_pwrite_same_block) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);
    IFile* raw = fs->open("test_aligned_concurrent_block", O_RDWR | O_CREAT | O_TRUNC, 0666);
    ASSERT_NE(nullptr, raw);
    DEFER(delete raw);
    ASSERT_EQ(0, raw->ftruncate(65536));

    IFile* target = new_aligned_file_adaptor(new YieldingFile(raw, false), 4096, true, true);
    ASSERT_NE(nullptr, target);
    DEFER(delete target);

    auto write_at = [&](off_t offset, char c) {
        std::string buf(512, c);
        EXPECT_EQ(512, target->pwrite(buf.data(), buf.size(), offset));
    };
    photon::semaphore done(0);
    photon::WorkPool pool(4, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 0);
    pool.async_call(new auto([&] { write_at(0, 'a'); done.signal(1); }));
    pool.async_call(new auto([&] { write_at(1024, 'b'); done.signal(1); }));
    ASSERT_EQ(0, done.wait(2, 30ULL * 1000 * 1000));

    char buf[2048] = {};
    ASSERT_EQ(2048, raw->pread(buf, sizeof(buf), 0));
    for (int i = 0; i < 512; i++) EXPECT_EQ('a', buf[i]);
    for (int i = 512; i < 1024; i++) EXPECT_EQ(0, buf[i]);
    for (int i = 1024; i < 1536; i++) EXPECT_EQ('b', buf[i]);
    for (int i = 1536; i < 2048; i++) EXPECT_EQ(0, buf[i]);
}

TEST(AlignedFileSystem, concurrent_pwrite_through_two_handles) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);
    IFileSystem* afs = new_aligned_fs_adaptor(new YieldingFS(fs), 4096, true, true);
    ASSERT_NE(nullptr, afs);
    DEFER(delete afs);

    IFile* f1 = afs->open("test_aligned_concurrent_handles", O_RDWR | O_CREAT | O_TRUNC, 0666);
    ASSERT_NE(nullptr, f1);
    DEFER(delete f1);
    ASSERT_EQ(0, f1->ftruncate(65536));
    IFile* f2 = afs->open("test_aligned_concurrent_handles", O_RDWR);
    ASSERT_NE(nullptr, f2);
    DEFER(delete f2);

    auto write_at = [](IFile* file, off_t offset, char c) {
        std::string buf(512, c);
        EXPECT_EQ(512, file->pwrite(buf.data(), buf.size(), offset));
    };
    photon::semaphore done(0);
    photon::WorkPool pool(4, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 0);
    pool.async_call(new auto([&] { write_at(f1, 0, 'c'); done.signal(1); }));
    pool.async_call(new auto([&] { write_at(f2, 1024, 'd'); done.signal(1); }));
    ASSERT_EQ(0, done.wait(2, 30ULL * 1000 * 1000));

    char buf[2048] = {};
    ASSERT_EQ(2048, f1->pread(buf, sizeof(buf), 0));
    for (int i = 0; i < 512; i++) EXPECT_EQ('c', buf[i]);
    for (int i = 1024; i < 1536; i++) EXPECT_EQ('d', buf[i]);
}

TEST(AlignedFile, concurrent_unaligned_extend) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);
    IFile* raw = fs->open("test_aligned_concurrent_extend", O_RDWR | O_CREAT | O_TRUNC, 0666);
    ASSERT_NE(nullptr, raw);
    DEFER(delete raw);
    IFile* target = new_aligned_file_adaptor(new YieldingFile(raw, false), 4096, true, true);
    ASSERT_NE(nullptr, target);
    DEFER(delete target);

    auto write_at = [&](off_t offset, char c) {
        std::string buf(100, c);
        EXPECT_EQ(100, target->pwrite(buf.data(), buf.size(), offset));
    };
    photon::semaphore done(0);
    photon::WorkPool pool(4, photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE, 0);
    pool.async_call(new auto([&] { write_at(0, 'e'); done.signal(1); }));
    pool.async_call(new auto([&] { write_at(8192, 'f'); done.signal(1); }));
    ASSERT_EQ(0, done.wait(2, 30ULL * 1000 * 1000));

    char buf[100];
    ASSERT_EQ(100, raw->pread(buf, sizeof(buf), 0));
    for (int i = 0; i < 100; i++) EXPECT_EQ('e', buf[i]);
    ASSERT_EQ(100, raw->pread(buf, sizeof(buf), 8192));
    for (int i = 0; i < 100; i++) EXPECT_EQ('f', buf[i]);
    struct stat st;
    ASSERT_EQ(0, raw->fstat(&st));
    EXPECT_EQ(8292, st.st_size);
}

TEST(AlignedFileSystem, lock_entry_released_with_last_handle) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);
    auto afs = new AlignedFSAdaptor(fs, 4096, true, false, nullptr);
    ASSERT_NE(nullptr, afs);
    DEFER(delete afs);

    auto f1 = static_cast<AlignedFileAdaptor*>(
        afs->open("test_aligned_lock_release", O_RDWR | O_CREAT | O_TRUNC, 0666));
    ASSERT_NE(nullptr, f1);
    auto f2 = static_cast<AlignedFileAdaptor*>(
        afs->open("test_aligned_lock_release", O_RDWR));
    ASSERT_NE(nullptr, f2);
    EXPECT_EQ(&f1->m_lock->range(), &f2->m_lock->range());
    EXPECT_EQ(1u, afs->m_lock_table->m_locks.size());

    delete f1;
    EXPECT_EQ(1u, afs->m_lock_table->m_locks.size());
    delete f2;
    EXPECT_EQ(0u, afs->m_lock_table->m_locks.size());
}

TEST(AlignedFileSystem, handle_outlives_fs_adaptor) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);

    IFile* f = nullptr;
    {
        auto afs = new AlignedFSAdaptor(fs, 4096, true, false, nullptr);
        ASSERT_NE(nullptr, afs);
        f = afs->open("test_aligned_handle_outlives_fs", O_RDWR | O_CREAT | O_TRUNC, 0666);
        ASSERT_NE(nullptr, f);
        delete afs;
    }

    // the handle keeps the lock table alive after the fs adaptor is gone
    std::string buf(100, 'x');
    EXPECT_EQ(100, f->pwrite(buf.data(), buf.size(), 0));
    delete f;
}

// test doubles

static bool wait_for(photon::semaphore& sem, uint64_t us = 5ULL * 1000 * 1000)
{
    return sem.wait(1, us) == 0;
}

// pauses after the first pread that matches the watched path, so a truncating
// operation can be attempted while the write holds its range lock
class PausingFile : public ForwardFile_Ownership {
public:
    PausingFile(IFile* file, photon::semaphore& reached, photon::semaphore& resume,
                bool& armed, bool pausing)
        : ForwardFile_Ownership(file, true), m_reached(reached), m_resume(resume),
          m_armed(armed), m_pausing(pausing) { }
    ssize_t pread(void* buf, size_t count, off_t offset) override
    {
        auto ret = m_file->pread(buf, count, offset);
        if (m_pausing && m_armed) {
            m_armed = false;
            m_reached.signal(1);
            m_resume.wait(1, 5ULL * 1000 * 1000);
        }
        return ret;
    }
protected:
    photon::semaphore& m_reached;
    photon::semaphore& m_resume;
    bool& m_armed;
    bool m_pausing;
};

class PausingFS : public ForwardFS_Ownership {
public:
    photon::semaphore reached{0}, resume{0};
    bool armed = false;
    std::string target;     // empty matches every path
    explicit PausingFS(IFileSystem* fs) : ForwardFS_Ownership(fs, true) { }
    IFile* open(const char* path, int flags, mode_t mode) override
    {
        auto file = m_fs->open(path, flags, mode);
        return file ? wrap(path, file) : nullptr;
    }
    IFile* open(const char* path, int flags) override
    {
        auto file = m_fs->open(path, flags);
        return file ? wrap(path, file) : nullptr;
    }
protected:
    IFile* wrap(const char* path, IFile* file)
    {
        bool pausing = target.empty() || target == path;
        return new PausingFile(file, reached, resume, armed, pausing);
    }
};

class FailingFile : public ForwardFile_Ownership {
public:
    enum Fault { NONE, FSTAT, PWRITE, FTRUNCATE };
    Fault fault = NONE;
    explicit FailingFile(IFile* file) : ForwardFile_Ownership(file, true) { }
    int fstat(struct stat* buf) override
    {
        if (fault == FSTAT)
            LOG_ERROR_RETURN(EIO, -1, "injected fstat failure");
        return m_file->fstat(buf);
    }
    ssize_t pwrite(const void* buf, size_t count, off_t offset) override
    {
        if (fault == PWRITE)
            LOG_ERROR_RETURN(EIO, -1, "injected pwrite failure");
        return m_file->pwrite(buf, count, offset);
    }
    int ftruncate(off_t length) override
    {
        if (fault == FTRUNCATE)
            LOG_ERROR_RETURN(EIO, -1, "injected ftruncate failure");
        return m_file->ftruncate(length);
    }
};

// fixtures

class AlignedFileSystemTruncation : public testing::Test {
public:
    const char* path = "test_aligned_truncation.data";
    IFileSystem* fs = nullptr;
    PausingFS* gate = nullptr;
    IFileSystem* afs = nullptr;
    IFile* f = nullptr;

    void SetUp() override
    {
        fs = new_localfs_adaptor("/tmp/");
        ASSERT_NE(nullptr, fs);
        gate = new PausingFS(new YieldingFS(fs));
        afs = new_aligned_fs_adaptor(gate, 4096, false, true, nullptr);
        ASSERT_NE(nullptr, afs);
        f = afs->open(path, O_RDWR | O_CREAT, 0666);
        ASSERT_NE(nullptr, f);
        ASSERT_EQ(0, f->ftruncate(65536));
    }

    void TearDown() override
    {
        delete f;
        delete afs;
        delete fs;
    }

    // starts an unaligned write, runs `op` while it is parked inside the
    // read-modify-write, and expects `op` to complete only after the write
    void expect_waits_for_inflight_write(const std::function<void()>& op, off_t expected_size = 0)
    {
        gate->target = path;
        gate->armed = true;
        std::string buf(100, 'a');
        auto w = photon::thread_enable_join(photon::thread_create11([&] {
            EXPECT_EQ(100, f->pwrite(buf.data(), buf.size(), 1));
        }));
        ASSERT_TRUE(wait_for(gate->reached));

        bool done = false;
        photon::semaphore started(0);
        auto o = photon::thread_enable_join(photon::thread_create11([&] {
            started.signal(1);
            op();
            done = true;
        }));
        ASSERT_TRUE(wait_for(started));
        photon::thread_usleep(1000);
        EXPECT_FALSE(done) << "the truncating operation must wait for the write";

        gate->resume.signal(1);
        photon::thread_join(w);
        photon::thread_join(o);
        EXPECT_TRUE(done);

        struct stat st = {};
        ASSERT_EQ(0, f->fstat(&st));
        EXPECT_EQ(expected_size, st.st_size);
    }
};

TEST_F(AlignedFileSystemTruncation, ftruncate_waits_for_inflight_write)
{
    expect_waits_for_inflight_write([&] {
        EXPECT_EQ(0, f->ftruncate(0));
    });
}

TEST_F(AlignedFileSystemTruncation, path_truncate_waits_for_inflight_write)
{
    expect_waits_for_inflight_write([&] {
        EXPECT_EQ(0, afs->truncate(path, 0));
    });
}

TEST_F(AlignedFileSystemTruncation, fallocate_waits_for_inflight_write)
{
    expect_waits_for_inflight_write([&] {
        EXPECT_EQ(0, f->fallocate(0, 0, 131072));
    }, 131072);
}

TEST(AlignedFileSystem, aliases_get_separate_locks) {
    // locks are keyed by path, so an alias of the same file is not serialized
    // against the original path; this documents the known limitation
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);
    IFileSystem* afs = new_aligned_fs_adaptor(fs, 4096, false, false, nullptr);
    ASSERT_NE(nullptr, afs);
    DEFER(delete afs);

    const char* path = "test_aligned_alias.data";
    const char* dot = "./test_aligned_alias.data";
    IFile* f1 = afs->open(path, O_RDWR | O_CREAT, 0666);
    ASSERT_NE(nullptr, f1);
    DEFER(delete f1);
    IFile* f2 = afs->open(dot, O_RDWR);
    ASSERT_NE(nullptr, f2);
    DEFER(delete f2);
    EXPECT_NE(&static_cast<AlignedFileAdaptor*>(f1)->m_lock->range(),
              &static_cast<AlignedFileAdaptor*>(f2)->m_lock->range());
}

TEST(AlignedFile, write_errors_are_reported) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);
    IFile* raw = fs->open("test_aligned_errors.data", O_RDWR | O_CREAT, 0666);
    ASSERT_NE(nullptr, raw);
    ASSERT_EQ(0, raw->ftruncate(0));
    ASSERT_EQ(0, raw->ftruncate(65536));
    FailingFile* faulty = new FailingFile(raw);
    IFile* f = new_aligned_file_adaptor(faulty, 4096, false, true);
    ASSERT_NE(nullptr, f);
    DEFER(delete f);
    char buf[100] = {};

    faulty->fault = FailingFile::FSTAT;
    errno = 0;
    EXPECT_EQ(-1, f->pwrite(buf, sizeof(buf), 1));
    EXPECT_EQ(EIO, errno);

    faulty->fault = FailingFile::PWRITE;
    errno = 0;
    EXPECT_EQ(-1, f->pwrite(buf, sizeof(buf), 1));
    EXPECT_EQ(EIO, errno);

    faulty->fault = FailingFile::FTRUNCATE;
    ASSERT_EQ(0, raw->ftruncate(0));
    errno = 0;
    EXPECT_EQ(-1, f->pwrite(buf, sizeof(buf), 1));
    EXPECT_EQ(EIO, errno);

    faulty->fault = FailingFile::NONE;
    EXPECT_EQ(100, f->pwrite(buf, sizeof(buf), 1));
}

TEST(AlignedFile, pread_is_not_blocked_by_write) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    PausingFS gate(fs);
    const char* path = "test_aligned_pread_unblocked.data";
    gate.target = path;
    IFile* raw = gate.open(path, O_RDWR | O_CREAT, 0666);
    ASSERT_NE(nullptr, raw);
    ASSERT_EQ(0, raw->ftruncate(65536));
    IFile* f = new_aligned_file_adaptor(raw, 4096, false, true);
    ASSERT_NE(nullptr, f);
    DEFER(delete f);

    gate.armed = true;
    std::string wbuf(100, 'a');
    auto w = photon::thread_enable_join(photon::thread_create11([&] {
        EXPECT_EQ(100, f->pwrite(wbuf.data(), wbuf.size(), 1));
    }));
    ASSERT_TRUE(wait_for(gate.reached));

    bool read_done = false;
    char rbuf[100] = {};
    auto r = photon::thread_enable_join(photon::thread_create11([&] {
        EXPECT_EQ(100, f->pread(rbuf, sizeof(rbuf), 1));
        read_done = true;
    }));
    for (int i = 0; i < 2000 && !read_done; i++)
        photon::thread_usleep(1000);
    EXPECT_TRUE(read_done) << "pread must not wait for the write lock";

    gate.resume.signal(1);
    photon::thread_join(w);
    photon::thread_join(r);
}

TEST(AlignedFile, vector_write_waits_for_scalar_write) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    PausingFS gate(fs);
    const char* path = "test_aligned_vector_vs_scalar.data";
    gate.target = path;
    IFile* raw = gate.open(path, O_RDWR | O_CREAT, 0666);
    ASSERT_NE(nullptr, raw);
    ASSERT_EQ(0, raw->ftruncate(65536));
    IFile* f = new_aligned_file_adaptor(raw, 4096, false, true);
    ASSERT_NE(nullptr, f);
    DEFER(delete f);

    gate.armed = true;
    std::string wbuf(100, 'a');
    auto w = photon::thread_enable_join(photon::thread_create11([&] {
        EXPECT_EQ(100, f->pwrite(wbuf.data(), wbuf.size(), 1));
    }));
    ASSERT_TRUE(wait_for(gate.reached));

    bool vec_done = false;
    photon::semaphore vec_started(0);
    std::string vbuf(100, 'b');
    iovec iov[2] = {{&vbuf[0], 50}, {&vbuf[0] + 50, 50}};
    auto v = photon::thread_enable_join(photon::thread_create11([&] {
        vec_started.signal(1);
        EXPECT_EQ(100, f->pwritev(iov, 2, 200));
        vec_done = true;
    }));
    ASSERT_TRUE(wait_for(vec_started));
    photon::thread_usleep(1000);
    EXPECT_FALSE(vec_done) << "pwritev must wait for a conflicting scalar write";

    gate.resume.signal(1);
    photon::thread_join(w);
    photon::thread_join(v);
    EXPECT_TRUE(vec_done);

    char buf[100] = {};
    ASSERT_EQ(100, f->pread(buf, sizeof(buf), 1));
    for (int i = 0; i < 100; i++) EXPECT_EQ('a', buf[i]);
    ASSERT_EQ(100, f->pread(buf, sizeof(buf), 200));
    for (int i = 0; i < 100; i++) EXPECT_EQ('b', buf[i]);
}

TEST(AlignedFile, lock_range_allows_adjacent_write) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    PausingFS gate(fs);
    const char* path = "test_aligned_lock_range.data";
    gate.target = path;
    IFile* raw = gate.open(path, O_RDWR | O_CREAT, 0666);
    ASSERT_NE(nullptr, raw);
    ASSERT_EQ(0, raw->ftruncate(65536));
    IFile* f = new_aligned_file_adaptor(raw, 4096, false, true);
    ASSERT_NE(nullptr, f);
    DEFER(delete f);

    // ends on a block boundary, so the lock covers [0, 4096) only
    gate.armed = true;
    std::string abuf(4095, 'a');
    auto a = photon::thread_enable_join(photon::thread_create11([&] {
        EXPECT_EQ(4095, f->pwrite(abuf.data(), abuf.size(), 1));
    }));
    ASSERT_TRUE(wait_for(gate.reached));

    bool adjacent_done = false;
    photon::semaphore adjacent_started(0);
    std::string adjacent(4096, 'b');
    auto t1 = photon::thread_enable_join(photon::thread_create11([&] {
        adjacent_started.signal(1);
        EXPECT_EQ(4096, f->pwrite(adjacent.data(), adjacent.size(), 4096));
        adjacent_done = true;
    }));
    ASSERT_TRUE(wait_for(adjacent_started));
    for (int i = 0; i < 2000 && !adjacent_done; i++)
        photon::thread_usleep(1000);
    EXPECT_TRUE(adjacent_done) << "an adjacent aligned write must not wait";

    bool overlap_done = false;
    photon::semaphore overlap_started(0);
    std::string overlap(4096, 'c');
    auto t2 = photon::thread_enable_join(photon::thread_create11([&] {
        overlap_started.signal(1);
        EXPECT_EQ(4096, f->pwrite(overlap.data(), overlap.size(), 0));
        overlap_done = true;
    }));
    ASSERT_TRUE(wait_for(overlap_started));
    photon::thread_usleep(1000);
    EXPECT_FALSE(overlap_done) << "an overlapping aligned write must wait";

    gate.resume.signal(1);
    photon::thread_join(a);
    photon::thread_join(t1);
    photon::thread_join(t2);

    char buf[4096] = {};
    ASSERT_EQ(4096, f->pread(buf, sizeof(buf), 4096));
    for (int i = 0; i < 4096; i++) EXPECT_EQ('b', buf[i]);
    ASSERT_EQ(4096, f->pread(buf, sizeof(buf), 0));
    for (int i = 0; i < 4096; i++) EXPECT_EQ('c', buf[i]);
}

TEST(AlignedFileSystem, different_files_write_concurrently) {
    IFileSystem* fs = new_localfs_adaptor("/tmp/");
    ASSERT_NE(nullptr, fs);
    DEFER(delete fs);
    PausingFS* gate = new PausingFS(new YieldingFS(fs));
    DEFER(delete gate);
    IFileSystem* afs = new_aligned_fs_adaptor(gate, 4096, false, false, nullptr);
    ASSERT_NE(nullptr, afs);
    DEFER(delete afs);

    IFile* a = afs->open("test_aligned_conc_a", O_RDWR | O_CREAT, 0666);
    ASSERT_NE(nullptr, a);
    DEFER(delete a);
    IFile* b = afs->open("test_aligned_conc_b", O_RDWR | O_CREAT, 0666);
    ASSERT_NE(nullptr, b);
    DEFER(delete b);
    ASSERT_EQ(0, a->ftruncate(65536));
    ASSERT_EQ(0, b->ftruncate(65536));
    EXPECT_NE(&static_cast<AlignedFileAdaptor*>(a)->m_lock->range(),
              &static_cast<AlignedFileAdaptor*>(b)->m_lock->range());

    gate->target = "test_aligned_conc_a";
    gate->armed = true;
    std::string sa(100, 'a'), sb(100, 'b');
    auto t1 = photon::thread_enable_join(photon::thread_create11([&] {
        EXPECT_EQ(100, a->pwrite(sa.data(), sa.size(), 1));
    }));
    ASSERT_TRUE(wait_for(gate->reached));

    bool b_done = false;
    photon::semaphore b_started(0);
    auto t2 = photon::thread_enable_join(photon::thread_create11([&] {
        b_started.signal(1);
        EXPECT_EQ(100, b->pwrite(sb.data(), sb.size(), 1));
        b_done = true;
    }));
    ASSERT_TRUE(wait_for(b_started));
    for (int i = 0; i < 2000 && !b_done; i++)
        photon::thread_usleep(1000);
    EXPECT_TRUE(b_done) << "writes to different files must not serialize";

    gate->resume.signal(1);
    photon::thread_join(t1);
    photon::thread_join(t2);
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
    (void)file;
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
    LOG_INFO(VALUE(file.data()));
    if (file.back() == '2') {
      EXPECT_EQ(0, strcmp(file.data(), file2.c_str()));
    } else {
      EXPECT_EQ(0, strcmp(file.data(), file1.c_str()));
    }
    count++;
  }
  EXPECT_EQ(2, count);
}

int main(int argc, char **argv){
    srand(time(nullptr));
    ::testing::InitGoogleTest(&argc, argv);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());
    return RUN_ALL_TESTS();
}
