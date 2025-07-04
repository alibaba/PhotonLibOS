#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>
#include <photon/common/iovector.h>
#include <photon/io/spdkbdev-wrapper.h>
#include "../../test/gtest.h"

class SPDKBDev {
public:
    void init() {
        ASSERT_EQ(photon::init(), 0);
        photon::spdk::bdev_env_init(json_cfg_path);
        photon::spdk::bdev_open_ext("Malloc0", true, &desc);
        ASSERT_NE(desc, nullptr);
        ch = photon::spdk::bdev_get_io_channel(desc);
        ASSERT_NE(ch, nullptr);
    }

    void fini() {
        photon::spdk::bdev_put_io_channel(ch);
        photon::spdk::bdev_close(desc);
        photon::spdk::bdev_env_fini();
        photon::fini();
    }

    static const char* json_cfg_path;
    struct spdk_bdev_desc* desc;
    struct spdk_io_channel* ch;
};

// path of bdev config json
const char* SPDKBDev::json_cfg_path = "./examples/spdk/bdev.json";

class SPDKBDevTestEnv : public ::testing::Environment {
public:
    void SetUp() override {
        ASSERT_EQ(bdev_info, nullptr);
        bdev_info = new SPDKBDev();
        bdev_info->init();
        GTEST_LOG_(INFO) << "SetUp Success";
    }

    void TearDown() override {
        ASSERT_NE(bdev_info, nullptr);
        bdev_info->fini();
        delete bdev_info;
        GTEST_LOG_(INFO) << "TearDown Success";
    }

    static SPDKBDev* bdev_info;
};

SPDKBDev* SPDKBDevTestEnv::bdev_info = nullptr;

class SPDKBDevTest : public ::testing::Test {
public:
    SPDKBDev* bdev_info = SPDKBDevTestEnv::bdev_info;
};

TEST_F(SPDKBDevTest, rw) {
    struct spdk_bdev_desc* desc = bdev_info->desc;
    struct spdk_io_channel* ch = bdev_info->ch;

    uint64_t bufsz = 4096;
    uint64_t blocksz = 512;
    uint64_t nblocks = bufsz / blocksz;

    void* bufwrite = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* bufread = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    EXPECT_NE(bufwrite, nullptr);
    EXPECT_NE(bufread, nullptr);
    DEFER(spdk_free(bufwrite));
    DEFER(spdk_free(bufread));

    // prepare datas to write
    char test_data[] = "hello world";
    for (uint64_t i=0; i<nblocks; i++) {
        strncpy((char*)bufwrite + i * blocksz, test_data, 12);
    }

    // writes in parallel
    std::vector<photon::join_handle*> ths_write;
    for (uint64_t i=0; i<nblocks; i++) {
        void* buf = (void*)((char*)bufwrite + i * blocksz);
        uint64_t off = i * blocksz, cnt = blocksz;
        ths_write.emplace_back(
            photon::thread_enable_join(
                photon::thread_create11(
                [](int idx, struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, void* buffer, uint64_t offset, uint64_t nbytes) {
                    int rc = photon::spdk::bdev_write(desc, ch, buffer, offset, nbytes);
                    EXPECT_EQ(rc, 0);
                }, i, desc, ch, buf, off, cnt)
            )
        );
    }
    for (auto th : ths_write) { // wait all writes complete
        photon::thread_join(th);
    }

    // reads in parallel
    std::vector<photon::join_handle*> ths_read;
    for (uint64_t i=0; i<nblocks; i++) {
        void* buf = (void*)((char*)bufread + i * blocksz);
        uint64_t off = i * blocksz, cnt = blocksz;
        ths_read.emplace_back(
            photon::thread_enable_join(
                photon::thread_create11(
                [](int idx, struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, void* buffer, uint64_t offset, uint64_t nbytes) {
                    int rc = photon::spdk::bdev_read(desc, ch, buffer, offset, nbytes);
                    EXPECT_EQ(rc, 0);
                }, i, desc, ch, buf, off, cnt)
            )
        );
    }
    for (auto th: ths_read) {   // wait all reads complete
        photon::thread_join(th);
    }

    // checking
    EXPECT_EQ(memcmp(bufwrite, bufread, bufsz), 0);
}

TEST_F(SPDKBDevTest, rw_blocks) {
    struct spdk_bdev_desc* desc = bdev_info->desc;
    struct spdk_io_channel* ch = bdev_info->ch;

    uint64_t bufsz = 4096;
    uint64_t blocksz = 512;
    uint64_t nblocks = bufsz / blocksz;

    void* bufwrite = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* bufread = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    EXPECT_NE(bufwrite, nullptr);
    EXPECT_NE(bufread, nullptr);
    DEFER(spdk_free(bufwrite));
    DEFER(spdk_free(bufread));

    std::vector<photon::join_handle*> ths_write;
    for (uint64_t i = 0; i < nblocks; i++) {
        void* bufbegin = (void*)((char*)bufwrite + i * blocksz);
        memset(bufbegin, i, blocksz);
        ths_write.emplace_back(photon::thread_enable_join(
        photon::thread_create11([](struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, void* buf, uint64_t offset_blocks, uint64_t num_blocks){
            photon::spdk::bdev_write_blocks(desc, ch, buf, offset_blocks, num_blocks);
        }, desc, ch, bufbegin, i, 1)));
    }
    for (auto th : ths_write) {
        photon::thread_join(th);
    }

    std::vector<photon::join_handle*> ths_read;
    for (uint64_t i = 0; i < nblocks; i++) {
        void* bufbegin = (void*)((char*)bufread + i * blocksz);
        ths_read.emplace_back(photon::thread_enable_join(
        photon::thread_create11([](struct spdk_bdev_desc* desc, struct spdk_io_channel* ch, void* buf, uint64_t offset_blocks, uint64_t num_blocks){
            photon::spdk::bdev_read_blocks(desc, ch, buf, offset_blocks, num_blocks);
        }, desc, ch, bufbegin, i, 1)));
    }
    for (auto th : ths_read) {
        photon::thread_join(th);
    }

    EXPECT_EQ(memcmp(bufwrite, bufread, bufsz), 0);
}

TEST_F(SPDKBDevTest, rwv) {
    struct spdk_bdev_desc* desc = bdev_info->desc;
    struct spdk_io_channel* ch = bdev_info->ch;

    uint64_t bufsz = 4096;
    uint64_t blocksz = 512;
    uint64_t nblocks = bufsz / blocksz;

    void* bufwrite = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* bufread = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    EXPECT_NE(bufwrite, nullptr);
    EXPECT_NE(bufread, nullptr);
    DEFER(spdk_free(bufwrite));
    DEFER(spdk_free(bufread));

    photon::WorkPool wp(1, 0, 0, 0);

    IOVector iov0, iov1;
    for (uint64_t i = 0; i < nblocks; i++) {
        void* bufbegin = (void*)((char*)bufwrite + i * blocksz);
        memset(bufbegin, i, blocksz);
        if (i % 2 == 0) iov0.push_back(bufbegin, blocksz);
        else iov1.push_back(bufbegin, blocksz);
    }
    EXPECT_EQ(iov0.sum() + iov1.sum(), bufsz);

    photon::semaphore sem;
    wp.async_call(new auto([&]{
        photon::spdk::bdev_writev(desc, ch, iov0.iovec(), iov0.iovcnt(), 0, iov0.sum());
        sem.signal(1);
    }));
    wp.async_call(new auto([&]{
        photon::spdk::bdev_writev(desc, ch, iov1.iovec(), iov1.iovcnt(), iov0.sum(), iov1.sum());
        sem.signal(1);
    }));
    sem.wait(2);


    IOVector iov2;
    for (uint64_t i = 0; i < nblocks; i+=2) {
        void* bufbegin = (void*)((char*)bufread + i * blocksz);
        iov2.push_back(bufbegin, blocksz);
    }
    for (uint64_t i = 1; i < nblocks; i+=2) {
        void* bufbegin = (void*)((char*)bufread + i * blocksz);
        iov2.push_back(bufbegin, blocksz);
    }
    EXPECT_EQ(iov2.iovcnt(), iov0.iovcnt() + iov1.iovcnt());
    wp.async_call(new auto([&]{
        photon::spdk::bdev_readv(desc, ch, iov2.iovec(), iov2.iovcnt(), 0, iov2.sum());
        sem.signal(1);
    }));
    sem.wait(1);

    EXPECT_EQ(memcmp(bufwrite, bufread, bufsz), 0);
}

TEST_F(SPDKBDevTest, rwv_blocks) {
    struct spdk_bdev_desc* desc = bdev_info->desc;
    struct spdk_io_channel* ch = bdev_info->ch;

    uint64_t bufsz = 512;

    void* bufwrite = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* bufread = spdk_zmalloc(bufsz, 4096, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    EXPECT_NE(bufwrite, nullptr);
    EXPECT_NE(bufread, nullptr);
    DEFER(spdk_free(bufwrite));
    DEFER(spdk_free(bufread));

    memset(bufwrite, 0x42, bufsz);

    IOVector iov_write, iov_read;
    iov_write.push_back(bufwrite, bufsz);
    iov_read.push_back(bufread, bufsz);

    EXPECT_EQ(photon::spdk::bdev_writev_blocks(desc, ch, iov_write.iovec(), iov_write.iovcnt(), 0, 1), 0);
    EXPECT_EQ(photon::spdk::bdev_readv_blocks(desc, ch, iov_read.iovec(), iov_read.iovcnt(), 0, 1), 0);

    EXPECT_EQ(memcmp(bufwrite, bufread, bufsz), 0);
}

int main(int argc, char** argv) {
    testing::AddGlobalTestEnvironment(new SPDKBDevTestEnv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}