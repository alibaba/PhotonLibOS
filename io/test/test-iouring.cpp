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

#include <sys/time.h>
#include <cstdlib>
#include <fcntl.h>
#include <unordered_map>

#include <gtest/gtest.h>
#include <gflags/gflags.h>

#include <photon/io/fd-events.h>
#include <photon/io/aio-wrapper.h>
#include <photon/io/signalfd.h>
#include <photon/fs/localfs.h>
#include <photon/fs/filesystem.h>
#include <photon/common/checksum/crc32c.h>
#include <photon/common/io-alloc.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>


using namespace photon;

// Common parameters
bool stop_test = false;
uint64_t qps = 0;
DEFINE_uint64(show_loop_interval, 10, "interval seconds of show loop");
DEFINE_uint64(num_threads, 32, "num of threads");
const size_t max_io_size = 2 * 1024 * 1024;
DEFINE_string(src, "src", "src file path");
DEFINE_string(dst, "dst", "dst file path");
DEFINE_uint64(buf_size, 4096, "buffer size");

/* Helper functions */

#define ROUND_DOWN(N, S) ((N) & ~((S) - 1))

static void handle_signal(int sig) {
    LOG_INFO("try to stop test");
    stop_test = true;
}

static void ignore_signal(int sig) {
    LOG_INFO("ignore signal `", sig);
}

static void show_qps_loop() {
    while (!stop_test) {
        photon::thread_sleep(FLAGS_show_loop_interval);
        LOG_INFO("qps: `", qps / FLAGS_show_loop_interval);
        qps = 0;
    }
}

/* IO Tests */

enum class IOTestType {
    RAND_READ,
    RAND_WRITE,
    RAND_COPY,
    RAND_WRITE_PERF,
    RAND_READ_PERF,
};

static uint64_t random64() {
    uint64_t r1 = random();
    uint64_t r2 = random();
    return (r1 << 32) + r2;
}

static void read_integrity(const off_t max_offset, fs::IFile* src_file, fs::IFile* dst_file,
                           IOAlloc* io_alloc) {
    void* buf_src = io_alloc->alloc(max_io_size);
    DEFER(io_alloc->dealloc(buf_src));
    void* buf_dst = io_alloc->alloc(max_io_size);
    DEFER(io_alloc->dealloc(buf_dst));

    while (!stop_test) {
        size_t count = random64() % max_io_size + 1;
        off_t offset = random64() % max_offset;

        int ret = src_file->pread(buf_src, count, offset);
        if (ret != (int) count) {
            LOG_ERROR("iouring read fail, count `, offset `, ret `, errno `", count, offset, ret, ERRNO());
            FAIL();
        }

        ret = dst_file->pread(buf_dst, count, offset);
        if (ret != (int) count) {
            LOG_ERROR("psync read fail, count `, offset `, ret `, errno `", count, offset, ret, ERRNO());
            FAIL();
        }

        auto crc_src = crc32c(buf_src, count);
        auto crc_dst = crc32c(buf_dst, count);
        if (crc_src != crc_dst) {
            FAIL() << "crc mismatch";
        }
        qps++;
    }
}

static void write_integrity(const off_t max_offset, fs::IFile* src_file, fs::IFile* dst_file,
                            IOAlloc* io_alloc) {
    void* buf = io_alloc->alloc(max_io_size);
    DEFER(io_alloc->dealloc(buf));

    auto rand_file = fs::open_localfile_adaptor("/dev/urandom", O_RDONLY);
    DEFER(delete rand_file);

    while (!stop_test) {
        size_t count = random64() % max_io_size + 1;
        off_t offset = random64() % max_offset;
        if (rand_file->read(buf, count) != (int) count) {
            FAIL();
        }
        if (src_file->pwrite(buf, count, offset) != (int) count) {
            FAIL();
        }
        if (dst_file->pwrite(buf, count, offset) != (int) count) {
            FAIL();
        }
        qps++;
    }
}

static void copy_integrity(const off_t max_offset, fs::IFile* src_file, fs::IFile* dst_file,
                           IOAlloc* io_alloc) {
    void* buf = io_alloc->alloc(max_io_size);
    DEFER(io_alloc->dealloc(buf));

    while (!stop_test) {
        size_t count = 4096;
        off_t offset = random64() % max_offset;
        int ret = src_file->pread(buf, count, offset);
        if (ret != (int) count) {
            LOG_ERROR("read fail, count `, offset `, ret `, errno `", count, offset, ret, ERRNO());
            FAIL();
        }
        if (dst_file->pwrite(buf, count, offset) != (int) count) {
            LOG_ERROR("write fail, count `, offset `, ret `, errno `", count, offset, ret, ERRNO());
            FAIL();
        }
        qps++;
    }
}

static void write_perf(const off_t max_offset, fs::IFile* src_file, IOAlloc* io_alloc) {
    void* buf = io_alloc->alloc(max_io_size);
    DEFER(io_alloc->dealloc(buf));

    while (!stop_test) {
        size_t count = FLAGS_buf_size;
        off_t offset = ROUND_DOWN(random64() % max_offset, count);
        int ret = src_file->pwrite(buf, count, offset);
        if (ret != (int) count) {
            LOG_ERROR("write fail, count `, offset `, ret `, errno `", count, offset, ret, ERRNO());
            FAIL();
        }
        qps++;
    }
}

static void read_perf(const off_t max_offset, fs::IFile* src_file, IOAlloc* io_alloc) {
    void* buf = io_alloc->alloc(max_io_size);
    DEFER(io_alloc->dealloc(buf));

    while (!stop_test) {
        size_t count = FLAGS_buf_size;
        off_t offset = ROUND_DOWN(random64() % max_offset, count);
        int ret = src_file->pread(buf, count, offset);
        if (ret != (int) count) {
            LOG_ERROR("read fail, count `, offset `, ret `, errno `", count, offset, ret, ERRNO());
            FAIL();
        }
        qps++;
    }
}

static void do_io_test(IOTestType type) {
    ASSERT_EQ(photon::sync_signal_init(), 0);
    DEFER(photon::sync_signal_fini());
    photon::sync_signal(SIGTERM, &handle_signal);
    photon::sync_signal(SIGINT, &handle_signal);

    ASSERT_EQ(photon::libaio_wrapper_init(), 0);
    DEFER(photon::libaio_wrapper_fini());

    photon::thread_create11(show_qps_loop);

    int flags = O_RDWR;
    if (type == IOTestType::RAND_READ || type == IOTestType::RAND_COPY || type == IOTestType::RAND_READ_PERF) {
        flags = O_RDONLY;
    }
    auto src_file = fs::open_localfile_adaptor(FLAGS_src.c_str(), flags, 0644, fs::ioengine_iouring);
    ASSERT_NE(src_file, nullptr);
    DEFER(delete src_file);

    auto dst_engine = (type == IOTestType::RAND_COPY) ? fs::ioengine_iouring : fs::ioengine_psync;
    fs::IFile* dst_file = nullptr;
    if (type != IOTestType::RAND_READ_PERF && type != IOTestType::RAND_WRITE_PERF) {
        dst_file = fs::open_localfile_adaptor(FLAGS_dst.c_str(), O_RDWR, 0644, dst_engine);
        ASSERT_NE(dst_file, nullptr);
    }
    DEFER(delete dst_file);

    struct stat st_buf{};
    ASSERT_EQ(src_file->fstat(&st_buf), 0);

    AlignedAlloc io_alloc(4096);
    off_t max_offset = st_buf.st_size - max_io_size;
    ASSERT_GT(max_offset, 0);

    std::vector<photon::thread*> join_threads;
    for (uint64_t i = 0; i < FLAGS_num_threads; i++) {
        photon::thread* th;
        switch (type) {
            case IOTestType::RAND_READ:
                th = photon::thread_create11(read_integrity, max_offset, src_file, dst_file, &io_alloc);
                break;
            case IOTestType::RAND_WRITE:
                th = photon::thread_create11(write_integrity, max_offset, src_file, dst_file, &io_alloc);
                break;
            case IOTestType::RAND_COPY:
                th = photon::thread_create11(copy_integrity, max_offset, src_file, dst_file, &io_alloc);
                break;
            case IOTestType::RAND_WRITE_PERF:
                th = photon::thread_create11(write_perf, max_offset, src_file, &io_alloc);
                break;
            case IOTestType::RAND_READ_PERF:
                th = photon::thread_create11(read_perf, max_offset, src_file, &io_alloc);
                break;
        }
        photon::thread_enable_join(th);
        join_threads.push_back(th);
    }
    for (auto th: join_threads) {
        photon::thread_join((photon::join_handle*) th);
    }
}

// Before test, manually use fio or dd to make a quick fill on src, and copy it as dst.
// Random read on these two files and compare checksums on the fly.
// Src is iouring engine while dst is psync.
TEST(integrity, DISABLED_read) {
    do_io_test(IOTestType::RAND_READ);
}

// Generate some random bytes, write to src and dst files at the same time.
// After test, manually compare their checksums.
// Src is iouring engine while dst is psync.
TEST(integrity, DISABLED_write) {
    do_io_test(IOTestType::RAND_WRITE);
}

// 4K random copy from src to dst.
// After test, compare their checksums manually.
// Both are iouring engines.
TEST(integrity, DISABLED_copy) {
    do_io_test(IOTestType::RAND_COPY);
}

// fsync and fdatasync
TEST(integrity, DISABLED_fsync) {
    const char* str = "1234";
    auto file = fs::open_localfile_adaptor(FLAGS_dst.c_str(), O_RDWR, 0644, fs::ioengine_iouring);
    ASSERT_NE(file, nullptr);
    auto ret = file->write(str, strlen(str));
    ASSERT_EQ(ret, (int) strlen(str));
    ret = file->fdatasync();
    ASSERT_EQ(ret, 0);
    ret = file->write(str, strlen(str));
    ASSERT_EQ(ret, (int) strlen(str));
    ret = file->fsync();
    ASSERT_EQ(ret, 0);
}

TEST(integrity, DISABLED_open_close_mkdir) {
    auto fs = fs::new_localfs_adaptor("", fs::ioengine_iouring);
    ASSERT_NE(fs, nullptr);
    DEFER(delete fs);

    auto file = fs->open("test_file.bin", O_CREAT | O_TRUNC);
    ASSERT_NE(file, nullptr);
    int ret = system("fuser test_file.bin");
    ASSERT_EQ(ret, 0);
    delete file;
    ret = system("fuser test_file.bin");
    ASSERT_NE(ret, 0);

    fs->rmdir("test_dir");
    ret = fs->mkdir("test_dir", 0755);
    ASSERT_EQ(ret, 0);
    fs->rmdir("test_dir");
}


// 4K random write on source file
TEST(perf, DISABLED_write) {
    do_io_test(IOTestType::RAND_WRITE_PERF);
}

// 4K random read on source file
TEST(perf, DISABLED_read) {
    do_io_test(IOTestType::RAND_READ_PERF);
}

/* Event Engine Tests */

photon::CascadingEventEngine* new_cascading_engine(bool iouring = false) {
    if (iouring) {
        return photon::new_iouring_cascading_engine();
    } else {
        return photon::new_epoll_cascading_engine();
    }
}

TEST(event_engine, master) {
    int fd[2];
    pipe(fd);
    char buf[1];
    auto f = [&] {
        LOG_INFO("sleep 2s");
        photon::thread_sleep(2);
        LOG_INFO("start write");
        write(fd[1], buf, 1);
    };
    photon::thread* sub = photon::thread_create11(&decltype(f)::operator(), &f);
    photon::thread_enable_join(sub);
    LOG_INFO("wait 3s at most");
    ASSERT_EQ(photon::wait_for_fd_readable(fd[0], 3000000), 0);
    photon::thread_join((photon::join_handle*) sub);
}

TEST(event_engine, master_timeout) {
    int fd[2];
    pipe(fd);
    char buf[1];
    auto f = [&] {
        LOG_INFO("sleep 2s");
        photon::thread_sleep(2);
        LOG_INFO("start write");
        write(fd[1], buf, 1);
    };
    photon::thread* sub = photon::thread_create11(&decltype(f)::operator(), &f);
    photon::thread_enable_join(sub);
    LOG_INFO("wait 1s at most");
    ASSERT_EQ(photon::wait_for_fd_readable(fd[0], 1000000), -1);
    ASSERT_EQ(errno, ETIMEDOUT);
    photon::thread_join((photon::join_handle*) sub);
}

TEST(event_engine, master_interrupted) {
    int fd[2];
    pipe(fd);
    photon::thread* main = photon::CURRENT;
    auto f = [&] {
        LOG_INFO("sleep 2s");
        photon::thread_sleep(2);
        LOG_INFO("start interrupt main");
        photon::thread_interrupt(main, EPERM);

    };
    photon::thread* sub = photon::thread_create11(&decltype(f)::operator(), &f);
    photon::thread_enable_join(sub);
    LOG_INFO("wait 3s at most");
    ASSERT_EQ(photon::wait_for_fd_readable(fd[0], 3000000), -1);
    ASSERT_EQ(errno, EPERM);
    photon::thread_join((photon::join_handle*) sub);
}

TEST(event_engine, master_interrupted_after_io) {
    int fd[2];
    pipe(fd);
    char buf[1];
    photon::thread* main = photon::CURRENT;
    auto f = [&] {
        LOG_INFO("sleep 2s");
        photon::thread_sleep(2);
        LOG_INFO("start write");
        write(fd[1], buf, 1);
        LOG_INFO("start interrupt main");
        photon::thread_interrupt(main, EPERM);

    };
    photon::thread* sub = photon::thread_create11(&decltype(f)::operator(), &f);
    photon::thread_enable_join(sub);
    LOG_INFO("wait 3s at most");
    ASSERT_EQ(photon::wait_for_fd_readable(fd[0], 3000000), -1);
    ASSERT_EQ(errno, EPERM);
    photon::thread_join((photon::join_handle*) sub);
}

TEST(event_engine, cascading) {
    int fd1[2];
    int fd2[2];
    pipe(fd1);
    pipe(fd2);
    char buf[1];
    auto f = [&] {
        photon::thread_sleep(2);
        LOG_INFO("write pipe");
        write(fd1[1], buf, 1);
        write(fd2[1], buf, 1);
    };
    photon::thread* sub = photon::thread_create11(&decltype(f)::operator(), &f);
    photon::thread_enable_join(sub);

    auto engine = new_cascading_engine();
    DEFER(delete engine);
    engine->add_interest({fd1[0], photon::EVENT_READ, (void*) 0x1111});
    engine->add_interest({fd2[0], photon::EVENT_READ, (void*) 0x2222});

    void* data[5] = {};
    ssize_t num_events = engine->wait_for_events(data, 5, -1UL);
    ASSERT_EQ(num_events, 2);
    bool b1 = data[0] == (void*) 0x1111 && data[1] == (void*) 0x2222;
    bool b2 = data[0] == (void*) 0x2222 && data[1] == (void*) 0x1111;
    ASSERT_EQ(b1 || b2, true);

    engine->rm_interest({fd1[0], photon::EVENT_READ, (void*) 0x1111});
    engine->rm_interest({fd2[0], photon::EVENT_READ, (void*) 0x2222});

    photon::thread_join((photon::join_handle*) sub);
}

TEST(event_engine, cascading_timeout) {
    int fd1[2];
    int fd2[2];
    pipe(fd1);
    pipe(fd2);
    char buf[1];
    auto f = [&] {
        photon::thread_sleep(2);
        write(fd1[1], buf, 1);
        write(fd2[1], buf, 1);
    };
    photon::thread* sub = photon::thread_create11(&decltype(f)::operator(), &f);
    photon::thread_enable_join(sub);

    auto engine = new_cascading_engine();
    DEFER(delete engine);
    engine->add_interest({fd1[0], photon::EVENT_READ, (void*) 0x1111});
    engine->add_interest({fd2[0], photon::EVENT_READ, (void*) 0x2222});

    void* data[5] = {};
    ssize_t num_events = engine->wait_for_events(data, 5, 1000000);
    ASSERT_EQ(num_events, -1);
    ASSERT_EQ(errno, ETIMEDOUT);

    engine->rm_interest({fd1[0], photon::EVENT_READ, (void*) 0x1111});
    engine->rm_interest({fd2[0], photon::EVENT_READ, (void*) 0x2222});

    photon::thread_join((photon::join_handle*) sub);
}

TEST(event_engine, cascading_remove) {
    int fd1[2];
    int fd2[2];
    pipe(fd1);
    pipe(fd2);
    char buf[1];
    auto engine = new_cascading_engine();
    DEFER(delete engine);
    auto f = [&] {
        photon::thread_sleep(1);
        engine->rm_interest({fd1[0], photon::EVENT_READ, (void*) 0x1111});
        photon::thread_sleep(1);
        LOG_INFO("start write fd");
        write(fd1[1], buf, 1);
        write(fd2[1], buf, 1);
    };
    photon::thread* sub = photon::thread_create11(&decltype(f)::operator(), &f);
    photon::thread_enable_join(sub);

    engine->add_interest({fd1[0], photon::EVENT_READ, (void*) 0x1111});
    engine->add_interest({fd2[0], photon::EVENT_READ, (void*) 0x2222});

    void* data[5] = {};
    ssize_t num_events = engine->wait_for_events(data, 5, -1UL);
    ASSERT_EQ(num_events, 1);
    ASSERT_EQ(data[0], (void*) 0x2222);


    engine->rm_interest({fd2[0], photon::EVENT_READ, (void*) 0x2222});

    photon::thread_join((photon::join_handle*) sub);
}

TEST(event_engine, cascading_one_shot) {
    int fd1[2];
    int fd2[2];
    pipe(fd1);
    pipe(fd2);
    char buf[1];
    auto f = [&] {
        photon::thread_sleep(1);
        LOG_INFO("start write 2 fd");
        write(fd1[1], buf, 1);
        write(fd2[1], buf, 1);
        photon::thread_sleep(1);
        LOG_INFO("start write 2 fd");
        write(fd1[1], buf, 1);
        write(fd2[1], buf, 1);

    };
    photon::thread* sub = photon::thread_create11(&decltype(f)::operator(), &f);
    photon::thread_enable_join(sub);

    auto engine = new_cascading_engine();
    DEFER(delete engine);
    engine->add_interest({fd1[0], photon::EVENT_READ | photon::ONE_SHOT, (void*) 0x1111});
    engine->add_interest({fd2[0], photon::EVENT_READ, (void*) 0x2222});

    void* data[5] = {};
    LOG_INFO("wait 2 events");
    ssize_t num_events = engine->wait_for_events(data, 5, 2000000);
    ASSERT_EQ(num_events, 2);
    bool b1 = data[0] == (void*) 0x1111 && data[1] == (void*) 0x2222;
    bool b2 = data[0] == (void*) 0x2222 && data[1] == (void*) 0x1111;
    ASSERT_EQ(b1 || b2, true);

    LOG_INFO("wait 1 events");
    num_events = engine->wait_for_events(data, 5, 2000000);
    ASSERT_EQ(num_events, 1);
    ASSERT_EQ(data[0], (void*) 0x2222);

    engine->rm_interest({fd2[0], photon::EVENT_READ, (void*) 0x2222});

    LOG_INFO("wait non events");
    num_events = engine->wait_for_events(data, 5, 2000000);
    ASSERT_EQ(num_events, -1);
    ASSERT_EQ(errno, ETIMEDOUT);

    photon::thread_join((photon::join_handle*) sub);
}

int main(int argc, char** arg) {
    srand(time(nullptr));
    set_log_output_level(ALOG_INFO);

    testing::InitGoogleTest(&argc, arg);
    testing::FLAGS_gtest_break_on_failure = true;
    gflags::ParseCommandLineFlags(&argc, &arg, true);

    int ret = photon::thread_init();
    if (ret != 0) return -1;
    DEFER(photon::thread_fini());
    ret = photon::fd_events_init();
    if (ret != 0) return -1;
    DEFER(photon::fd_events_fini());

    return RUN_ALL_TESTS();
}
