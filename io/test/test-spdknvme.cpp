#include <photon/photon.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>
#include <photon/common/iovector.h>
#include <photon/io/spdknvme-wrapper.h>
#include "../../test/gtest.h"

class SPDKNVMe {
public:
    void init() {
        ASSERT_EQ(photon::spdk::nvme_env_init(), 0);
        ctrlr = photon::spdk::nvme_probe_attach(trid_str);
        ASSERT_NE(ctrlr, nullptr);
        ns = photon::spdk::nvme_get_namespace(ctrlr, nsid);
        ASSERT_NE(ns, nullptr);
        ASSERT_EQ(photon::init(), 0);
    }

    void fini() {
        GTEST_LOG_(INFO) << "SPDK NVMe fini";
        photon::fini();
        GTEST_LOG_(INFO) << "after photon::fini";
        photon::spdk::nvme_detach(ctrlr);
        GTEST_LOG_(INFO) << "after detach";
        photon::spdk::nvme_env_fini();
        GTEST_LOG_(INFO) << "after nvme_env_fini";
    }

    static const char* trid_str;
    static const int nsid;
    struct spdk_nvme_ctrlr* ctrlr;
    struct spdk_nvme_ns* ns;
};

const char* SPDKNVMe::trid_str = "trtype:pcie traddr:0000:86:00.0";
const int SPDKNVMe::nsid = 1;

class SPDKNVMeTestEnv : public ::testing::Environment {
public:
    void SetUp() override {
        ASSERT_EQ(nvme_info, nullptr);
        nvme_info = new SPDKNVMe();
        nvme_info->init();
        GTEST_LOG_(INFO) << "SetUp Success";
    }

    void TearDown() override {
        ASSERT_NE(nvme_info, nullptr);
        nvme_info->fini();
        delete nvme_info;
        GTEST_LOG_(INFO) << "TearDown Success";

    }

    static SPDKNVMe* nvme_info;
};

SPDKNVMe* SPDKNVMeTestEnv::nvme_info = nullptr;

class SPDKNVMeTest : public ::testing::Test {
public:
    SPDKNVMe* nvme_info = SPDKNVMeTestEnv::nvme_info;
};

TEST_F(SPDKNVMeTest, rw) {
    struct spdk_nvme_ctrlr* ctrlr = nvme_info->ctrlr;
    struct spdk_nvme_ns* ns = nvme_info->ns;

    struct spdk_nvme_qpair* qpair = photon::spdk::nvme_ctrlr_alloc_io_qpair(ctrlr, nullptr, 0);
    EXPECT_NE(qpair, nullptr);
    DEFER(photon::spdk::nvme_ctrlr_free_io_qpair(ctrlr, qpair));

    uint32_t sectorsz = spdk_nvme_ns_get_sector_size(ns);
    uint32_t nsec = 8;
    uint64_t bufsz = sectorsz * nsec;

    void* buf_write = spdk_zmalloc(bufsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* buf_read = spdk_zmalloc(bufsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    EXPECT_NE(buf_write, nullptr);
    EXPECT_NE(buf_read, nullptr);
    DEFER(spdk_free(buf_write));
    DEFER(spdk_free(buf_read));

    // prepare datas to write
    for (int i=0; i<nsec; i++) {
        memset((char*)buf_write + i * sectorsz, i, sectorsz);
    }

    std::vector<photon::join_handle*> jhs_write;
    for (int i=0; i<nsec; i++) {
        char* buf = (char*)buf_write + i * sectorsz;
        jhs_write.emplace_back(photon::thread_enable_join(photon::thread_create11([](struct spdk_nvme_ns* ns, spdk_nvme_qpair* qpair, void* buf, uint64_t lba, uint32_t lba_count){
            EXPECT_EQ(photon::spdk::nvme_ns_cmd_write(ns, qpair, buf, lba, lba_count, 0), 0);
        }, ns, qpair, buf, i, 1)));
    }
    for (auto jh: jhs_write) {
        photon::thread_join(jh);
    }

    std::vector<photon::join_handle*> jhs_read;
    for (int i=0; i<nsec; i++) {
        char* buf = (char*)buf_read + i * sectorsz;
        jhs_read.emplace_back(photon::thread_enable_join(photon::thread_create11([](struct spdk_nvme_ns* ns, spdk_nvme_qpair* qpair, void* buf, uint64_t lba, uint32_t lba_count){
            EXPECT_EQ(photon::spdk::nvme_ns_cmd_read(ns, qpair, buf, lba, lba_count, 0), 0);
        }, ns, qpair, buf, i, 1)));
    }
    for (auto jh: jhs_read) {
        photon::thread_join(jh);
    }

    // checking
    EXPECT_EQ(memcmp(buf_write, buf_read, bufsz), 0);
}

TEST_F(SPDKNVMeTest, rwv) {
    struct spdk_nvme_ctrlr* ctrlr = nvme_info->ctrlr;
    struct spdk_nvme_ns* ns = nvme_info->ns;

    struct spdk_nvme_qpair* qpair = photon::spdk::nvme_ctrlr_alloc_io_qpair(ctrlr, nullptr, 0);
    EXPECT_NE(qpair, nullptr);
    DEFER(photon::spdk::nvme_ctrlr_free_io_qpair(ctrlr, qpair));

    uint32_t sectorsz = spdk_nvme_ns_get_sector_size(ns);
    uint32_t nsec = 4;
    uint64_t bufsz = sectorsz * nsec;

    void* buf_write = spdk_zmalloc(bufsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    void* buf_read = spdk_zmalloc(bufsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    EXPECT_NE(buf_write, nullptr);
    EXPECT_NE(buf_read, nullptr);
    DEFER(spdk_free(buf_write));
    DEFER(spdk_free(buf_read));

    // prepare datas to write
    char* buf_write_a = (char*)buf_write;
    char* buf_write_b = (char*)buf_write + sectorsz;
    char* buf_write_c = (char*)buf_write + 3 * sectorsz;
    memset(buf_write_a, 'a', sectorsz);
    memset(buf_write_b, 'b', 2 * sectorsz);
    memset(buf_write_c, 'c', sectorsz);

    IOVector iovs_write;
    iovs_write.push_back(buf_write_a, sectorsz);
    iovs_write.push_back(buf_write_b, 2 * sectorsz);
    iovs_write.push_back(buf_write_c, sectorsz);

    IOVector iovs_read;
    iovs_read.push_back(buf_read, sectorsz);
    iovs_read.push_back((char*)buf_read + sectorsz, sectorsz);
    iovs_read.push_back((char*)buf_read + 2 * sectorsz, 2 * sectorsz);

    GTEST_LOG_(INFO) << "writev";
    EXPECT_EQ(photon::spdk::nvme_ns_cmd_writev(ns, qpair, iovs_write.iovec(), iovs_write.iovcnt(), nsec, nsec, 0), 0);
    GTEST_LOG_(INFO) << "readv";
    EXPECT_EQ(photon::spdk::nvme_ns_cmd_readv(ns, qpair, iovs_read.iovec(), iovs_read.iovcnt(), nsec, nsec, 0), 0);

    // checking
    EXPECT_EQ(memcmp(buf_write, buf_read, bufsz), 0);
}

TEST_F(SPDKNVMeTest, multi_thread) {
    struct spdk_nvme_ctrlr* ctrlr = nvme_info->ctrlr;
    struct spdk_nvme_ns* ns = nvme_info->ns;

    int nvcpu = 4;
    photon::WorkPool wp(nvcpu, 0, 0, 0);

    int ntest = 64;
    uint32_t sectorsz = spdk_nvme_ns_get_sector_size(ns);

    // writes
    GTEST_LOG_(INFO) << "writes";
    photon::semaphore sem;
    for (int i=0; i<ntest; i++) {
        wp.thread_migrate(photon::thread_create11([&](int idx){
            GTEST_LOG_(INFO) << "write " << idx;
            struct spdk_nvme_qpair* qpair = photon::spdk::nvme_ctrlr_alloc_io_qpair(ctrlr, nullptr, 0);
            EXPECT_NE(qpair, nullptr);
            DEFER(photon::spdk::nvme_ctrlr_free_io_qpair(ctrlr, qpair));

            void* buffer = spdk_zmalloc(sectorsz, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
            EXPECT_NE(buffer, nullptr);
            DEFER(spdk_free(buffer));

            memset(buffer, idx, sectorsz);
            GTEST_LOG_(INFO) << "write before " << idx;
            EXPECT_EQ(photon::spdk::nvme_ns_cmd_write(ns, qpair, buffer, idx, 1, 0), 0);
            GTEST_LOG_(INFO) << "write after " << idx;
            sem.signal(1);
        }, i));
    }
    sem.wait(ntest);

    // read
    GTEST_LOG_(INFO) << "read";
    struct spdk_nvme_qpair* qpair = photon::spdk::nvme_ctrlr_alloc_io_qpair(ctrlr, nullptr, 0);
    EXPECT_NE(qpair, nullptr);
    DEFER(photon::spdk::nvme_ctrlr_free_io_qpair(ctrlr, qpair));

    void* buffer = spdk_zmalloc(sectorsz * ntest, 0, nullptr, SPDK_ENV_SOCKET_ID_ANY, 1);
    EXPECT_NE(buffer, nullptr);
    DEFER(spdk_free(buffer));
    EXPECT_EQ(photon::spdk::nvme_ns_cmd_read(ns, qpair, buffer, 0, ntest, 0), 0);

    // checking
    void* checkbuf = malloc(sectorsz * ntest);
    EXPECT_NE(checkbuf, nullptr);
    DEFER(free(checkbuf));
    for (int i=0; i<ntest; i++) {
        memset((char*)checkbuf + i * sectorsz, i, sectorsz);
    }
    EXPECT_EQ(memcmp(buffer, checkbuf, sectorsz * ntest), 0);
    GTEST_LOG_(INFO) << "read done";
}


int main(int argc, char** argv) {
    testing::AddGlobalTestEnvironment(new SPDKNVMeTestEnv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}