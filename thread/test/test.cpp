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

#define private public

#include "../thread.cpp"
#include "../thread11.h"
#include "../thread-pool.h"
#include "../workerpool.h"
#include <photon/common/alog-audit.h>

using namespace std;
using namespace photon;

DEFINE_int32(ths_total, 100, "total threads when testing threadpool.");
DEFINE_int32(vcpus, 1, "total # of vCPUs");

thread_local semaphore aSem(4);
void* asdf(void* arg)
{
    LOG_DEBUG("Hello world, in photon thread-`! step 1", (uint64_t)arg);
    photon::thread_usleep(rand() % 1000 + 1000*500);
    LOG_DEBUG("Hello world, in photon thread-`! step 2", (uint64_t)arg);
    photon::thread_usleep(rand() % 1000 + 1000*500);
    LOG_DEBUG("Hello world, in photon thread-`! step 3", (uint64_t)arg);
    aSem.signal(1);
    return arg;
}

struct fake_context
{
    uint64_t a,b,c,d,e,f,g,h;
};

void fake_context_switch(fake_context* a, fake_context* b)
{
    a->a = b->a;
    a->b = b->b;
    a->c = b->c;
    a->d = b->d;
    a->e = b->e;
    a->f = b->f;
    a->g = b->g;
    a->h = b->h;
}

uint64_t isqrt(uint64_t n)
{
    if (n == 0)
        return 0;

    uint64_t x =  1ULL << (sizeof(n) * 8 / 2);
    while (true)
    {
        auto y = (x + n / x) / 2;
        if (y >= x)
            return x;
        x = y;
    }
}

inline uint64_t now_time()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 * 1000 + tv.tv_usec;
}

__attribute__((noinline))
void test_sat_add()
{
    uint64_t a = -1, b = 10;
    auto c = sat_add(a, b);
    LOG_DEBUG(c);
}

bool check(SleepQueue& sleepq)
{
    for (size_t i = 0; (i << 1) + 1 < sleepq.q.size(); i++) {
        size_t l = (i << 1) + 1;
        size_t r = l + 1;
        assert(sleepq.q[i]->ts_wakeup <= sleepq.q[l]->ts_wakeup);
        if (r < sleepq.q.size())
            assert(sleepq.q[i]->ts_wakeup <= sleepq.q[r]->ts_wakeup);
    }
    return true;
}

void print_heap(SleepQueue& sleepq)
{
    LOG_INFO("    ");
    int i = 0, k = 1;
    for (auto it : sleepq.q) {
        printf("%lu(%d)", it->ts_wakeup, it->idx);
        i++;
        if (i == k) {
            printf("\n");
            i = 0;
            k <<= 1;
        } else printf(" ");
    }
    printf("\n");
}

void sleepq_perf(SleepQueue& sleepq, const vector<photon::thread*>& items)
{
    auto size = items.size() / 1000;
    LOG_INFO("============ performance test ==============");
    {
        update_now();
        SCOPE_AUDIT("BUILD HEAP (push) `K items into sleepq.", size);
        for (auto &it : items)
            sleepq.push(it);
        update_now();
    }
    LOG_INFO("--------------------------------------------");
    // print_heap(sleepq);
    check(sleepq);

    auto pops = items;
    random_shuffle(pops.begin(), pops.end());
    pops.resize(pops.size()/2);
    {
        update_now();
        SCOPE_AUDIT("RANDOM POP `K items in sleepq", size/2);
        for (auto& th : pops)
            sleepq.pop(th);
        update_now();
    }
    LOG_INFO("--------------------------------------------");
    // print_heap();
    check(sleepq);

    LOG_INFO("REBUILD HEAP");
    while (!sleepq.empty())
        sleepq.pop_front();
    for (auto& th : items)
        sleepq.push(th);
    LOG_INFO("--------------------------------------------");

    {
        update_now();
        SCOPE_AUDIT("POP FRONT `K items.", size);
        while (!sleepq.empty())
            sleepq.pop_front();
        update_now();
    }
    LOG_INFO("============================================");
}

TEST(Sleep, queue)  //Sleep_queue_Test::TestBody
{
    const int heap_size  = 1000000;
    const int rand_limit = 100000000;

    auto seed = /* time(0); */10007;
    srand(seed);
    SleepQueue sleepq;
    sleepq.q.reserve(heap_size);
    vector<photon::thread*> items;

    LOG_INFO("ITEMS WITH SAME VALUE.");
    for (int i = 0; i < heap_size; i++){
        auto th = new photon::thread();
        th->ts_wakeup = 1000;/* rand() % 100000000 */;
        items.emplace_back(th);
    }
    sleepq_perf(sleepq, items);

    LOG_INFO("ITEMS WITH RANDOM VALUE.");
    for (auto th: items)
        th->ts_wakeup = rand() % rand_limit;
    sleepq_perf(sleepq, items);

    LOG_INFO("sleepq test done.");
    for (auto th : items)
        delete th;
}

thread_local photon::condition_variable aConditionVariable;
thread_local photon::mutex aMutex;

void* thread_test_function(void* arg)
{
    LOG_DEBUG(VALUE(CURRENT), " before lock");
    photon::scoped_lock aLock(aMutex);
    LOG_DEBUG(VALUE(CURRENT), " after lock");

    uint32_t* result = (uint32_t*)arg;

    *result += 100;
//    for(uint32_t i = 0; i < 100; ++i)
//    {
//        (*result)++;
//    }

    LOG_DEBUG(VALUE(CURRENT), " before sleep");
    photon::thread_usleep(100);
    LOG_DEBUG(VALUE(CURRENT), " after sleep");

    *result += 100;
//    for(uint32_t i = 0; i < 100; ++i)
//    {
//        (*result)++;
//    }

    LOG_DEBUG(VALUE(CURRENT), " before yield");
    photon::thread_yield_to(nullptr);
    LOG_DEBUG(VALUE(CURRENT), " after yield");

    *result += 100;
//    for(uint32_t i = 0; i < 100; ++i)
//    {
//        (*result)++;
//    }

    aConditionVariable.notify_one();
    aConditionVariable.notify_all();

    LOG_DEBUG(VALUE(CURRENT), " after notified, about to release lock and exit");
    return arg;
}

void wait_for_completion(int vcpu_id = -1)
{
    auto current = CURRENT;
    auto& sleepq = current->get_vcpu()->sleepq;
    while (true)
    {
        photon::thread_usleep(1);
        if (current->prev() == current->next())
        {
            if (sleepq.empty()) break;
            auto len = sleepq.front()->ts_wakeup - now;
            if (len > 1000*1000) len = 1000*1000;
            LOG_DEBUG("sleep ` us", len);
            ::usleep(len);
        }
    }
}

TEST(ThreadTest, HandleNoneZeroInput)
{
    uint32_t result = 0;
    auto th1 = photon::thread_create(&thread_test_function, (void*)&result);
    wait_for_completion();
    _unused(th1);

    // EXPECT_EQ(photon::states::DONE, thread_stat(th1));
    photon::thread_yield();
    photon::thread_yield_to(nullptr);
    photon::thread_usleep(0);
    photon::thread_usleep(1000);

    auto th2 = photon::thread_create(&thread_test_function, (void*)&result);
    auto th3 = photon::thread_create(&thread_test_function, (void*)&result);
    _unused(th2);
    _unused(th3);

    LOG_DEBUG("before aConditionVariable.wait_no_lock");
    EXPECT_EQ(0, aConditionVariable.wait_no_lock(1000*1000*1000));
    LOG_DEBUG("after aConditionVariable.wait_no_lock");

//    thread_interrupt(th2,0);

    wait_for_completion();

    EXPECT_EQ(900U, result);

}

TEST(ListTest, HandleNoneZeroInput)
{
    struct listElement :
        public intrusive_list_node<listElement> { };

    listElement* aList = nullptr;
    for (int i = 0; i < 10; ++i)
    {
        listElement* newElement = new listElement();
        if (!aList)
        {
            aList = newElement;
        }
        else
        {
            aList->insert_tail(newElement);
        }
    }

    listElement::iterator it = aList->begin();

    int deleteCount = 0;
    std::vector<listElement*> delete_vector;

    for ( ;it != aList->end(); ++it)
    {
        ++deleteCount;
        delete_vector.push_back(*it);
    }

    for (auto e : delete_vector)
    {
        delete e;
    }

    EXPECT_EQ(10, deleteCount);
}

thread_local static int volatile running = 0;
void* thread_pong(void* depth)
{
#ifdef RANDOMIZE_SP
    if (depth)
        return thread_pong((void*)((uint64_t)depth - 1));
#endif
    running = 1;
    while (running)
        thread_yield_fast();
    return nullptr;
}

int DevNull(void*, int);
void test_thread_switch(uint64_t nth, uint64_t stack_size)
{
    const uint64_t total = 100*1000*1000;
    uint64_t count = total / nth;

    for (uint64_t i = 0; i < nth - 2; ++i)
        photon::thread_create(&thread_pong, (void*)(uint64_t)(rand() % 32), stack_size);

    for (uint64_t i = 0; i < count; ++i)
        thread_yield_fast();

    auto t0 = now_time();
    for (uint64_t i = 0; i < count; ++i)
        thread_yield_fast();

    auto t1 = now_time();

    for (uint64_t i = 0; i < count; ++i)
        DevNull(&i, 0);

    auto t2 = now_time();

    auto d1 = t1 - t0;
    auto d2 = (t2 - t1) * nth;
    LOG_INFO("threads `, stack-size `, time `, loop time `, ", nth, stack_size, d1, d2);
    LOG_INFO("single context switch: ` ns (` M/s)",
             double((d1 - d2)*1000) / total, total / double(d1 - d2));

    running = 0;
    wait_for_completion();
}

TEST(Perf, ThreadSwitch)
{
    test_thread_switch(10, 64 * 1024);
    return;

    const uint64_t stack_size = 8 * 1024 * 1024;
    LOG_INFO(VALUE(stack_size));
    test_thread_switch(2, stack_size);
    test_thread_switch(10, stack_size);
    test_thread_switch(100, stack_size);
    test_thread_switch(1000, stack_size);
    test_thread_switch(10000, stack_size);
    test_thread_switch(100000, stack_size);

    for (uint64_t ss = 5; ss <= 13; ++ss)
    {
        test_thread_switch(100000, (1<<ss) * 1024);
    }
}

TEST(Perf, ThreadSwitchWithStandaloneTSUpdater)
{
    timestamp_updater_init();
    DEFER(timestamp_updater_fini());
    test_thread_switch(10, 64 * 1024);
    return;
}

thread_local int shot_count;
thread_local photon::condition_variable shot_cond;
uint64_t on_timer_asdf(void* arg)
{
    shot_count++;
    LOG_DEBUG(VALUE(arg), VALUE(shot_count));
    shot_cond.notify_one();
    return 0;
}

void wait_for_shot()
{
    LOG_DEBUG("wait for a timer to occur ", VALUE(now));
    auto t0 = now;
    shot_cond.wait_no_lock();
    auto delta_time = now - t0;
    LOG_DEBUG("ok, the timer fired ", VALUE(now), VALUE(delta_time));
}

TEST(Timer, OneShot)
{
    shot_count = 0;
    // since last test is perf, will not update photon::now
    // all timers may trigger before wait_for_shot() called.
    // photon::now should be update.
    photon::thread_yield();
    Timer timer1(-1, {&on_timer_asdf, &shot_cond}, true);
    timer1.reset(1000*1000);
    Timer timer2(-1, {&shot_cond, &on_timer_asdf}, false);
    timer2.reset(500*1000);
    wait_for_shot();
    wait_for_shot();
    EXPECT_EQ(shot_count, 2);
}

void test_reuse(Timer& timer)
{
    timer.reset(100*1000);
    wait_for_shot();
    timer.reset(300*1000);
    wait_for_shot();
}

TEST(Timer, Reuse)
{
    {
        Timer timer(1000*1000, {&on_timer_asdf, &shot_count});
        wait_for_shot();
        test_reuse(timer);
    }
    {
        Timer timer(-1, {&on_timer_asdf, nullptr});
        test_reuse(timer);
    }
}

thread_local uint64_t t0;
thread_local int timer_count;
uint64_t on_timer(void* arg)
{
    EXPECT_EQ(arg, &timer_count);
    auto t1 = now_time();
    auto delta_t = t1 - t0;
    t0 = t1;
    LOG_INFO(VALUE(delta_t));
    LOG_INFO(VALUE(timer_count));
    if (timer_count == 0)
    {
        timer_count = -1;
        return -1;
    }
    return timer_count-- * (100*1000);
}

TEST(Timer, Reapting)
{
    timer_count = 5;
    t0 = now_time();
    Timer timer(1000*1000, {&on_timer, &timer_count});
    // auto timer = timer_create(1000*1000, &on_timer, &timer_count, TIMER_FLAG_REPEATING);
    // _unused(timer);
    while(timer_count >= 0)
        thread_usleep(1000);
    // timer_destroy(timer_arg.ptimer);
}

TEST(Thread, function)
{
    photon::join_handle* th1 = photon::thread_enable_join(photon::thread_create(&asdf, (void*)0));
    photon::join_handle* th2 = photon::thread_enable_join(photon::thread_create(&asdf, (void*)1));
    photon::join_handle* th3 = photon::thread_enable_join(photon::thread_create(&asdf, (void*)2));

    photon::thread_yield();
    photon::thread_usleep(0);
    photon::thread_usleep(1000);

    LOG_DEBUG("before join");
    photon::thread_join(th1);
    photon::thread_join(th2);
    photon::thread_join(th3);
    LOG_DEBUG("after join");
}

TEST(thread11, example)
{
    __Example_of_Thread11__ example;
    example.asdf();
}

class Marker : public string
{
public:
    using string::string;
    Marker(const Marker& rhs) : string(rhs)
    {
        auto r = (void*)&rhs;
        LOG_DEBUG(VALUE(this), "(&)", VALUE(r));
    }
    Marker(Marker&& rhs) : string((string&&)rhs)
    {
        auto r = (void*)&rhs;
        LOG_DEBUG(VALUE(this), "(&&)", VALUE(r));
    }
    void operator = (const Marker& rhs)
    {
        auto r = (void*)&rhs;
        LOG_DEBUG(VALUE(this), "=(&)", VALUE(r));
        string::operator=(rhs);
    }
    void operator = (Marker&& rhs)
    {
        auto r = (void*)&rhs;
        LOG_DEBUG(VALUE(this), "=(&&)", VALUE(r));
        string::operator=(rhs);
    }

    void randwrite(void* file, size_t nwrites);

//    ~Marker()                                   { LOG_DEBUG(VALUE(this), "~()"); }
};

thread_local Marker a("asdf"), b("bbsitter"), c("const");
thread_local Marker aa(a.c_str()), cc(c.c_str());

void Marker::randwrite(void* file, size_t nwrites)
{
    EXPECT_EQ(this, &c);
    EXPECT_EQ(file, (void*)1234567);
    EXPECT_EQ(1244UL, nwrites);
}

int test_thread(Marker x, Marker& y, Marker z, int n)
{
    LOG_DEBUG(' ', n);
    EXPECT_EQ(x, a);
    EXPECT_EQ(&y, &b);
    EXPECT_EQ(z, cc);
    EXPECT_EQ(c, "");
    return 0;
}

int test_thread2(Marker x, Marker& y, Marker&& z, int n)
{
    LOG_DEBUG(' ', n);
    EXPECT_EQ(x, aa);
    EXPECT_EQ(a, "");
    EXPECT_EQ(&y, &b);
    EXPECT_EQ(&z, &c);
    EXPECT_EQ(c, cc);
    return 0;
}

TEST(thread11, test)
{
    LOG_DEBUG(VALUE(&a), VALUE(&b), VALUE(&c));
    {
        LOG_DEBUG(' ');
        test_thread(a, b, std::move(c), 30);
        a.assign(aa); c.assign(cc);
    }
    {
        LOG_DEBUG(' ');
        auto th = thread_create11(&test_thread, a, b, std::move(c), 31);
        auto jh = thread_enable_join(th);
        thread_join(jh);
        a.assign(aa); c.assign(cc);
    }

    {
        int n = 42;
        LOG_DEBUG(' ');
        test_thread2(std::move(a), (b), std::move(c), std::move(n));
        a.assign(aa); c.assign(cc);
    }
    {
        int n = 43;
        LOG_DEBUG(' ');
        auto th = thread_create11(&test_thread2, std::move(a), (b), std::move(c), std::move(n));
        auto jh = thread_enable_join(th);
        thread_join(jh);
        a.assign(aa); c.assign(cc);
    }

    {
        LOG_DEBUG(' ');
        auto th = thread_create11(&Marker::randwrite, &c, (void*)1234567, 1244);
        auto jh = thread_enable_join(th);
        thread_join(jh);
    }
    {
        // int n = 11;
        auto th = thread_create11(&Marker::randwrite, &c, (void*)1234567, 1244);
        auto jh = thread_enable_join(th);
        thread_join(jh);
    }
}


void semaphore_test_hold(semaphore* sem, int &step) {
    int ret = 0;
    step++;
    EXPECT_EQ(1, step);
    sem->signal(1);
    LOG_DEBUG("+1");
    ret = sem->wait(2);
    EXPECT_EQ(0, ret);
    LOG_DEBUG("-2");
    step++;
    EXPECT_EQ(3, step);
    sem->signal(3);
    LOG_DEBUG("+3");
    ret = sem->wait(4);
    EXPECT_EQ(0, ret);
    step++;
    EXPECT_EQ(5, step);
    LOG_DEBUG("-4");
}

void semaphore_test_catch(semaphore* sem, int &step) {
    int ret = 0;
    ret = sem->wait(1);
    EXPECT_EQ(0, ret);
    LOG_DEBUG("-1");
    step++;
    EXPECT_EQ(2, step);
    sem->signal(2);
    LOG_DEBUG("+2");
    ret = sem->wait(3);
    EXPECT_EQ(0, ret);
    LOG_DEBUG("-3");
    step++;
    EXPECT_EQ(4, step);
    sem->signal(4);
    LOG_DEBUG("+4");
}

TEST(Semaphore, basic) {
    int step = 0;
    semaphore sem(0);
    auto th1 = thread_create11(semaphore_test_catch, &sem, step);
    auto th2 = thread_create11(semaphore_test_hold, &sem, step);
    auto jh1 = thread_enable_join(th1);
    auto jh2 = thread_enable_join(th2);
    thread_join(jh1);
    thread_join(jh2);
}

thread_local int cnt = 0, cnt2 = 0;
void semaphore_heavy(semaphore& sem, int tid) {
//    LOG_DEBUG("enter");
    cnt++;
    cnt2++;
    sem.wait(tid);
    sem.signal(tid + 1);
    cnt--;
}

TEST(Semaphore, heavy) {
    semaphore sem(0);
    const int thread_num = 100000;
    for (int i=1; i<=thread_num;i++) {
        thread_create11(64 * 1024, semaphore_heavy, sem, i);
    }
    LOG_DEBUG("created ` threads", thread_num);
    sem.signal(1);
    auto ret = sem.wait(thread_num+2, 1);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ETIMEDOUT, errno);
    ret = sem.wait(thread_num+1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0UL, sem.m_count);
    wait_for_completion();
}

TEST(Sleep, sleep_only_thread) {    //Sleep_sleep_only_thread_Test::TestBody
    // If current thread is only thread and sleeping
    // it should be able to avoid crash during long time sleep
    auto start = photon::now;
    photon::thread_sleep(2);
    auto dt = photon::now - start;
    EXPECT_GT(dt, 1UL*1024*1024);
    EXPECT_LE(dt, 3UL*1024*1024);
}

void *func1(void *)
{
    photon::thread_sleep(rand()%5);
    return nullptr;
}

TEST(ThreadPool, test)
{
    ThreadPool<64> pool(64*1024);
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
    ThreadPool<64> pool(64 * 1024);
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
    ThreadPool<64> pool(64 * 1024);
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

thread_local uint64_t rw_count;
thread_local bool writing = false;
thread_local photon::rwlock rwl;

void *rwlocktest(void* args) {
    uint64_t carg = (uint64_t) args;
    auto mode = carg & ((1ULL<<32) -1);
    auto id = carg >> 32;
    // LOG_DEBUG("locking ", VALUE(id), VALUE(mode));
    rwl.lock(mode);
    LOG_DEBUG("locked ", VALUE(id), VALUE(mode));
    EXPECT_EQ(id, rw_count);
    rw_count ++;
    if (mode == photon::RLOCK)
        EXPECT_FALSE(writing);
    else
        writing = true;
    photon::thread_usleep(100*1000);
    if (mode == photon::WLOCK)
        writing = false;
    LOG_DEBUG("unlocking ", VALUE(id), VALUE(mode));
    rwl.unlock();
    // LOG_DEBUG("unlocked ", VALUE(id), VALUE(mode));
    return NULL;
}

TEST(RWLock, checklock) {
    std::vector<photon::join_handle*> handles;
    rw_count = 0;
    writing = false;
    for (uint64_t i=0; i<100;i++) {
        uint64_t arg = (i << 32) | (rand()%10 < 7 ? photon::RLOCK : photon::WLOCK);
        handles.emplace_back(
            photon::thread_enable_join(
                photon::thread_create(&rwlocktest, (void*)arg, 64*1024)
            )
        );
    }
    for (auto &x : handles)
        photon::thread_join(x);
    EXPECT_EQ(100UL, rw_count);
}

void* interrupt(void* th) {
    photon::thread_interrupt((photon::thread*)th, EALREADY);
    return 0;
}

TEST(RWLock, interrupt) {
    rwlock rwl;
    int ret = rwl.lock(photon::WLOCK); // write lock
    EXPECT_EQ(0, ret);
    ret = rwl.lock(photon::RLOCK, 1000UL); // it should not be locked
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ETIMEDOUT, errno);
    photon::thread_create(&interrupt, photon::CURRENT);
    ret = rwl.lock(photon::RLOCK);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(EALREADY, errno);
}

TEST(RWLock, smp) {
    rwlock lock;
    std::vector<std::thread> ths;
    for (int i=0 ;i< 10;i++) {
        ths.emplace_back([&]{
            photon::vcpu_init();
            DEFER(photon::vcpu_fini());
            for (int i = 0; i < 100; i++){
                auto ret = lock.lock(WLOCK);
                // LOG_INFO("Locked");
                EXPECT_EQ(0, ret);
                EXPECT_EQ(-1, lock.state);
                // LOG_INFO("Unlock");
                lock.unlock();
            }
        });
    }
    for (auto&x: ths) {
        x.join();
    }
}

void run_all_tests(uint32_t i)
{
#define RUN_TEST(A, B) LOG_DEBUG("vCPU #", i, ": "#A":"#B); A##_##B##_Test().TestBody();
    RUN_TEST(Sleep, queue);
    RUN_TEST(Sleep, sleep_only_thread);
    RUN_TEST(ThreadTest, HandleNoneZeroInput);
    RUN_TEST(ListTest, HandleNoneZeroInput);
    RUN_TEST(Perf, ThreadSwitch);
    RUN_TEST(Timer, OneShot);
    RUN_TEST(Timer, Reuse);
    RUN_TEST(Timer, Reapting);
    RUN_TEST(Thread, function);
    RUN_TEST(thread11, example);
    RUN_TEST(thread11, test);
    RUN_TEST(Semaphore, basic);
    RUN_TEST(Semaphore, heavy);
    RUN_TEST(ThreadPool, test);
    RUN_TEST(RWLock, checklock);
    RUN_TEST(RWLock, interrupt);
    wait_for_completion(i);
#undef RUN_TEST
}

int running_all_tests = 0;
void vcpu_start(uint32_t i)
{
    LOG_DEBUG("vCPU #` begin", i);
    while(running_all_tests)
        run_all_tests(i);
    LOG_DEBUG("vCPU #` end", i);
}

TEST(saturated, add)
{
    uint64_t cases[][3] = {
        {10UL, -1ULL, -1ULL},
        {10UL, -9ULL, -1ULL},
        {1UL, 2UL, 3UL},
        {10UL, 3UL, 13UL},
        {100UL, 4UL, 104UL},
        {1000UL, 5UL, 1005UL},
    };
    for (auto& x: cases) {
        auto z = sat_add(x[0], x[1]);
        EXPECT_EQ(x[2], z);
    }
}

__attribute__((noinline))
void test_sat_sub()
{
    auto c = sat_sub(1, 2);
    DevNull(&c, sizeof(c));
}

TEST(saturated, sub)
{
    test_sat_sub();
    uint64_t cases[][3] = {
        {10UL, 1UL, 9UL},
        {10UL, 9UL, 1UL},
        {1UL, 2UL, 0UL},
        {10UL, 30UL, 0UL},
        {100UL, 400UL, 0UL},
        {1000UL, 5000UL, 0UL},
    };
    for (auto& x: cases) {
        auto z = sat_sub(x[0], x[1]);
        EXPECT_EQ(x[2], z);
    }
}

void* to_ctx(void*)
{
    LOG_DEBUG("target context");
    return 0;
}

void defer_ctx(void* th)
{
    LOG_DEBUG("run function by hijack-ing context ", th);
    auto p = malloc(34798);
    printf("%p\n", p);
    free(p);
}

TEST(context_switch, defer)
{
    auto th = thread_create(&to_ctx, nullptr);
    auto r = prepare_usleep(1000*1000, nullptr);
    switch_context_defer(r.from, r.to, &defer_ctx, th);
}

template<typename...Ts>
void std_threads_create_join(int N, Ts&&...xs)
{
    std::vector<std::thread> ths;
    for (int i=0; i<N; ++i)
    {
        ths.emplace_back(std::forward<Ts>(xs)...);
    }
    // for (int i=0; i<30; ++i)
    // {
    //     ::sleep(1);
    //     auto _interrupted = photon::_interrupted.load();
    //     LOG_DEBUG(VALUE(_interrupted));
    // }
    for (size_t i=0; i<ths.size(); ++i)
    {
        ths[i].join();
        LOG_DEBUG("finished ths[`].join()", i);
    }
}

void photon_do(int n, thread_entry start, void* args)
{
    photon::vcpu_init();
    photon::threads_create_join(n, start, args);
    photon::vcpu_fini();
}

struct smp_args
{
    std::atomic<int> running{0}, counter{0};
    waitq q;
};

void* test_smp_waitq_waiter(void* args_)
{
    auto args = (smp_args*)args_;
    args->running.fetch_add(1);
    uint64_t last_t = 0;
    while(args->q.wait() == 0) {
        auto c = args->counter++;
        if (photon::now - last_t > 10*1024*1024) {
            LOG_INFO("got resumed: ", c);
            last_t = photon::now;
        }
    }
    LOG_INFO("got EINTR (errno=`) and exit", errno);
    args->running.fetch_add(-1);
    return 0;
}

void test_smp_waitq_resumer(int n, smp_args* args)
{
    for (int j = 0; j < n*n*n; ++j)
    {
        ::usleep(rand()%1024 + 1024);
        args->q.resume_one();
    }
}

TEST(smp, waitq)
{
    smp_args args;
    constexpr int N = 9;
    std::thread waiter(&photon_do,
        N, &test_smp_waitq_waiter, &args);
    while(args.running.load() != N) ::usleep(1000);

    std_threads_create_join(N,
        &test_smp_waitq_resumer, N*2, &args);

    while(args.running.load() > 0)
    {
        args.q.resume_all(EINTR);
        ::usleep(1000);
    }
    waiter.join();
}

struct smp_mutex_args
{
    photon::mutex mutex;
    int n, counter = 0;
};

void* test_smp_mutex(void* args_)
{
    auto args = (smp_mutex_args*)args_;
    auto m = args->n * args->n;
    for (int i = 0; i < m*m; ++i)
    {
        photon::thread_yield();
        SCOPED_LOCK(args->mutex);
        args->counter++;
    }
    return 0;
}

smp_mutex_args* _smp_args;
TEST(smp, mutex)
{
    smp_mutex_args args;
    _smp_args = &args;
    auto n = args.n = 8;
    std_threads_create_join(n, &photon_do, n*n, &test_smp_mutex, &args);
    EXPECT_EQ(n*n*n*n*n*n*n, args.counter);
}


std::atomic<uint64_t> srw_count;
std::atomic<bool> swriting;
photon::rwlock srwl;

void *smprwlocktest(void* args) {
    uint64_t carg = (uint64_t) args;
    auto mode = carg & ((1ULL<<32) -1);
    auto id = carg >> 32;
    // LOG_DEBUG("locking ", VALUE(id), VALUE(mode));
    srwl.lock(mode);
    LOG_DEBUG("locked ", VALUE(id), VALUE(mode));
    srw_count ++;
    if (mode == photon::RLOCK)
        EXPECT_FALSE(swriting);
    else {
        swriting = true;
    }
    photon::thread_usleep(1000);
    if (mode == photon::WLOCK)
        swriting = false;
    LOG_DEBUG("unlocking ", VALUE(id), VALUE(mode));
    srwl.unlock();
    // LOG_DEBUG("unlocked ", VALUE(id), VALUE(mode));
    return NULL;
}

TEST(smp, rwlock) {
    srw_count = 0;
    swriting = false;
    std_threads_create_join(10, [&]{
        photon::vcpu_init();
        DEFER(photon::vcpu_fini());
        std::vector<photon::join_handle*> handles;
        for (uint64_t i=0; i<100;i++) {
            uint64_t arg = (i << 32) | (rand()%10 < 7 ? photon::RLOCK : photon::WLOCK);
            handles.emplace_back(
                photon::thread_enable_join(
                    photon::thread_create(&smprwlocktest, (void*)arg, 64*1024)
                )
            );
        }
        for (auto &x : handles)
            photon::thread_join(x);
    });
    EXPECT_EQ(1000UL, srw_count);
}

struct smp_cvar_args
{
    photon::mutex mutex;
    photon::condition_variable cvar;
    atomic<uint64_t> sent{0}, missed{0};
    atomic<uint64_t> senders{0}, recvers{0};
    atomic<uint64_t> recvd{0};
    int n;
};

void* test_smp_cvar_recver(void* args_)
{
    auto args = (smp_cvar_args*)args_;
    args->recvers++;
    while(true)
    {
        {
            thread_yield();
            SCOPED_LOCK(args->mutex);
            int ret = args->cvar.wait(args->mutex);
            assert(ret == 0);
            args->recvd++;
        }
        if (args->senders == 0) break;
        thread_usleep(rand() % 128);
    }
    args->recvers--;
    return 0;
}

void* test_smp_cvar_sender(void* args_)
{
    auto args = (smp_cvar_args*)args_;
    args->senders++;
    auto m = args->n * args->n;
    for (int i=0; i<m*m; ++i)
    {
        if (rand() % 16) {
            auto th = args->cvar.notify_one();
            if (th) args->sent++;
            else args->missed++;
        } else {
            auto n = args->cvar.notify_all();
            // LOG_DEBUG("notify_all()ed to ` threads", n);
            args->sent += n;
        }
        thread_usleep(rand() % 128);
    }
    args->senders--;
    return 0;
}

void* test_smp_cvar(void* args_)
{
    auto args = (smp_cvar_args*)args_;
    auto th = thread_create(&test_smp_cvar_recver, args);
    thread_enable_join(th);
    test_smp_cvar_sender(args);
    if (args->senders == 0)
        while(args->recvers > 0)
        {
            thread_usleep(rand() % 128);
            auto n = args->cvar.notify_all();
            args->sent += n;
        }
    thread_join((join_handle*)th);
    return 0;
}

TEST(smp, cvar)
{
    smp_cvar_args args;
    auto n = args.n = 18;
    std_threads_create_join(n, &photon_do, n, &test_smp_cvar, &args);
    EXPECT_EQ(args.recvd, args.sent);
    auto sent = args.sent.load();
    auto missed = args.missed.load();
    auto recvd = args.recvd.load();
    LOG_DEBUG(VALUE(sent), VALUE(missed), VALUE(recvd));
}

struct smp_semaphore_args
{
    photon::semaphore sem;
    int n;
};

void* test_smp_semaphore(void* args_)
{
    auto args = (smp_semaphore_args*)args_;
    auto m = args->n;// * args->n;
    for (int i=1; i<=m*m; ++i)
    {
        args->sem.wait(i);
        thread_usleep(rand()%64 + i*32);
        args->sem.signal(i+1);
    }
    return 0;
}

TEST(smp, semaphore)
{
    smp_semaphore_args args;
    auto n = args.n = 10UL;
    args.sem.signal(n/2);
    std_threads_create_join(n, &photon_do,
        n*n, &test_smp_semaphore, &args);
    EXPECT_EQ(args.sem.m_count, n*n*n*n*n + n/2UL);
}

TEST(sleep_defer, basic) {
    for (int i = 0; i < 1000; i++) {
        auto start = photon::now;
        auto th = CURRENT;
        std::atomic_bool flag(false), ready(false);
        std::thread([th, &flag, &ready] {
            ready.store(true);
            while (!flag.load()) {
                sched_yield();
            }
            photon::thread_interrupt(th, ENXIO);
        }).detach();
        while (!ready.load()) photon::thread_yield();
        auto ret = photon::thread_usleep_defer(1000UL * 1000, [&flag] {
            flag.store(true);
        });
        EXPECT_EQ(-1, ret);
        EXPECT_EQ(ENXIO, errno);
        EXPECT_LT(photon::now - start, 1000UL * 1000);
    }
}

TEST(mutex, timeout_is_zero) {
    // when timeout is zero
    // mutex.lock should works like try_lock with multi thread(smp) support
    std::vector<std::thread> ths;
    photon::mutex mtx;
    std::atomic<int> cnt(0);
    for (int i=0;i<32;i++) {
        ths.emplace_back([&]{
            photon::vcpu_init();
            DEFER(photon::vcpu_fini());
            for(int j=0;j<10000;j++) {
                auto ret = mtx.lock(0);
                if (ret == 0)
                    mtx.unlock();
                if (ret != 0)
                    cnt ++;
            }
        });
    }
    for(auto &th : ths) th.join();
    LOG_INFO("Meet ` lock timeout, all work finished", cnt.load());
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

    std::vector<photon::join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 4; i++) {
        jhs.emplace_back(photon::thread_enable_join(
            photon::thread_create11(&jobwork, pool.get(), i)));
    }
    for (auto& j : jhs) {
        photon::thread_join(j);
    }
    auto duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(2));
    EXPECT_LE(duration, std::chrono::seconds(3));
}

TEST(workpool, async_work_capture) {
    std::unique_ptr<WorkPool> pool(new WorkPool(2, 0, 0, 0));

    photon::semaphore sem;
    int flag[10] = {0};
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 10; i++) {
        pool->async_call(new auto([&sem, i, &flag]{
            EXPECT_FALSE(flag[i]);
            flag[i] = true;
            auto x = i;
            LOG_INFO(x);
            photon::thread_usleep(2000 * 1000);
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

    std::vector<photon::join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 4; i++) {
        CopyMoveRecord *r = new CopyMoveRecord();
        pool->async_call(
            new auto ([i, r]() {
                LOG_INFO("START ", VALUE(__cplusplus), VALUE(r->copy),
                         VALUE(r->move));
                EXPECT_EQ(0, r->copy);
                this_thread::sleep_for(std::chrono::seconds(1));
                LOG_INFO("FINISH");
                delete r;
            }));
    }
    auto duration = std::chrono::system_clock::now() - start;
    EXPECT_GE(duration, std::chrono::seconds(0));
    EXPECT_LE(duration, std::chrono::seconds(1));
    photon::thread_sleep(3);
    LOG_INFO("DONE");
}


TEST(workpool, async_work_lambda_threadcreate) {
    std::unique_ptr<WorkPool> pool(new WorkPool(1, 0, 0, 0));

    std::vector<photon::join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    photon::semaphore sem;
    for (int i = 0; i < 4; i++) {
        CopyMoveRecord *r = new CopyMoveRecord();
        pool->async_call(
            new auto ([&sem, i, r]() {
                LOG_INFO("START ", VALUE(__cplusplus), VALUE(r->copy),
                         VALUE(r->move));
                EXPECT_EQ(0, r->copy);
                photon::thread_sleep(1);
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

    std::vector<photon::join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    photon::semaphore sem;
    for (int i = 0; i < 4; i++) {
        CopyMoveRecord *r = new CopyMoveRecord();
        pool->async_call(
            new auto ([&sem, i, r]() {
                LOG_INFO("START ", VALUE(__cplusplus), VALUE(r->copy),
                         VALUE(r->move));
                EXPECT_EQ(0, r->copy);
                photon::thread_sleep(1);
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
            photon::vcpu_init();
            DEFER(photon::vcpu_fini());
            pool->join_current_vcpu_into_workpool();
        }).detach();
    }

    std::vector<photon::join_handle*> jhs;
    auto start = std::chrono::system_clock::now();
    photon::semaphore sem;
    for (int i = 0; i < 4; i++) {
        CopyMoveRecord *r = new CopyMoveRecord();
        pool->async_call(
            new auto ([&sem, i, r]() {
                LOG_INFO("START ", VALUE(__cplusplus), VALUE(r->copy),
                         VALUE(r->move));
                EXPECT_EQ(0, r->copy);
                photon::thread_sleep(1);
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

#if defined(_WIN64)
#define SAVE_REG(R) register uint64_t R asm(#R); volatile uint64_t saved_##R = R;
#define CHECK_REG(R) asm volatile ("" : "=m"(saved_##R)); if (saved_##R != R) puts(#R " differs after context switch!");
#else
#define SAVE_REG(R)
#define CHECK_REG(R)
#endif

void* waiter(void* arg) {
    auto p = (int*)arg;
    LOG_INFO("Start", VALUE(p), VALUE(*p));
    SAVE_REG(rbx);
    SAVE_REG(rsi);
    SAVE_REG(rdi);
    SAVE_REG(r12);
    SAVE_REG(r13);
    SAVE_REG(r14);
    SAVE_REG(r15);
    SAVE_REG(rbp);
    SAVE_REG(rsp);
    photon::thread_usleep(1UL * 1000 * 1000);
    CHECK_REG(rbx);
    CHECK_REG(rsi);
    CHECK_REG(rdi);
    CHECK_REG(r12);
    CHECK_REG(r13);
    CHECK_REG(r14);
    CHECK_REG(r15);
    CHECK_REG(rbp);
    CHECK_REG(rsp);
    (*p)--;
    LOG_INFO("Fin", VALUE(p), VALUE(*p));
    return nullptr;
}

void* somework(void* arg) {
    auto vcpu = (photon::vcpu_base*)arg;
    LOG_INFO("Work on `", photon::get_vcpu());
    EXPECT_NE(vcpu, photon::get_vcpu());
    return nullptr;
}

TEST(photon, migrate) {
    vcpu_base* vcpu = nullptr;
    photon::thread *th;
    photon::semaphore sem, semdone;
    std::thread worker([&vcpu, &sem, &th, &semdone]{
        photon::vcpu_init();
        DEFER(photon::vcpu_fini());
        th = CURRENT;
        vcpu = photon::get_vcpu();
        sem.signal(1);
        LOG_TEMP("WORKER READY ", CURRENT);
        semdone.wait(1);
        LOG_TEMP("WORKER DONE ", CURRENT);
    });
    sem.wait(1);
    auto task = photon::thread_create(somework, photon::get_vcpu());
    photon::thread_enable_join(task);
    LOG_TEMP("task READY ", task);
    auto ret = photon::thread_migrate(task, vcpu);
    LOG_TEMP("migrate DONE ", ret);
    photon::thread_join(task);
    LOG_TEMP("task join");
    semdone.signal(1);
    LOG_TEMP("worker interrupt");
    worker.join();
    LOG_TEMP("worker join");
}

TEST(photon, wait_all) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    auto start = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
    int count = 0;
    for (int i = 0; i < 10; i++) {
        count++;
        thread_create(waiter, &count);
    }
    wait_all();
    gettimeofday(&tv, NULL);
    auto end = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
    EXPECT_GE(end - start, 999 * 1000);
    EXPECT_LE(end - start, 1024 * 1024);
    EXPECT_EQ(0, count);
}

void wait_to_be_interrupt() {
    auto th = CURRENT;
    photon::thread_usleep_defer(
        -1UL, [&]{
            std::thread([&]{
                photon::thread_interrupt(th);
            }).detach();
        }
    );
}

TEST(photon, yield_to_standby) {
    auto th = photon::thread_enable_join(photon::thread_create11(wait_to_be_interrupt));
    photon::thread_yield_to((photon::thread*)th);
    while(((photon::thread*)th)->state != states::STANDBY)
        ::sched_yield();
    EXPECT_EQ(states::STANDBY, ((photon::thread*)th)->state);
    photon::thread_yield_to((photon::thread*)th);
    photon::thread_join(th);
}

static char* topbuf = nullptr;
void recursive_call(int level = 1024) {
    char buf[4096];
    topbuf = &buf[0];
    strncpy(buf, "Hello, stack", 13);
    if (level)
        recursive_call(level - 1);
}

void* testwork(void*) {
    recursive_call();
    // there are 4MB stack used;
    stack_pages_gc();
    EXPECT_STRNE("Hello, stack", topbuf);
    return nullptr;
}

#if !defined(__aarch64__) && defined(__linux__)
TEST(photon, free_stack) {
    auto th = thread_enable_join(thread_create(&testwork, nullptr));
    thread_join(th);
}
#endif

void* __null_work(void*) {
    LOG_INFO("RUNNING");
    photon::thread_usleep(1UL * 1000 * 1000);
    LOG_INFO("DONE");
    return nullptr;
}

TEST(smp, join_on_smp) {
    photon::semaphore sem(0);
    photon::condition_variable cv;
    photon::mutex mtx;
    std::vector<photon::join_handle*> jh;
    std::vector<std::thread> jt;
    jh.clear();
    for (int i=0;i<3;i++) {
        jt.emplace_back([&]{
            photon::vcpu_init();
            DEFER({
                LOG_INFO("before FINI");
                photon::vcpu_fini();
                LOG_INFO("FINI");
            });
            {
                photon::scoped_lock locker(mtx);
                jh.emplace_back(photon::thread_enable_join(photon::thread_create(__null_work, nullptr)));
                sem.signal(1);
            }
            cv.wait_no_lock();
            LOG_INFO("STD thread done, ", VALUE(::CURRENT->vcpu->nthreads.load()));
        });
    }
    sem.wait(3);
    LOG_INFO("threads ready to join");
    for (auto &x : jh) {
        photon::thread_join(x);
    }
    LOG_INFO("photon threads joined");
    cv.notify_all();
    for (auto &x : jt) {
        x.join();
    }
    LOG_INFO("std threads joined");
}

TEST(makesure_yield, basic) {
    static volatile bool mark = true;
    photon::thread_create11([&]{
        photon::thread_usleep(100*1000);
        mark=false;
    });
    while (mark) {
        SCOPE_MAKESURE_YIELD;
#ifdef __aarch64__
        asm volatile("isb" : : : "memory");
#else
        _mm_pause();
#endif
    }
    EXPECT_EQ(false, mark);
}


TEST(thread11, lambda) {
    photon::semaphore sem(0);
    photon::thread_create11([&]{
        sem.signal(1);
    });
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
    auto lambda = [](photon::semaphore &sem){
        sem.signal(1);
    };
    photon::thread_create11(lambda, std::ref(sem));
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
    auto lambda2 = [&sem]{
        sem.signal(1);
    };
    photon::thread_create11(lambda2);
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
}

struct simple_functor {
    void operator()(char) {}
};

struct simple_typed_functor {
    int operator()(char) { return 0; }
};

struct overloaded_functor {
    void operator()(char) {}
    void operator()(int) {}
};

struct dumb_object {};

struct static_member_function {
    static void somefunc(char) {}
};

struct member_function_bind {
    void somefunc() {}
} mf;

void func(void*, char) {}

TEST(thread11, functor_trait) {
    char x = rand();
    auto lambda = [](){};
    auto lambda2 = [x](char) {(void)x; };
    auto lambda3 = [lambda, &x]()->int { lambda(); return (int)x; };
    auto bind_obj = std::bind(&member_function_bind::somefunc, &mf);

    EXPECT_EQ(true, (is_functor<simple_functor, char>::value));
    EXPECT_EQ(true, (is_functor<simple_typed_functor, char>::value));
    EXPECT_EQ(true, (is_functor<overloaded_functor, char>::value));
    EXPECT_EQ(true, (is_functor<overloaded_functor, int>::value));
    EXPECT_EQ(true, (is_functor<decltype(lambda)>::value));
    EXPECT_EQ(true, (is_functor<typename std::add_lvalue_reference<decltype(lambda)>::type>::value));
    EXPECT_EQ(true, (is_functor<typename std::add_rvalue_reference<decltype(lambda)>::type>::value));
    EXPECT_EQ(true, (is_functor<decltype(lambda2), char>::value));
    EXPECT_EQ(true, (is_functor<decltype(lambda3)>::value));
    EXPECT_EQ(true, (is_functor<decltype(bind_obj)>::value));
    EXPECT_EQ(false, (is_functor<dumb_object>::value));
    EXPECT_EQ(false, (is_functor<static_member_function, char>::value));
    EXPECT_EQ(false, (is_functor<decltype(&static_member_function::somefunc), char>::value));
    EXPECT_EQ(false, (is_functor<size_t>::value));
}

struct invoke_functor {
    photon::semaphore &sem;
    void operator()(char) {
        sem.signal(1);
    }
};

struct invoke_typed_functor {
    photon::semaphore &sem;
    int operator()(char) {
        sem.signal(1);
        return 0;
    }
};

struct invoke_overloaded_functor {
    photon::semaphore &sem;
    void operator()(char) {
        sem.signal(1);
        LOG_DEBUG("functor overloading: ", "void(char)");
    }
    void operator()(int) {
        sem.signal(2);
        LOG_DEBUG("functor overloading: ", "void(int)");
    }
};

struct invoke_function_bind {
    photon::semaphore &sem;
    void somefunc() {
        sem.signal(1);
    }
};

TEST(thread11, functor_invoke) {
    photon::semaphore sem;
    char x = 1;
    auto lambda = [&sem](){sem.signal(1);};
    auto lambda2 = [x](photon::semaphore&sem) { sem.signal(x); };
    auto lambda3 = [lambda2, &x, &sem]()->int { lambda2(sem); return (int)x; };
    invoke_function_bind mf{sem};
    auto bind_obj = std::bind(&invoke_function_bind::somefunc, &mf);

    invoke_functor ivf1{sem};
    invoke_typed_functor ivf2{sem};
    invoke_overloaded_functor ivf3{sem};

    thread_create11(ivf1, 'x');
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
    thread_create11(ivf2, 'x');
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
    thread_create11(ivf3, 'x');
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
    thread_create11(ivf3, 123);
    EXPECT_EQ(0, sem.wait(2, 1UL*1000*1000));
    thread_create11(lambda);
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
    thread_create11(lambda2, std::ref(sem));
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
    thread_create11(lambda3);
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
    thread_create11(bind_obj);
    EXPECT_EQ(0, sem.wait(1, 1UL*1000*1000));
}

TEST(thread11, functor_param) {
    photon::semaphore sem;
    int arr[8] = {0};
    for (int i=0;i<8;i++) {
        thread_create11([&](int x){
            arr[x] ++;
            LOG_INFO(VALUE(x));
            sem.signal(1);
        }, i);
    }
    sem.wait(8);
    for (int i=0;i<8;i++) {
        EXPECT_EQ(1, arr[i]);
    }
    memset(arr, 0, sizeof(arr));
    for (int i=0;i<8;i++) {
        thread_create11([i, &arr, &sem](){
            arr[i] ++;
            LOG_INFO(VALUE(i));
            sem.signal(1);
        });
    }
    sem.wait(8);
    for (int i=0;i<8;i++) {
        EXPECT_EQ(1, arr[i]);
    }
}

struct IntNode : intrusive_list_node<IntNode> {
    int x;
    IntNode() = default;
};

TEST(intrusive_list, split) {
    intrusive_list<IntNode> testlist;
    IntNode intarr[100];

    for (int i=0;i<100;i++) {
        intarr[i].x=i;
        testlist.push_back(&intarr[i]);
    }

    // since testlist member are on stack
    // just set to clear, without delete
    DEFER(testlist.node = nullptr);

    auto sp = testlist.split_front_exclusive(&intarr[50]);
    DEFER(sp.node = nullptr);
    int cnt = 0;
    for (auto x : sp) {
        EXPECT_EQ(cnt, x->x);
        cnt++;
    }
    EXPECT_EQ(50, cnt);
    for (auto x : testlist) {
        EXPECT_EQ(cnt, x->x);
        cnt++;
    }
    EXPECT_EQ(100, cnt);

    auto ssp = sp.split_front_inclusive(&intarr[15]);
    DEFER(ssp.node = nullptr);
    cnt = 0;
    for (auto x : ssp) {
        EXPECT_EQ(cnt, x->x);
        cnt++;
    }
    EXPECT_EQ(16, cnt);
    for (auto x : sp) {
        EXPECT_EQ(cnt, x->x);
        cnt++;
    }
    EXPECT_EQ(50, cnt);

    auto esplit = ssp.split_front_exclusive(&intarr[0]);
    DEFER(esplit.node = nullptr);
    EXPECT_EQ(nullptr, esplit.node);
    cnt = 0;
    for (auto x : ssp) {
        EXPECT_EQ(cnt, x->x);
        cnt++;
    }
    EXPECT_EQ(16, cnt);

    auto isplit = ssp.split_front_inclusive(&intarr[15]);
    DEFER(isplit.node = nullptr);
    EXPECT_EQ(nullptr, ssp.node);
    cnt = 0;
    for (auto x : isplit) {
        EXPECT_EQ(cnt, x->x);
        cnt++;
    }
    EXPECT_EQ(16, cnt);

    auto psplit = isplit.split_by_predicate([](IntNode* x){ return x->x < 3; });
    DEFER(psplit.node = nullptr);
    cnt = 0;
    for (auto x : psplit) {
        EXPECT_EQ(cnt, x->x);
        cnt++;
    }
    EXPECT_EQ(3, cnt);

}

int main(int argc, char** arg)
{
    ::testing::InitGoogleTest(&argc, arg);
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    default_audit_logger.log_output = log_output_stdout;
    photon::vcpu_init();
    set_log_output_level(ALOG_DEBUG);

    if (FLAGS_vcpus <= 1)
    {
        return RUN_ALL_TESTS();
    }

    std::vector<std::thread> ths;
    for(int i=1; i<=FLAGS_vcpus; i++) {
        ths.emplace_back([i]{
            photon::vcpu_init();
            set_log_output_level(ALOG_INFO);
            run_all_tests(i);
            photon::vcpu_fini();
        });
    }

    for (auto &x : ths) x.join();

    // return 0;
    test_sat_add();
//    test_sleepqueue();

    srand(time(0));

    //aSem.wait(1);
    aSem.wait(3);
    //aSem.wait(10);

    wait_for_completion(0);
    while(photon::vcpu_fini() != 0)
    {
        LOG_DEBUG("wait for other vCPU(s) to end");
        ::usleep(1000*1000);
    }
    LOG_DEBUG("exit");
    return 0;
}
