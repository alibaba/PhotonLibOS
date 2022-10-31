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

#include <vector>
#include <random>
#include <queue>
#include <thread>
#include <algorithm>

#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include <photon/common/alog.h>

#define private public
#define protected public
#include "../out-of-order-execution.cpp"
#include "../../thread/thread.cpp"
#include <photon/thread/thread11.h>

using namespace std;
using namespace photon;
using namespace photon::rpc;

DEFINE_int32(vcpus, 1, "total # of vCPUs");

thread_local vector<int> aop;

static void wait_for_completion(int vcpu_id = -1) {
    auto current = CURRENT;
    auto& sleepq = current->get_vcpu()->sleepq;
    while (true) {
        photon::thread_usleep(1);
        if (current->prev() == current->next()) {
            if (sleepq.empty()) break;
            auto len = sleepq.front()->ts_wakeup - now;
            if (len > 1000 * 1000) len = 1000 * 1000;
            LOG_DEBUG("sleep ` us", len);
            ::usleep(len);
        }
    }
}

int do_issue(void*, OutOfOrderContext* args)
{
    auto tag = args->tag;
    LOG_DEBUG(VALUE(tag));
    aop.push_back(args->tag);
    random_shuffle(aop.begin(), aop.end());
    return 0;
}

int do_completion(void*, OutOfOrderContext* args)
{
    thread_usleep(1000*10);
    auto tag = args->tag = aop.back();
    LOG_DEBUG(VALUE(tag));
    return 0;
}

void* test_ooo_execution(void* args_)
{
    OutOfOrderContext args;
    args.engine = (OutOfOrder_Execution_Engine*)args_;
    args.do_issue.bind(nullptr, &do_issue);
    args.do_completion.bind(nullptr, &do_completion);

    LOG_DEBUG("before issue_wait()");
    auto engine = (OooEngine*)args_;
    engine->issue_wait(args);
    LOG_DEBUG("after issue_wait()");

    EXPECT_FALSE(aop.empty());
    auto tag = aop.back();
    EXPECT_EQ(args.tag, (uint64_t)tag);
    aop.pop_back();
    engine->result_collected();
    LOG_DEBUG("exit ", VALUE(tag));
    return nullptr;
}

TEST(OutOfOrder, Execution) {
    OooEngine engine;
    for (int i = 0; i < 5; ++i)
        thread_create(test_ooo_execution, &engine, 64 * 1024);

    thread_yield();
    wait_for_completion();
    // while (!aop.empty())
    //     thread_usleep(0);
}


thread_local vector<uint64_t> issue_list;
thread_local queue<uint64_t> processing_queue;

int heavy_issue(void*, OutOfOrderContext* args) {
    issue_list.push_back(args->tag);
    return 0;
}

int heavy_complete(void*, OutOfOrderContext* args) {
    while (processing_queue.empty()) { thread_yield(); } //waiting till something comming
    args->tag = processing_queue.front();
    processing_queue.pop();
    return 0;
}

int process_thread() {
    while (issue_list.size()) {
        thread_yield();
        random_shuffle(issue_list.begin(), issue_list.end());
        processing_queue.push(issue_list.back());
        issue_list.pop_back();
    }
    return 0;
}

void test_ooo_same_tag(OutOfOrder_Execution_Engine* engine, int(*test_issue)(void*, OutOfOrderContext*), int(*test_complete)(void*, OutOfOrderContext*)) {
    // 3 stages:
    // 1. Get ready and commit issue
    OutOfOrderContext args;
    args.engine = engine;
    args.do_issue.bind(nullptr, test_issue);
    args.do_completion.bind(nullptr, test_complete);
    if (ooo_issue_operation(args) < 0) {
        return;
    }
    // end with commit issue
    // 2. get ready to recieve results (maybe not it's own result)
    auto mytag = args.tag;
    if (ooo_wait_completion(args) < 0) {
        return;
    }
    // end with being a part of workers recieving results while waiting
    // 3. Once the thread runs to here, it means the result of this own args are recieved
    EXPECT_EQ(mytag, args.tag);
    ooo_result_collected(args);
    // end by telling the engine the collection is done.
}

void test_ooo(OutOfOrder_Execution_Engine* engine, int(*test_issue)(void*, OutOfOrderContext*), int(*test_complete)(void*, OutOfOrderContext*)) {
    OutOfOrderContext args;
    args.engine = engine;
    args.do_issue.bind(nullptr, test_issue);
    args.do_completion.bind(nullptr, test_complete);
    // generally using issue_wait
    ooo_issue_wait(args);
    ooo_result_collected(args);
}

TEST(OutOfOrder, keep_same_tag) {
    const int thread_num = 100; //10k photon threads
    OutOfOrder_Execution_Engine * engine = new_ooo_execution_engine();
    DEFER({
              wait_for_completion();
              delete_ooo_execution_engine(engine);
          });
    for (int i=0;i<thread_num;i++) {
        thread_create11(64*1024, test_ooo_same_tag, engine, &heavy_issue, &heavy_complete);
    }
    thread_join(thread_enable_join(thread_create11(process_thread)));
}


TEST(OutOfOrder, heavy_test) {
    const int thread_num = 10000; //10k photon threads
    OutOfOrder_Execution_Engine * engine = new_ooo_execution_engine();
    DEFER({
              wait_for_completion();
              delete_ooo_execution_engine(engine);
          });
    for (int i=0;i<thread_num;i++) {
        thread_create11(64*1024, test_ooo, engine, &heavy_issue, &heavy_complete);
    }
    thread_join(thread_enable_join(thread_create11(process_thread)));
}

inline int error_issue(void*, OutOfOrderContext* args) {
    if ((rand()%2) == 0) {
        return -1;
    }
    return heavy_issue(nullptr, args);
}

inline int error_complete(void*, OutOfOrderContext* args) {
    while (processing_queue.empty()) { thread_yield_to(nullptr); } //waiting till something comming
    args->tag = processing_queue.front();
    if (rand()%2)
        return -1;
    processing_queue.pop();
    return 0;
}

inline int error_process() {
    while (issue_list.size()) {
        thread_yield_to(nullptr);
        random_shuffle(issue_list.begin(), issue_list.end());
        auto val = issue_list.back();
        if (rand()%2)
            val = UINT64_MAX;
        processing_queue.push(val);
        issue_list.pop_back();
    }
    return 0;
}

TEST(OutOfOrder, error_issue) {
    log_output = log_output_null;
    const int thread_num = 100;
    issue_list.clear();
    while(!processing_queue.empty()) processing_queue.pop();
    OutOfOrder_Execution_Engine * engine = new_ooo_execution_engine();
    DEFER({
              wait_for_completion();
              delete_ooo_execution_engine(engine);
          });
    for (int i=0;i<thread_num;i++) {
        thread_create11(64*1024, test_ooo, engine, &error_issue, &heavy_complete);
    }
    thread_join(thread_enable_join(thread_create11(process_thread)));
    log_output = log_output_stdout;
}


TEST(OutOfOrder, error_complete) {
    log_output = log_output_null;
    DEFER({
              log_output = log_output_stdout;
          });
    const int thread_num = 100;
    issue_list.clear();
    while(!processing_queue.empty()) processing_queue.pop();
    OutOfOrder_Execution_Engine * engine = new_ooo_execution_engine();
    DEFER({
              wait_for_completion();
              delete_ooo_execution_engine(engine);
          });
    for (int i=0;i<thread_num;i++) {
        thread_create11(64*1024, test_ooo, engine, &heavy_issue, &error_complete);
    }
    thread_join(thread_enable_join(thread_create11(process_thread)));
    thread_sleep(1);
}

TEST(OutOfOrder, error_process) {
    log_output = log_output_null;
    DEFER({
              log_output = log_output_stdout;
          });
    const int thread_num = 100;
    issue_list.clear();
    while(!processing_queue.empty()) processing_queue.pop();
    OutOfOrder_Execution_Engine * engine = new_ooo_execution_engine();
    DEFER({
              wait_for_completion();
              delete_ooo_execution_engine(engine);
          });
    for (int i=0;i<thread_num;i++) {
        thread_create11(64*1024, test_ooo, engine, &heavy_issue, &heavy_complete);
    }
    thread_join(thread_enable_join(thread_create11(error_process)));
    thread_sleep(1);
}

int null_op(void*, OutOfOrderContext*) {
    return 0;
}

void twice_issue(OutOfOrder_Execution_Engine *engine) {
    OutOfOrderContext args, args_same;
    args.engine = engine;
    args.flag_tag_valid = true;
    args.tag = 20150820UL;
    args.do_issue.bind(nullptr, null_op);
    args.do_completion.bind(nullptr, null_op);
    args_same = args;
    errno = 0;
    auto ret = ooo_issue_operation(args);
    EXPECT_EQ(0, ret);
    ret = ooo_issue_operation(args_same);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EINVAL, errno);
    ooo_wait_completion(args);
}

void auto_gen_tag_overwhelm(OutOfOrder_Execution_Engine *engine) {
    OutOfOrderContext args;
    args.engine = engine;
    args.do_issue.bind(nullptr, null_op);
    args.do_completion.bind(nullptr, null_op);
    args.do_collect.bind(nullptr, null_op);
    args.flag_tag_valid = true;
    args.tag = 1;
    errno = 0;
    auto ret = ooo_issue_operation(args);
    EXPECT_EQ(0, ret);
    //here args get a auto generated tag by engine
    OutOfOrderContext args_same;
    args_same.engine = engine;
    args_same.do_issue.bind(nullptr, null_op);
    args_same.do_completion.bind(nullptr, null_op);
    args.do_collect.bind(nullptr, null_op);
    ret = ooo_issue_operation(args_same);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(2UL, args_same.tag);
    ooo_wait_completion(args);
    ooo_wait_completion(args_same);
}


TEST(OutOfOrder, error_same_tag) {
    log_output = log_output_null;
    OutOfOrder_Execution_Engine * engine = new_ooo_execution_engine();
    DEFER({
              wait_for_completion();
              delete_ooo_execution_engine(engine);
          });
    twice_issue(engine);
    auto_gen_tag_overwhelm(engine);
    log_output = log_output_stdout;
}

TEST(OutOfOrder, error_change_arg) {
    log_output = log_output_null;
    OutOfOrder_Execution_Engine * engine = new_ooo_execution_engine();
    DEFER({
              wait_for_completion();
              delete_ooo_execution_engine(engine);
          });
    OutOfOrderContext args;
    args.engine = engine;
    args.do_issue.bind(nullptr, null_op);
    args.do_completion.bind(nullptr, null_op);
    args.do_collect.bind(nullptr, null_op);
    int ret = ooo_issue_operation(args);
    EXPECT_EQ(0, ret);
    auto otag = args.tag;
    args.tag = 20150820UL;
    errno = 0;
    ret = ooo_wait_completion(args);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EINVAL, errno);
    args.tag = otag;
    errno = 0;
    ret = ooo_wait_completion(args);
    log_output = log_output_stdout;
}

int error_change_engine_complete(void*, OutOfOrderContext* args) {
    OooEngine* engine = (OooEngine*)(args->engine);
    if (args->tag == 1) {
        thread_usleep(1000);
        args->tag = 2;
    } else {
        args->tag = 1;
    }
    engine->m_map[args->tag]->th = nullptr;
    return 0;
}

void error_change_engine_issuewait(OutOfOrder_Execution_Engine * engine) {
    OutOfOrderContext args;
    args.engine = engine;
    args.do_issue.bind(nullptr, null_op);
    args.do_completion.bind(nullptr, error_change_engine_complete);
    args.do_collect.bind(nullptr, null_op);
    int ret = ooo_issue_wait(args);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EINVAL, errno);
}

TEST(OutOfOrder, error_thread_become_NULL) {
    log_output = log_output_null;
    OutOfOrder_Execution_Engine * engine = new_ooo_execution_engine();
    DEFER({
              wait_for_completion();
              // delete_ooo_execution_engine(engine);
          });
    OutOfOrderContext args;
    args.engine = engine;
    args.do_issue.bind(nullptr, null_op);
    args.do_completion.bind(nullptr, error_change_engine_complete);
    thread_create11(error_change_engine_issuewait, engine);
    int ret = ooo_issue_wait(args);
    EXPECT_EQ(-2, ret);
    EXPECT_EQ(ENOENT, errno);
    wait_for_completion();
    log_output = log_output_stdout;
}

void run_all_tests(uint32_t i) {
#define RUN_TEST(A, B) LOG_DEBUG("vCPU #", i, ": "#A":"#B); A##_##B##_Test().TestBody();
    RUN_TEST(OutOfOrder, Execution);
    RUN_TEST(OutOfOrder, keep_same_tag);
    RUN_TEST(OutOfOrder, heavy_test);
    RUN_TEST(OutOfOrder, error_issue);
    RUN_TEST(OutOfOrder, error_complete);
    RUN_TEST(OutOfOrder, error_process);
    RUN_TEST(OutOfOrder, error_same_tag);
    RUN_TEST(OutOfOrder, error_change_arg);
    RUN_TEST(OutOfOrder, error_thread_become_NULL);
    wait_for_completion(i);
#undef RUN_TEST
}

int main(int argc, char** arg) {
    ::testing::InitGoogleTest(&argc, arg);
    google::ParseCommandLineFlags(&argc, &arg, true);
    default_audit_logger.log_output = log_output_stdout;
    set_log_output_level(ALOG_INFO);

    photon::vcpu_init();

    if (FLAGS_vcpus <= 1) {
        return RUN_ALL_TESTS();
    }

    std::vector<std::thread> ths;
    for (int i = 1; i <= FLAGS_vcpus; i++) {
        ths.emplace_back([i] {
            photon::vcpu_init();
            set_log_output_level(ALOG_INFO);
            run_all_tests(i);
            photon::vcpu_fini();
        });
    }

    for (auto &x : ths) x.join();

    wait_for_completion(0);
    while (photon::vcpu_fini() != 0) {
        LOG_DEBUG("wait for other vCPU(s) to end");
        ::usleep(1000 * 1000);
    }
    LOG_DEBUG("exit");
    return 0;
}
