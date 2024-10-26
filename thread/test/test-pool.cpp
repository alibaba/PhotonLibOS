#include <inttypes.h>
#include <errno.h>
#include <math.h>
#include <string>
#include <stdlib.h>
#include <queue>
#include <algorithm>
#include <sys/time.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <photon/common/alog.h>
#include <photon/thread/thread-pool.h>
#include <photon/thread/thread11.h>
#include <photon/thread/workerpool.h>
#include "../../test/ci-tools.h"
#include <thread>

DEFINE_int32(ths_total, 100, "total threads when testing threadpool.");

using namespace std;
using namespace photon;

void *func1(void *)
{
    thread_sleep(rand()%5);
    return nullptr;
}

TEST(ThreadPool, test)
{
    ThreadPool<64> pool(DEFAULT_STACK_SIZE);
    vector<TPControl*> ths;
    ths.resize(FLAGS_ths_total);
    for (int i = 0; i<FLAGS_ths_total; i++)
        ths[i] = pool.thread_create_ex(&::func1, nullptr, true);
    // LOG_INFO("----------");
    for (int i = 0; i<FLAGS_ths_total; i++) {
        LOG_DEBUG("wait thread: `", ths[i]->th);
        pool.join(ths[i]);
    }
    // LOG_INFO("???????????????");
}

TEST(ThreadPool, migrate) {
    WorkPool wp(4, 0, 0, -1);
    ThreadPool<64> pool(DEFAULT_STACK_SIZE);
    vector<TPControl*> ths;
    ths.resize(FLAGS_ths_total);
    for (int i = 0; i < FLAGS_ths_total; i++) {
        ths[i] = pool.thread_create_ex(&::func1, nullptr, true);
        wp.thread_migrate(ths[i]->th);
    }
    LOG_INFO("----------");
    for (int i = 0; i < FLAGS_ths_total; i++) {
        LOG_DEBUG("wait thread: `", ths[i]->th);
        pool.join(ths[i]);
    }
    LOG_INFO("???????????????");
}

TEST(ThreadPool, multithread) {
    WorkPool wp(4, 0, 0, -1);
    ThreadPool<64> pool(DEFAULT_STACK_SIZE);
    vector<TPControl*> ths;
    ths.resize(FLAGS_ths_total);
    for (int i = 0; i < FLAGS_ths_total; i++) {
        wp.call(
            [&] { ths[i] = pool.thread_create_ex(&::func1, nullptr, true); });
    }
    for (int i = 0; i < FLAGS_ths_total; i++) {
        wp.call([&] {
            pool.join(ths[i]);
        });
    }
}

int jobwork(WorkPool* pool, int i) {
    LOG_INFO("LAUNCH");
    int ret = 0;
    pool->call(
        [&ret](int i) {
            LOG_INFO("START");
            this_thread::sleep_for(std::chrono::seconds(1));
            LOG_INFO("FINISH");
            ret = i;
        },
        i);
    LOG_INFO("RETURN");
    EXPECT_EQ(i, ret);
    return 0;
}

TEST(workpool, work) {
    std::unique_ptr<WorkPool> pool(new WorkPool(2));

    std::vector<join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 4; i++) {
        jhs.emplace_back(thread_enable_join(
            thread_create11(&jobwork, pool.get(), i)));
    }
    for (auto& j : jhs) {
        thread_join(j);
    }
    auto duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(2));
    EXPECT_LE(duration, std::chrono::seconds(3));
}

TEST(workpool, async_work_capture) {
    std::unique_ptr<WorkPool> pool(new WorkPool(2, 0, 0, 0));

    semaphore sem;
    int flag[10] = {0};
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 10; i++) {
        pool->async_call(new auto([&sem, i, &flag]{
            EXPECT_FALSE(flag[i]);
            flag[i] = true;
            auto x = i;
            LOG_INFO(x);
            thread_usleep(2000 * 1000);
            EXPECT_EQ(x, i);
            EXPECT_TRUE(flag[i]);
            sem.signal(1);
        }));
    }
    auto duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(0));
    EXPECT_LE(duration, std::chrono::seconds(1));
    sem.wait(10);
    LOG_INFO("DONE");
}

struct CopyMoveRecord{
    size_t copy = 0;
    size_t move = 0;
    CopyMoveRecord() {
    }
    ~CopyMoveRecord() {
    }
    CopyMoveRecord(const CopyMoveRecord& rhs) {
        copy = rhs.copy + 1;
        move = rhs.move;
        LOG_INFO("COPY ", this);
    }
    CopyMoveRecord(CopyMoveRecord&& rhs) {
        copy = rhs.copy;
        move = rhs.move + 1;
        LOG_INFO("MOVE ", this);
    }
    CopyMoveRecord& operator=(const CopyMoveRecord& rhs) {
        copy = rhs.copy + 1;
        move = rhs.move;
        LOG_INFO("COPY ASSIGN ", this);
        return *this;
    }
    CopyMoveRecord& operator=(CopyMoveRecord&& rhs) {
        copy = rhs.copy;
        move = rhs.move + 1;
        LOG_INFO("MOVE ASSIGN", this);
        return *this;
    }
};

TEST(workpool, async_work_lambda) {
    std::unique_ptr<WorkPool> pool(new WorkPool(2));

    std::vector<join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 4; i++) {
        CopyMoveRecord *r = new CopyMoveRecord();
        pool->async_call(
            new auto ([r]() {
                LOG_INFO("START ", VALUE(__cplusplus), VALUE(r->copy),
                         VALUE(r->move));
                EXPECT_EQ(0UL, r->copy);
                this_thread::sleep_for(std::chrono::seconds(1));
                LOG_INFO("FINISH");
                delete r;
            }));
    }
    auto duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(0));
    EXPECT_LE(duration, std::chrono::seconds(1));
    thread_sleep(3);
    LOG_INFO("DONE");
}


TEST(workpool, async_work_lambda_threadcreate) {
    std::unique_ptr<WorkPool> pool(new WorkPool(1, 0, 0, 0));

    std::vector<join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    semaphore sem;
    for (int i = 0; i < 4; i++) {
        CopyMoveRecord *r = new CopyMoveRecord();
        pool->async_call(
            new auto ([&sem, r]() {
                LOG_INFO("START ", VALUE(__cplusplus), VALUE(r->copy),
                         VALUE(r->move));
                EXPECT_EQ(0UL, r->copy);
                thread_sleep(1);
                sem.signal(1);
                LOG_INFO("FINISH");
                delete r;
            }));
    }
    auto duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(0));
    EXPECT_LE(duration, std::chrono::seconds(1));
    sem.wait(4);
    duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(1));
    EXPECT_LE(duration, std::chrono::seconds(3));
    LOG_INFO("DONE");
}

TEST(workpool, async_work_lambda_threadpool) {
    std::unique_ptr<WorkPool> pool(new WorkPool(1, 0, 0, 4));

    std::vector<join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    semaphore sem;
    for (int i = 0; i < 4; i++) {
        CopyMoveRecord *r = new CopyMoveRecord();
        pool->async_call(
            new auto ([&sem, r]() {
                LOG_INFO("START ", VALUE(__cplusplus), VALUE(r->copy),
                         VALUE(r->move));
                EXPECT_EQ(0UL, r->copy);
                thread_sleep(1);
                sem.signal(1);
                LOG_INFO("FINISH");
                delete r;
            }));
    }
    auto duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(0));
    EXPECT_LE(duration, std::chrono::seconds(1));
    sem.wait(4);
    duration = std::chrono::system_clock::now() - start;
    LOG_INFO(VALUE(duration.count()));
    EXPECT_GE(duration, std::chrono::seconds(1));
    EXPECT_LE(duration, std::chrono::seconds(2));
    LOG_INFO("DONE");
}

TEST(workpool, async_work_lambda_threadpool_append) {
    std::unique_ptr<WorkPool> pool(new WorkPool(0, 0, 0, 0));

    for (int i=0;i<4;i++) {
        std::thread([&]{
            vcpu_init();
            DEFER(vcpu_fini());
            pool->join_current_vcpu_into_workpool();
        }).detach();
    }

    std::vector<join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    semaphore sem;
    for (int i = 0; i < 4; i++) {
        CopyMoveRecord *r = new CopyMoveRecord();
        pool->async_call(
            new auto ([&sem, r]() {
                LOG_INFO("START ", VALUE(__cplusplus), VALUE(r->copy),
                         VALUE(r->move));
                EXPECT_EQ(0UL, r->copy);
                thread_sleep(1);
                sem.signal(1);
                LOG_INFO("FINISH");
                delete r;
            }));
    }
    auto duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(0));
    EXPECT_LE(duration, std::chrono::seconds(1));
    sem.wait(4);
    duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(1));
    EXPECT_LE(duration, std::chrono::seconds(2));
    LOG_INFO("DONE");
}

int main(int argc, char** arg)
{
    if (!is_using_default_engine()) return 0;
    ::testing::InitGoogleTest(&argc, arg);
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    default_audit_logger.log_output = log_output_stdout;
    vcpu_init();
    DEFER(vcpu_fini());
    set_log_output_level(ALOG_DEBUG);
    return RUN_ALL_TESTS();
}

