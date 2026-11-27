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
#include <atomic>
#include <functional>
#include <thread>
#include <unistd.h>
#include <sys/time.h>
#include <gflags/gflags.h>
#include "../../test/gtest.h"
#include <photon/common/alog-audit.h>

#define private public
#include "../thread.cpp"
#include "../thread11.h"
#include "../../test/ci-tools.h"
#include "../future.h"

#include <photon/common/lockfree_queue.h>

static void invoke_scoped_pause_outside_photon_ns(void (*fn)(void*), void* arg) {
    SCOPED_PAUSE_WORK_STEALING;
    if (fn)
        fn(arg);
}

using namespace std;
using namespace photon;

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
        printf("%llu(%d)", (unsigned long long)it->ts_wakeup, it->idx);
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
    shuffle(pops.begin(), pops.end());
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
    test_thread_switch(10, 16 * PAGE_SIZE);
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
    test_thread_switch(10, 16 * PAGE_SIZE);
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

TEST(Timer, Reapting2)
{
    timer_count = 5;
    t0 = now_time();
    auto cb = [](void* arg) -> uint64_t {
        LOG_INFO("timer callback");
        auto t1 = now_time();
        auto delta_t = t1 - t0;
        EXPECT_GT(delta_t, 1 * 1000 * 1000);
        EXPECT_LT(delta_t, 2 * 1000 * 1000);
        t0 = t1;
        LOG_INFO(VALUE(delta_t));
        LOG_INFO(VALUE(timer_count));
        auto& timer_count = *(int*)arg;
        timer_count--;
        return 0;
    };
    Timer timer(1000*1000, {cb, &timer_count});
    while(timer_count >= 0) {
        usleep(200000); // 200ms. Simulate the situation of cpu usage
        std::cout << "ready to yield" << std::endl;
        photon::thread_yield();
    }
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
    EXPECT_EQ(1244ULL, nwrites);
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

TEST(thread11, dtor)
{
    struct Foo {
        Foo() { LOG_DEBUG("ctor ", this); }
        Foo(const Foo&) { LOG_DEBUG("copy ctor ", this); }
        Foo(Foo&&) = delete;
        ~Foo() { LOG_DEBUG("dtor ", this); }
        int x = 100;
    };
    Foo foo;
    LOG_DEBUG("thread_create11");
    auto th = thread_create11([foo]{
        LOG_DEBUG(&foo, ":thread");
        LOG_DEBUG(foo.x);
    });
    auto jh = thread_enable_join(th);
    thread_join(jh);
    LOG_DEBUG("end");
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
        thread_create11(4 * PAGE_SIZE, semaphore_heavy, sem, i);
    }
    LOG_DEBUG("created ` threads", thread_num);
    sem.signal(1);
    auto ret = sem.wait(thread_num+2, 1);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ETIMEDOUT, errno);
    ret = sem.wait(thread_num+1);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0ULL, sem.m_count);
    wait_for_completion();
}

using Ring = photon::common::RingChannel<
    LockfreeSPSCRingQueue<Delegate<void>, 16>>;

void test_disposable_semaphore(Ring* ring, uint64_t count, uint64_t slat, uint64_t oplat) {
    while (count--) {
        disposable_semaphore sem;
        auto op = [oplat, &sem]() {
            if (oplat) ::usleep(oplat); // operation latency
            sem.signal();               // completion signal
        };
        ring->send(op);
        if (slat) ::usleep(slat);       // submission latency
        int ret = sem.wait(1000*1000);  // wait for completion
        EXPECT_EQ(0, ret);
        if (ret) return;
    }
}

TEST(disposable_semaphore, basic) {
    Ring ring;
    std::thread worker([&]() {
        vcpu_init();
        while(auto cb = ring.recv(0, 0)) cb();
        vcpu_fini();
    });

    test_disposable_semaphore(&ring, 10000, 0, 100);
    test_disposable_semaphore(&ring, 10000, 100, 0);
    test_disposable_semaphore(&ring, 10000, 00, 00);

    ring.send({});
    worker.join();
}

TEST(Sleep, sleep_only_thread) {    //Sleep_sleep_only_thread_Test::TestBody
    // If current thread is only thread and sleeping
    // it should be able to avoid crash during long time sleep
    auto start = photon::now;
    photon::thread_sleep(2);
    auto dt = photon::now - start;
    EXPECT_GT(dt, 1ULL*1024*1024);
    EXPECT_LE(dt, 3ULL*1024*1024);
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
                photon::thread_create(&rwlocktest, (void*)arg)
            )
        );
    }
    for (auto &x : handles)
        photon::thread_join(x);
    EXPECT_EQ(100ULL, rw_count);
}

void* interrupt(void* th) {
    photon::thread_interrupt((photon::thread*)th, EALREADY);
    return 0;
}

TEST(RWLock, interrupt) {
    rwlock rwl;
    int ret = rwl.lock(photon::WLOCK); // write lock
    EXPECT_EQ(0, ret);
    ret = rwl.lock(photon::RLOCK, 1000ULL); // it should not be locked
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
    // RUN_TEST(ThreadPool, test);
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
        {10ULL, -1ULL, -1ULL},
        {10ULL, -9ULL, -1ULL},
        {1ULL, 2ULL, 3ULL},
        {10ULL, 3ULL, 13ULL},
        {100ULL, 4ULL, 104ULL},
        {1000ULL, 5ULL, 1005ULL},
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
        {10ULL, 1ULL, 9ULL},
        {10ULL, 9ULL, 1ULL},
        {1ULL, 2ULL, 0ULL},
        {10ULL, 30ULL, 0ULL},
        {100ULL, 400ULL, 0ULL},
        {1000ULL, 5000ULL, 0ULL},
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
                    photon::thread_create(&smprwlocktest, (void*)arg)
                )
            );
        }
        for (auto &x : handles)
            photon::thread_join(x);
    });
    EXPECT_EQ(1000ULL, srw_count);
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
            EXPECT_EQ(ret, 0);
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

TEST(get, info) {
    EXPECT_EQ(get_vcpu_num(), 1);
    auto expect_info = [](uint64_t run, uint64_t standby, uint64_t sleep) {
        EXPECT_EQ(get_info(INFO_RUNNABLE_THREAD_NUM), run);
        EXPECT_EQ(get_info(INFO_STANDBY_THREAD_NUM), standby);
        EXPECT_EQ(get_info(INFO_SLEEPING_THREAD_NUM), sleep);
        EXPECT_EQ(get_info(INFO_THREAD_NUM), run + sleep);
    };
    expect_info(2, 0, 0);
    photon::condition_variable cvar;
    thread_create11([&]() { cvar.wait_no_lock(); });
    expect_info(3, 0, 0);
    thread_yield();
    expect_info(2, 0, 1);
    cvar.notify_one();
    expect_info(3, 0, 0);
    thread_yield();
    expect_info(2, 0, 0);
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
    auto n = args.n = 10ULL;
    args.sem.signal(n/2);
    std_threads_create_join(n, &photon_do,
        n*n, &test_smp_semaphore, &args);
    EXPECT_EQ(args.sem.m_count, n*n*n*n*n + n/2ULL);
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
        auto ret = photon::thread_usleep_defer(1000ULL * 1000, [&flag] {
            flag.store(true);
        });
        EXPECT_EQ(-1, ret);
        EXPECT_EQ(ENXIO, errno);
        EXPECT_LT(photon::now - start, 1000ULL * 1000);
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
    photon::thread_usleep(1ULL * 1000 * 1000);
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
    auto task = photon::thread_create({somework, photon::get_vcpu()});
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
        -1ULL, [&]{
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
    photon::thread_usleep(1ULL * 1000 * 1000);
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
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
    auto lambda = [](photon::semaphore &sem){
        sem.signal(1);
    };
    photon::thread_create11(lambda, std::ref(sem));
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
    auto lambda2 = [&sem]{
        sem.signal(1);
    };
    photon::thread_create11(lambda2);
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
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
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
    thread_create11(ivf2, 'x');
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
    thread_create11(ivf3, 'x');
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
    thread_create11(ivf3, 123);
    EXPECT_EQ(0, sem.wait(2, 1ULL*1000*1000));
    thread_create11(lambda);
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
    thread_create11(lambda2, std::ref(sem));
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
    thread_create11(lambda3);
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
    thread_create11(bind_obj);
    EXPECT_EQ(0, sem.wait(1, 1ULL*1000*1000));
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

TEST(interrupt, mutex) {
    photon::mutex mtx(0);
    // lock first
    mtx.lock();
    auto th = photon::CURRENT;
    int reason = rand();
    while (reason == 0) reason = rand();
    photon::thread_create11([th, reason]() {
        // any errno except 0 is able to stop waiting
        photon::thread_interrupt(th, reason);
    });
    // this time will goto sleep
    auto ret = mtx.lock();
    ERRNO err;
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(reason, err.no);
    mtx.unlock();
}

TEST(interrupt, condition_variable) {
    photon::condition_variable cond;
    auto th = photon::CURRENT;
    int reason = rand();
    while (reason == 0) reason = rand();
    photon::thread_create11([th, reason]() {
        // any errno except 0 is able to stop waiting
        photon::thread_interrupt(th, reason);
    });
    auto ret = cond.wait_no_lock();
    ERRNO err;
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(reason, err.no);
}

TEST(interrupt, semaphore) {
    photon::semaphore sem(0);
    auto th = photon::CURRENT;
    int reason = rand();
    while (reason == 0) reason = rand();
    photon::thread_create11([th, reason]() {
        // any errno except 0 is able to stop waiting
        photon::thread_interrupt(th, reason);
    });
    auto ret = sem.wait_interruptible(1); // nobody
    ERRNO err;
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(reason, err.no);
}


TEST(condition_variable, pred) {
    photon::condition_variable cond;
    int flag = 0;
    photon::thread_create11([&cond, &flag]() {
        // any errno except 0 is able to stop waiting
        flag = 1;
        cond.notify_one();
        // first notify should not wake up condition variable
        photon::thread_usleep(1000 * 1000);
        flag = 2;
        cond.notify_one();

    });
    auto ret = cond.wait_no_lock([&flag](){ return flag == 2;});
    EXPECT_EQ(0, ret);
    EXPECT_EQ(2, flag);
    ret = cond.wait_no_lock([&flag](){ return flag == 3; }, 1000);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ETIMEDOUT, errno);
    flag = 0;
    photon::mutex mtx;
    SCOPED_LOCK(mtx);
    photon::thread_create11([&cond, &flag, &mtx]() {
        // any errno except 0 is able to stop waiting
        {
            SCOPED_LOCK(mtx);
            flag = 1;
            cond.notify_one();
        }
        // first notify should not wake up condition variable
        photon::thread_usleep(1000 * 1000);
        {
            SCOPED_LOCK(mtx);
            flag = 2;
            cond.notify_one();
        }
    });
    ret = cond.wait(mtx, [&flag](){ return flag == 2;});
    EXPECT_EQ(0, ret);
    EXPECT_EQ(2, flag);
    ret = cond.wait(mtx, [&flag](){ return flag == 3; }, 1000);
    EXPECT_EQ(-1, ret);
    EXPECT_EQ(ETIMEDOUT, errno);
}

const int PROMISE_VALUE = 1024;
static void _promise_worker(Promise<int> promise) {
    thread_usleep(1000 * 10);
    LOG_DEBUG("set value as ", PROMISE_VALUE);
    promise.set_value(PROMISE_VALUE);
}

static void* promise_worker(void* arg) {
    auto fut = (Future<int>*) arg;
    _promise_worker(fut->get_promise());
    return 0;
}

static void* ws_basic(void* arg) {
    auto& stolen = *(bool*)arg;
    EXPECT_FALSE(stolen);
    LOG_INFO("this work (thread) has been stolen");
    stolen = true;
    return nullptr;
}

TEST(WorkStealing, basic) {
    bool running = true, ready = false, stolen = false;
    std::thread vcpu([&](){
        vcpu_init(VCPU_ENABLE_ACTIVE_WORK_STEALING);
        while(running) {
            ready = true;
            thread_usleep(1000);
        }
        DEFER(vcpu_fini());
    });
    while(!ready)
        thread_usleep(1000);
    auto th = thread_create(&ws_basic, &stolen, 0, 0,
            THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
    thread_pause_work_stealing(true, th);
    ::usleep(1000 * 10);    // emulate a busy work of 10ms
    EXPECT_FALSE(stolen);

    thread_pause_work_stealing(false, th);
    for (int i = 0; i < 100; i++) {
        if (stolen) break;
        ::usleep(1000 * 10);    // emulate a busy work of 10ms
    }
    EXPECT_TRUE(stolen);

    thread_join((join_handle*)th);
    running = false;
    vcpu.join();
}

// Busy-wait WITHOUT scheduling photon threads on this vCPU (by sleeping the
// OS thread directly), so that READY threads stay in runq/standbyq and
// remain candidates for work-stealing.
static bool wait_blocked(const std::function<bool()>& cond, uint64_t timeout_ms) {
    for (uint64_t i = 0; i < timeout_ms; i++) {
        if (cond()) return true;
        ::usleep(1000);
    }
    return cond();
}

// Wait while allowing this vCPU to schedule its own photon threads.
static bool wait_sched(const std::function<bool()>& cond, uint64_t timeout_ms) {
    for (uint64_t i = 0; i < timeout_ms * 10; i++) {
        if (cond()) return true;
        photon::thread_usleep(100);
    }
    return cond();
}

// An idling vCPU (typically an active stealer). Its main photon thread
// sleeps in 1ms slices, so its idle worker keeps trying work-stealing.
struct IdlingVCpu {
    std::thread os_thread;
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};
    vcpu_base* vcpu = nullptr;

    explicit IdlingVCpu(uint64_t flags = VCPU_ENABLE_ACTIVE_WORK_STEALING) {
        os_thread = std::thread([this, flags] {
            photon::vcpu_init(flags);
            DEFER(photon::vcpu_fini());
            vcpu = photon::get_vcpu();
            ready.store(true, std::memory_order_release);
            while (!stop.load(std::memory_order_acquire))
                photon::thread_usleep(1000);
        });
        while (!ready.load(std::memory_order_acquire))
            ::usleep(100);
    }
    ~IdlingVCpu() {
        stop.store(true, std::memory_order_release);
        os_thread.join();
    }
};

// Spawn an OS thread that initializes a vCPU with `flags`, runs `body` on
// that vCPU, and finalizes the vCPU when `body` returns.
static std::thread spawn_vcpu(uint64_t flags, std::function<void()> body) {
    return std::thread([flags, body] {
        photon::vcpu_init(flags);
        DEFER(photon::vcpu_fini());
        body();
    });
}

struct WorkerRec {
    std::atomic<bool> done{false};
    vcpu_base* ran_on = nullptr;
};

static void* record_worker(void* arg) {
    auto r = (WorkerRec*)arg;
    r->ran_on = photon::get_vcpu();
    r->done.store(true, std::memory_order_release);
    return nullptr;
}

static bool all_done(WorkerRec* recs, int n) {
    for (int i = 0; i < n; i++)
        if (!recs[i].done.load(std::memory_order_acquire)) return false;
    return true;
}

// READY threads with THREAD_ENABLE_WORK_STEALING in a busy vCPU's runq
// must be stolen by an active stealer, with correct nthreads bookkeeping.
TEST(WorkStealing, steal_from_runq) {
    auto main_vcpu = photon::get_vcpu();
    auto n0 = ((vcpu_t*)main_vcpu)->nthreads.load();
    IdlingVCpu stealer;
    constexpr int N = 8;
    WorkerRec recs[N];
    photon::thread* ths[N];
    for (int i = 0; i < N; i++) {
        ths[i] = thread_create(&record_worker, &recs[i], 0, 0,
                               THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        ASSERT_NE(nullptr, ths[i]);
    }
    ASSERT_TRUE(wait_blocked([&] { return all_done(recs, N); }, 5000));
    for (auto& r : recs)
        EXPECT_EQ(stealer.vcpu, r.ran_on);
    // the victim's thread count dropped as soon as the threads were stolen
    EXPECT_EQ(n0, ((vcpu_t*)main_vcpu)->nthreads.load());
    for (auto th : ths)
        thread_join((join_handle*)th);
    // the stealer's count returns to baseline (main + idle worker) after exits
    EXPECT_TRUE(wait_sched([&] {
        return ((vcpu_t*)stealer.vcpu)->nthreads.load() == 2;
    }, 2000));
}

TEST(WorkStealing, steal_from_runq_multiple_stealers) {
    auto main_vcpu = photon::get_vcpu();
    IdlingVCpu s1, s2;
    constexpr int N = 16;
    WorkerRec recs[N];
    photon::thread* ths[N];
    for (int i = 0; i < N; i++) {
        ths[i] = thread_create(&record_worker, &recs[i], 0, 0,
                               THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        ASSERT_NE(nullptr, ths[i]);
    }
    ASSERT_TRUE(wait_blocked([&] { return all_done(recs, N); }, 10000));
    for (auto& r : recs) {
        ASSERT_NE(nullptr, r.ran_on);
        EXPECT_NE(main_vcpu, r.ran_on);    // stolen by one of the stealers
    }
    for (auto th : ths)
        thread_join((join_handle*)th);
}

// A vCPU without VCPU_ENABLE_PASSIVE_WORK_STEALING must never be stolen from.
TEST(WorkStealing, no_steal_without_passive_flag) {
    IdlingVCpu stealer;
    auto victim = spawn_vcpu(0, [&] {
        auto my_vcpu = photon::get_vcpu();
        WorkerRec rec;
        auto th = thread_create(&record_worker, &rec, 0, 0,
                                THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        EXPECT_FALSE(wait_blocked([&] { return rec.done.load(std::memory_order_acquire); }, 300));
        photon::thread_yield();
        EXPECT_TRUE(rec.done.load(std::memory_order_acquire));
        EXPECT_EQ(my_vcpu, rec.ran_on);
        thread_join((join_handle*)th);
    });
    victim.join();
}

// A vCPU without VCPU_ENABLE_ACTIVE_WORK_STEALING must never steal.
TEST(WorkStealing, no_steal_without_active_flag) {
    IdlingVCpu idle(0);
    auto main_vcpu = photon::get_vcpu();
    WorkerRec rec;
    auto th = thread_create(&record_worker, &rec, 0, 0,
                            THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
    EXPECT_FALSE(wait_blocked([&] { return rec.done.load(std::memory_order_acquire); }, 300));
    photon::thread_yield();
    EXPECT_TRUE(rec.done.load(std::memory_order_acquire));
    EXPECT_EQ(main_vcpu, rec.ran_on);
    thread_join((join_handle*)th);
}

// A thread without THREAD_ENABLE_WORK_STEALING must never be stolen.
TEST(WorkStealing, no_steal_without_thread_flag) {
    IdlingVCpu stealer;
    auto main_vcpu = photon::get_vcpu();
    WorkerRec rec;
    auto th = thread_create(&record_worker, &rec, 0, 0, THREAD_JOINABLE);
    EXPECT_FALSE(wait_blocked([&] { return rec.done.load(std::memory_order_acquire); }, 300));
    photon::thread_yield();
    EXPECT_TRUE(rec.done.load(std::memory_order_acquire));
    EXPECT_EQ(main_vcpu, rec.ran_on);
    thread_join((join_handle*)th);
}

// thread_pause_work_stealing() suppresses and re-enables stealing.
TEST(WorkStealing, pause_work_stealing) {
    IdlingVCpu stealer;
    WorkerRec rec;
    auto th = thread_create(&record_worker, &rec, 0, 0,
                            THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
    thread_pause_work_stealing(true, th);
    EXPECT_FALSE(wait_blocked([&] { return rec.done.load(std::memory_order_acquire); }, 300));
    thread_pause_work_stealing(false, th);
    EXPECT_TRUE(wait_blocked([&] { return rec.done.load(std::memory_order_acquire); }, 5000));
    EXPECT_EQ(stealer.vcpu, rec.ran_on);
    thread_join((join_handle*)th);
}

struct ScopedRec {
    std::atomic<bool> entered{false};
    std::atomic<bool> leave{false};
    std::atomic<bool> done{false};
    vcpu_base* vcpu_in_scope = nullptr;
};

static void* scoped_worker(void* arg) {
    auto r = (ScopedRec*)arg;
    SCOPED_PAUSE_WORK_STEALING;
    r->vcpu_in_scope = photon::get_vcpu();
    r->entered.store(true, std::memory_order_release);
    while (!r->leave.load(std::memory_order_acquire))
        photon::thread_yield();
    r->done.store(true, std::memory_order_release);
    return nullptr;
}

// SCOPED_PAUSE_WORK_STEALING protects the thread inside the scope and
// restores the flags on scope exit.
TEST(WorkStealing, scoped_pause_work_stealing) {
    auto main_vcpu = photon::get_vcpu();
    ScopedRec rec;
    auto th = thread_create(&scoped_worker, &rec, 0, 0,
                            THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
    // let the thread start and enter the scope before the stealer exists,
    // so that it deterministically starts on this vCPU
    ASSERT_TRUE(wait_sched([&] { return rec.entered.load(std::memory_order_acquire); }, 1000));
    IdlingVCpu stealer;
    // the thread is READY and yielding, but paused: it must not be stolen
    for (int i = 0; i < 300; i++) {
        ::usleep(1000);
        ASSERT_EQ(main_vcpu, photon::get_vcpu(th));
        ASSERT_FALSE(rec.done.load(std::memory_order_acquire));
    }
    rec.leave.store(true, std::memory_order_release);
    ASSERT_TRUE(wait_sched([&] { return rec.done.load(std::memory_order_acquire); }, 5000));
    EXPECT_EQ(main_vcpu, rec.vcpu_in_scope);
    // the scope must have cleared the pause flag
    EXPECT_EQ(0, ((partial_thread*)th)->flags & THREAD_PAUSE_WORK_STEALING);
    thread_join((join_handle*)th);
}

// A sleeping thread (still in its owner's sleepq) must not be stolen,
// even after its wakeup time has expired.
TEST(WorkStealing, sleeping_thread_not_stolen) {
    IdlingVCpu stealer;
    auto main_vcpu = photon::get_vcpu();
    WorkerRec rec;
    auto th = thread_create(+[](void* arg) -> void* {
        auto r = (WorkerRec*)arg;
        photon::thread_usleep(200 * 1000);
        r->ran_on = photon::get_vcpu();
        r->done.store(true, std::memory_order_release);
        return nullptr;
    }, &rec, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
    photon::thread_yield();    // let it start and fall asleep
    ASSERT_EQ(1, photon::get_info(INFO_SLEEPING_THREAD_NUM, main_vcpu));
    // busy-wait past the 200ms wakeup time; the thread must stay on this vCPU
    for (int i = 0; i < 400; i++) {
        ::usleep(1000);
        ASSERT_EQ(main_vcpu, photon::get_vcpu(th));
        ASSERT_FALSE(rec.done.load(std::memory_order_acquire));
    }
    ASSERT_EQ(1, photon::get_info(INFO_SLEEPING_THREAD_NUM, main_vcpu));
    ASSERT_TRUE(wait_sched([&] { return rec.done.load(std::memory_order_acquire); }, 5000));
    // after the owner's resume pass it is a normal READY thread and may
    // legitimately be stolen before main schedules it
    EXPECT_TRUE(rec.ran_on == main_vcpu || rec.ran_on == stealer.vcpu);
    thread_join((join_handle*)th);
}

// A sleeping thread interrupted from another vCPU goes to the owner's
// standbyq while remaining in the owner's sleepq. It must not be stolen;
// the owner must resume it locally with the interrupt error.
TEST(WorkStealing, interrupted_sleeper_not_stolen_from_standbyq) {
    IdlingVCpu stealer;
    auto main_vcpu = photon::get_vcpu();
    struct Rec {
        std::atomic<bool> sleeping{false};
        std::atomic<bool> done{false};
        vcpu_base* ran_on = nullptr;
        int ret = 0;
        int err = 0;
    } rec;
    auto th = thread_create(+[](void* arg) -> void* {
        auto r = (Rec*)arg;
        r->sleeping.store(true, std::memory_order_release);
        r->ret = photon::thread_usleep(5UL * 1000 * 1000);
        r->err = errno;
        r->ran_on = photon::get_vcpu();
        r->done.store(true, std::memory_order_release);
        return nullptr;
    }, &rec, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
    ASSERT_TRUE(wait_sched([&] { return rec.sleeping.load(std::memory_order_acquire); }, 1000));
    ASSERT_EQ(1, photon::get_info(INFO_SLEEPING_THREAD_NUM, main_vcpu));
    // interrupt from a third vCPU: the thread lands on main's standbyq
    std::atomic<bool> interrupted{false};
    auto intr = spawn_vcpu(0, [&] {
        photon::thread_interrupt(th, EINTR);
        interrupted.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(wait_blocked([&] { return interrupted.load(std::memory_order_acquire); }, 1000));
    intr.join();
    ASSERT_EQ(1, photon::get_info(INFO_STANDBY_THREAD_NUM, main_vcpu));
    // busy-wait: the stealer must not steal the interrupted sleeper
    for (int i = 0; i < 300; i++) {
        ::usleep(1000);
        ASSERT_EQ(main_vcpu, photon::get_vcpu(th));
        ASSERT_FALSE(rec.done.load(std::memory_order_acquire));
    }
    // the owner resumes it locally with EINTR
    ASSERT_TRUE(wait_sched([&] { return rec.done.load(std::memory_order_acquire); }, 5000));
    EXPECT_EQ(-1, rec.ret);
    EXPECT_EQ(EINTR, rec.err);
    // after the owner's resume pass it is a normal READY thread and may
    // legitimately be stolen before main schedules it
    EXPECT_TRUE(rec.ran_on == main_vcpu || rec.ran_on == stealer.vcpu);
    EXPECT_EQ(0, photon::get_info(INFO_STANDBY_THREAD_NUM, main_vcpu));
    EXPECT_EQ(0, photon::get_info(INFO_SLEEPING_THREAD_NUM, main_vcpu));
    thread_join((join_handle*)th);
}

// A READY thread migrated into a busy vCPU's standbyq (not present in any
// sleepq) can and should be stolen from the standbyq.
TEST(WorkStealing, migrated_thread_in_standbyq_can_be_stolen) {
    IdlingVCpu stealer;
    auto main_vcpu = photon::get_vcpu();
    WorkerRec rec;
    photon::thread* th = nullptr;
    std::atomic<bool> migrated{false};
    auto spawner = spawn_vcpu(0, [&] {
        th = thread_create(&record_worker, &rec, 0, 0,
                           THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        if (photon::thread_migrate(th, main_vcpu) == 0)
            migrated.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(wait_blocked([&] { return migrated.load(std::memory_order_acquire); }, 1000));
    spawner.join();
    ASSERT_TRUE(wait_blocked([&] { return rec.done.load(std::memory_order_acquire); }, 5000));
    EXPECT_EQ(stealer.vcpu, rec.ran_on);
    thread_join((join_handle*)th);
}

TEST(WorkStealing, mixed_stealable_and_unstealable) {
    IdlingVCpu stealer;
    auto main_vcpu = photon::get_vcpu();
    constexpr int N = 4;
    WorkerRec s[N], u[N];
    photon::thread* sths[N];
    photon::thread* uths[N];
    for (int i = 0; i < N; i++) {
        sths[i] = thread_create(&record_worker, &s[i], 0, 0,
                                THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        uths[i] = thread_create(&record_worker, &u[i], 0, 0, THREAD_JOINABLE);
    }
    ASSERT_TRUE(wait_blocked([&] { return all_done(s, N); }, 5000));
    for (auto& r : s)
        EXPECT_EQ(stealer.vcpu, r.ran_on);
    for (auto& r : u)
        EXPECT_FALSE(r.done.load(std::memory_order_acquire));
    ASSERT_TRUE(wait_sched([&] { return all_done(u, N); }, 5000));
    for (auto& r : u)
        EXPECT_EQ(main_vcpu, r.ran_on);
    for (auto th : sths) thread_join((join_handle*)th);
    for (auto th : uths) thread_join((join_handle*)th);
}

// A stolen thread must live a full life-cycle on its new vCPU: sleep on the
// new vCPU's sleepq, and return a value to join() on the original vCPU.
struct LifeRec {
    std::atomic<bool> done{false};
    vcpu_base* vcpu_before_sleep = nullptr;
    vcpu_base* vcpu_after_sleep = nullptr;
};

TEST(WorkStealing, stolen_thread_full_lifecycle) {
    IdlingVCpu stealer;
    LifeRec rec;
    auto th = thread_create(+[](void* arg) -> void* {
        auto r = (LifeRec*)arg;
        r->vcpu_before_sleep = photon::get_vcpu();
        photon::thread_usleep(50 * 1000);
        r->vcpu_after_sleep = photon::get_vcpu();
        r->done.store(true, std::memory_order_release);
        return (void*)42;
    }, &rec, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
    ASSERT_TRUE(wait_blocked([&] { return rec.done.load(std::memory_order_acquire); }, 5000));
    EXPECT_EQ(stealer.vcpu, rec.vcpu_before_sleep);
    EXPECT_EQ(stealer.vcpu, rec.vcpu_after_sleep);
    auto ret = thread_join((join_handle*)th);
    EXPECT_EQ((void*)42, ret);
}

// Repeated batches of stealable work with multiple stealers; every thread
// must complete exactly once regardless of where it runs.
struct StressRec {
    WorkerRec* rec;
    uint64_t nap_us;
};

TEST(WorkStealing, stress_multiple_stealers) {
    IdlingVCpu s1, s2, s3;
    long total = 0;
    for (int round = 0; round < 20; round++) {
        constexpr int N = 8;
        WorkerRec recs[N];
        StressRec args[N];
        photon::thread* ths[N];
        for (int i = 0; i < N; i++) {
            args[i] = {&recs[i], ((i + round) % 2) ? (uint64_t)((i + 1) * 100) : 0};
            ths[i] = thread_create(+[](void* arg) -> void* {
                auto a = (StressRec*)arg;
                if (a->nap_us) photon::thread_usleep(a->nap_us);
                a->rec->ran_on = photon::get_vcpu();
                a->rec->done.store(true, std::memory_order_release);
                return nullptr;
            }, &args[i], 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
            ASSERT_NE(nullptr, ths[i]);
        }
        ASSERT_TRUE(wait_blocked([&] { return all_done(recs, N); }, 10000));
        for (auto& r : recs) {
            EXPECT_TRUE(r.done.load(std::memory_order_acquire));
            total++;
        }
        for (auto th : ths)
            thread_join((join_handle*)th);
    }
    EXPECT_EQ(20L * 8, total);
}

// Creating/destroying passive vCPUs while a stealer is active must be safe.
TEST(WorkStealing, vcpu_churn_with_active_stealer) {
    IdlingVCpu stealer;
    for (int round = 0; round < 20; round++) {
        std::atomic<bool> body_done{false};
        auto os = spawn_vcpu(VCPU_ENABLE_PASSIVE_WORK_STEALING, [&, round] {
            WorkerRec recs[2];
            photon::thread* ths[2];
            for (int i = 0; i < 2; i++)
                ths[i] = thread_create(&record_worker, &recs[i], 0, 0,
                                       THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
            if (round % 2)
                ::usleep(2000);    // leave a window for stealing
            EXPECT_TRUE(wait_sched([&] { return all_done(recs, 2); }, 5000));
            for (auto th : ths)
                thread_join((join_handle*)th);
            body_done.store(true, std::memory_order_release);
        });
        os.join();
        EXPECT_TRUE(body_done.load(std::memory_order_acquire));
    }
}

// Threads stolen from a passive vCPU's standbyq (both via the front fast-path
// in ws_scan_standbyq and via ws_scan_q behind an unstealable head) must be
// transitioned from STANDBY to READY as soon as they enter the stealer's runq.
TEST(WorkStealing, standbyq_stolen_threads_marked_ready) {
    auto main_vcpu = photon::get_vcpu();

    // Case 1: stealable threads at the front of standbyq (ws_scan_standbyq loop).
    {
        struct PairCtx {
            photon::thread* th2 = nullptr;
            photon::vcpu_base* main_vcpu = nullptr;
            std::atomic<int> th2_state_on_steal{-1};
            std::atomic<int> migrate_ret{-1};
            std::atomic<bool> th1_done{false};
            std::atomic<bool> th2_done{false};
        } ctx;
        ctx.main_vcpu = main_vcpu;

        photon::thread* th1 = nullptr;
        std::atomic<bool> migrated{false};
        auto spawner = spawn_vcpu(0, [&] {
            th1 = thread_create(+[](void* arg) -> void* {
                auto c = (PairCtx*)arg;
                c->th2_state_on_steal.store(photon::thread_stat(c->th2),
                                            std::memory_order_release);
                c->migrate_ret.store(photon::thread_migrate(c->th2, c->main_vcpu),
                                     std::memory_order_release);
                c->th1_done.store(true, std::memory_order_release);
                return nullptr;
            }, &ctx, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
            ctx.th2 = thread_create(+[](void* arg) -> void* {
                auto c = (PairCtx*)arg;
                c->th2_done.store(true, std::memory_order_release);
                return nullptr;
            }, &ctx, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
            if (photon::thread_migrate(th1, main_vcpu) == 0 &&
                photon::thread_migrate(ctx.th2, main_vcpu) == 0) {
                migrated.store(true, std::memory_order_release);
            }
        });
        ASSERT_TRUE(wait_blocked([&] { return migrated.load(std::memory_order_acquire); }, 1000));
        spawner.join();
        ASSERT_EQ(2, photon::get_info(INFO_STANDBY_THREAD_NUM, main_vcpu));

        // Start the stealer only after both threads sit in main_vcpu->standbyq so
        // both are stolen in a single batch and th1 inspects th2 before th2 runs.
        IdlingVCpu stealer;
        ASSERT_TRUE(wait_blocked([&] { return ctx.th1_done.load(std::memory_order_acquire); }, 5000));
        EXPECT_EQ(states::READY, ctx.th2_state_on_steal.load(std::memory_order_acquire));
        EXPECT_EQ(0, ctx.migrate_ret.load(std::memory_order_acquire));
        ASSERT_TRUE(wait_sched([&] { return ctx.th2_done.load(std::memory_order_acquire); }, 5000));
        thread_join((join_handle*)th1);
        thread_join((join_handle*)ctx.th2);
    }

    // Case 2: stealable threads queued behind an unstealable interrupted sleeper
    // at the head of standbyq (ws_scan_q with possibly_running == false).
    {
        struct TailCtx {
            photon::thread* th4 = nullptr;
            std::atomic<bool> sleeping{false};
            std::atomic<bool> sleeper_done{false};
            std::atomic<int> th4_state_on_steal{-1};
            std::atomic<bool> th3_done{false};
            std::atomic<bool> th4_done{false};
        } ctx;

        auto sleeper = thread_create(+[](void* arg) -> void* {
            auto c = (TailCtx*)arg;
            c->sleeping.store(true, std::memory_order_release);
            photon::thread_usleep(5UL * 1000 * 1000);
            c->sleeper_done.store(true, std::memory_order_release);
            return nullptr;
        }, &ctx, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        ASSERT_TRUE(wait_sched([&] { return ctx.sleeping.load(std::memory_order_acquire); }, 1000));

        photon::thread* th3 = nullptr;
        std::atomic<bool> queued{false};
        auto helper = spawn_vcpu(0, [&] {
            photon::thread_interrupt(sleeper, EINTR);
            th3 = thread_create(+[](void* arg) -> void* {
                auto c = (TailCtx*)arg;
                c->th4_state_on_steal.store(photon::thread_stat(c->th4),
                                            std::memory_order_release);
                c->th3_done.store(true, std::memory_order_release);
                return nullptr;
            }, &ctx, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
            ctx.th4 = thread_create(+[](void* arg) -> void* {
                auto c = (TailCtx*)arg;
                c->th4_done.store(true, std::memory_order_release);
                return nullptr;
            }, &ctx, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
            if (photon::thread_migrate(th3, main_vcpu) == 0 &&
                photon::thread_migrate(ctx.th4, main_vcpu) == 0) {
                queued.store(true, std::memory_order_release);
            }
        });
        ASSERT_TRUE(wait_blocked([&] { return queued.load(std::memory_order_acquire); }, 1000));
        helper.join();
        ASSERT_EQ(3, photon::get_info(INFO_STANDBY_THREAD_NUM, main_vcpu));

        IdlingVCpu stealer;
        ASSERT_TRUE(wait_blocked([&] {
            return ctx.th3_done.load(std::memory_order_acquire) &&
                   ctx.th4_done.load(std::memory_order_acquire);
        }, 5000));
        EXPECT_EQ(states::READY, ctx.th4_state_on_steal.load(std::memory_order_acquire));
        ASSERT_TRUE(wait_sched([&] { return ctx.sleeper_done.load(std::memory_order_acquire); }, 5000));
        thread_join((join_handle*)sleeper);
        thread_join((join_handle*)th3);
        thread_join((join_handle*)ctx.th4);
    }
}

// Read a thread's owning vCPU directly from its struct rather than through the
// TLS pointer `photon::CURRENT`: on ARM64 (mrs tpidr_el0) and macOS Mach-O TLV
// (_tlv_bootstrap), compilers mark TLS address lookup pure/const and hoist
// `&photon::CURRENT` out of loops across fiber steals/migrations.
static inline photon::vcpu_base* load_thread_vcpu(photon::thread* th) {
    return (photon::vcpu_base*)__atomic_load_n(&th->vcpu, __ATOMIC_RELAXED);
}

__attribute__((noinline))
static uint64_t ws_yield_canary_frame(photon::thread* self, uint64_t fiber_id,
                                      int depth, int iter,
                                      photon::vcpu_base* migrate_back_vcpu,
                                      photon::thread* peer) {
    volatile uint64_t canaries[8];
    for (int i = 0; i < 8; i++) {
        canaries[i] = (fiber_id * 0x9e3779b97f4a7c15ULL) ^
                      ((uint64_t)depth << 32) ^
                      ((uint64_t)iter << 8) ^ (uint64_t)i;
    }
    uint64_t acc = 0;
    if (depth > 0) {
        acc = ws_yield_canary_frame(self, fiber_id, depth - 1, iter,
                                    migrate_back_vcpu, peer);
    } else {
        // Periodically pause outside AtomicRunQ (and yield the OS CPU slice on
        // low-core CI VMs / ARM64 where stack_pages_gc is a no-op) so peer
        // stealers can acquire background_try_lock(), while keeping 15/16 of
        // iterations tight to stress the context-switch race window.
        if ((iter & 15) == 7) {
            std::this_thread::yield();
        }
        switch (iter & 7) {
            case 0:
                photon::thread_yield();
                break;
            case 1:
                photon::thread_yield_fast();
                break;
            case 2:
                if (peer && load_thread_vcpu(peer) == load_thread_vcpu(self) &&
                    photon::thread_stat(peer) == states::READY) {
                    photon::thread_yield_to(peer);
                } else {
                    photon::thread_yield();
                }
                break;
            case 3:
                if ((iter & 31) == 3)
                    photon::stack_pages_gc(self);
                else
                    photon::thread_yield();
                break;
            default:
                photon::thread_yield();
                break;
        }
        if (migrate_back_vcpu && load_thread_vcpu(self) != migrate_back_vcpu &&
            (iter & 3) == 0) {
            photon::thread_migrate(self, migrate_back_vcpu);
        }
    }
    for (int i = 0; i < 8; i++) {
        uint64_t expected = (fiber_id * 0x9e3779b97f4a7c15ULL) ^
                            ((uint64_t)depth << 32) ^
                            ((uint64_t)iter << 8) ^ (uint64_t)i;
        EXPECT_EQ(expected, canaries[i]);
        acc ^= canaries[i];
    }
    return acc;
}

// Like IdlingVCpu, but naps for 1us instead of 1ms so its idler scans victim
// queues at high frequency during concurrency race tests.
struct FastStealerVCpu {
    std::thread os_thread;
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};
    vcpu_base* vcpu = nullptr;

    explicit FastStealerVCpu(uint64_t flags = VCPU_ENABLE_ACTIVE_WORK_STEALING) {
        os_thread = std::thread([this, flags] {
            photon::vcpu_init(flags);
            DEFER(photon::vcpu_fini());
            vcpu = photon::get_vcpu();
            ready.store(true, std::memory_order_release);
            while (!stop.load(std::memory_order_acquire))
                photon::thread_usleep(1);
        });
        while (!ready.load(std::memory_order_acquire))
            ::usleep(50);
    }
    ~FastStealerVCpu() {
        stop.store(true, std::memory_order_release);
        os_thread.join();
    }
};

// Concurrently yielding, GC-ing, and self-migrating stealable fibers on a
// passive vCPU while active stealers scan its runq exercises:
//   - deferring states::READY until after _photon_switch_context_defer saves RSP,
//   - asymmetric_spinLock seq_cst mutual exclusion between AtomicRunQ and ws_scan_runq,
//   - raced single-element runq handling in thread_yield() / thread_yield_fast().
TEST(WorkStealing, concurrent_yield_and_steal) {
    auto main_vcpu = photon::get_vcpu();

    constexpr int N = 16;
    struct YieldWorkerArg {
        uint64_t id = 0;
        photon::thread* self = nullptr;
        photon::vcpu_base* home_vcpu = nullptr;
        std::atomic<bool>* stop = nullptr;
        std::atomic<uint64_t>* steals = nullptr;
        std::atomic<bool> done{false};
    };
    YieldWorkerArg args[N];
    photon::thread* ths[N];
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> steals{0};

    for (int i = 0; i < N; i++) {
        args[i].id = (uint64_t)(i + 1);
        args[i].home_vcpu = main_vcpu;
        args[i].stop = &stop;
        args[i].steals = &steals;
        ths[i] = thread_create(+[](void* arg) -> void* {
            auto a = (YieldWorkerArg*)arg;
            auto prev_vcpu = a->home_vcpu;
            for (int iter = 0; !a->stop->load(std::memory_order_relaxed); iter++) {
                ws_yield_canary_frame(a->self, a->id, 4, iter, a->home_vcpu, nullptr);
                auto cur_vcpu = load_thread_vcpu(a->self);
                if (cur_vcpu != prev_vcpu) {
                    a->steals->fetch_add(1, std::memory_order_relaxed);
                    prev_vcpu = cur_vcpu;
                }
            }
            a->done.store(true, std::memory_order_release);
            return nullptr;
        }, &args[i], 64 * 1024, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        ASSERT_NE(nullptr, ths[i]);
        args[i].self = ths[i];
    }

    FastStealerVCpu s1, s2;

    auto start = std::chrono::steady_clock::now();
    auto min_deadline = start + std::chrono::milliseconds(60);
    auto max_deadline = start + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < min_deadline ||
           (steals.load(std::memory_order_relaxed) == 0 &&
            std::chrono::steady_clock::now() < max_deadline)) {
        photon::thread_yield();
    }
    stop.store(true, std::memory_order_release);

    ASSERT_TRUE(wait_sched([&] {
        for (int i = 0; i < N; i++)
            if (!args[i].done.load(std::memory_order_acquire))
                return false;
        return true;
    }, 5000));

    for (int i = 0; i < N; i++)
        thread_join((join_handle*)ths[i]);
    EXPECT_GT(steals.load(std::memory_order_relaxed), 0U);
}

// Multiple vCPUs with both VCPU_ENABLE_ACTIVE_WORK_STEALING and
// VCPU_ENABLE_PASSIVE_WORK_STEALING enabled must be able to steal from each
// other without ABBA deadlock between runq_lock and vcpu_list_lock in idler(),
// and without corrupting stacks when fibers bounce across vCPUs mid-yield.
TEST(WorkStealing, active_passive_mutual_stealing) {
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 1000; i++) {
            if (finished.load(std::memory_order_acquire))
                return;
            ::usleep(10 * 1000);
        }
        LOG_ERROR("deadlock detected in WorkStealing.active_passive_mutual_stealing");
        std::_Exit(1);
    });
    DEFER({
        finished.store(true, std::memory_order_release);
        watchdog.join();
    });

    auto main_vcpu = photon::get_vcpu();
    constexpr int N = 24;
    struct MutualArg {
        uint64_t id = 0;
        photon::thread* self = nullptr;
        photon::vcpu_base* initial_vcpu = nullptr;
        photon::thread** peers = nullptr;
        int peer_idx = 0;
        std::atomic<bool>* stop = nullptr;
        std::atomic<uint64_t>* hops = nullptr;
        std::atomic<uint64_t>* mutual_hops = nullptr;
        std::atomic<bool> done{false};
    };
    MutualArg args[N];
    photon::thread* ths[N] = {};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> hops{0};
    std::atomic<uint64_t> mutual_hops{0};

    // Populate all stealable fibers before launching the ACTIVE|PASSIVE workers
    // so self and peers[] are fully initialized before any fiber can run.
    for (int i = 0; i < N; i++) {
        args[i].id = (uint64_t)(i + 100);
        args[i].initial_vcpu = main_vcpu;
        args[i].peers = ths;
        args[i].peer_idx = (i + 1) % N;
        args[i].stop = &stop;
        args[i].hops = &hops;
        args[i].mutual_hops = &mutual_hops;
        ths[i] = thread_create(+[](void* arg) -> void* {
            auto a = (MutualArg*)arg;
            auto prev_vcpu = a->initial_vcpu;
            photon::thread* peer = a->peers[a->peer_idx];
            for (int iter = 0; !a->stop->load(std::memory_order_relaxed); iter++) {
                ws_yield_canary_frame(a->self, a->id, 4, iter, nullptr, peer);
                auto cur_vcpu = load_thread_vcpu(a->self);
                if (cur_vcpu != prev_vcpu) {
                    a->hops->fetch_add(1, std::memory_order_relaxed);
                    if (prev_vcpu != a->initial_vcpu && cur_vcpu != a->initial_vcpu)
                        a->mutual_hops->fetch_add(1, std::memory_order_relaxed);
                    prev_vcpu = cur_vcpu;
                }
            }
            a->done.store(true, std::memory_order_release);
            return nullptr;
        }, &args[i], 64 * 1024, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        ASSERT_NE(nullptr, ths[i]);
        args[i].self = ths[i];
    }

    constexpr uint64_t BOTH_WS = VCPU_ENABLE_ACTIVE_WORK_STEALING |
                                 VCPU_ENABLE_PASSIVE_WORK_STEALING;
    FastStealerVCpu w1(BOTH_WS), w2(BOTH_WS), w3(BOTH_WS);

    auto start = std::chrono::steady_clock::now();
    auto min_deadline = start + std::chrono::milliseconds(60);
    auto max_deadline = start + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < min_deadline ||
           (mutual_hops.load(std::memory_order_relaxed) == 0 &&
            std::chrono::steady_clock::now() < max_deadline)) {
        photon::thread_usleep(1000);
    }
    stop.store(true, std::memory_order_release);

    ASSERT_TRUE(wait_sched([&] {
        for (int i = 0; i < N; i++)
            if (!args[i].done.load(std::memory_order_acquire))
                return false;
        return true;
    }, 5000));

    for (int i = 0; i < N; i++)
        thread_join((join_handle*)ths[i]);
    EXPECT_GT(hops.load(std::memory_order_relaxed), 0U);
    EXPECT_GT(mutual_hops.load(std::memory_order_relaxed), 0U);
}

// Nested SCOPED_PAUSE_WORK_STEALING scopes (and an inner SCOPED_PAUSE_WORK_STEALING
// inside an outer thread_pause_work_stealing(true), as happens when I/O engines
// use SCOPED_PAUSE_WORK_STEALING internally) must restore the prior pause bit
// rather than unconditionally clearing it on inner scope exit.
TEST(WorkStealing, nested_scoped_pause_work_stealing) {
    auto main_vcpu = photon::get_vcpu();

    // Case 1: nested SCOPED_PAUSE_WORK_STEALING scopes (including a helper
    // compiled outside `namespace photon`).
    {
        struct NestedCtx {
            photon::thread* self = nullptr;
            std::atomic<int> phase{0};
            std::atomic<bool> release_outer{false};
            std::atomic<bool> done{false};
            uint32_t flags_after_inner = 0;
            vcpu_base* vcpu_after_inner = nullptr;
            vcpu_base* vcpu_after_outer = nullptr;
        } ctx;

        auto th = thread_create(+[](void* arg) -> void* {
            auto c = (NestedCtx*)arg;
            {
                SCOPED_PAUSE_WORK_STEALING;
                invoke_scoped_pause_outside_photon_ns(
                    +[](void*) {
                        SCOPED_PAUSE_WORK_STEALING;
                        photon::thread_yield();
                    },
                    nullptr);
                c->flags_after_inner = ((partial_thread*)c->self)->flags;
                c->phase.store(1, std::memory_order_release);
                while (!c->release_outer.load(std::memory_order_acquire))
                    photon::thread_yield();
                c->vcpu_after_inner = load_thread_vcpu(c->self);
            }
            c->phase.store(2, std::memory_order_release);
            photon::thread_yield();
            c->vcpu_after_outer = load_thread_vcpu(c->self);
            c->done.store(true, std::memory_order_release);
            return nullptr;
        }, &ctx, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        ASSERT_NE(nullptr, th);
        ctx.self = th;
        DEFER({
            ctx.release_outer.store(true, std::memory_order_release);
            wait_sched([&] { return ctx.done.load(std::memory_order_acquire); }, 5000);
            thread_join((join_handle*)th);
        });

        ASSERT_TRUE(wait_sched([&] { return ctx.phase.load(std::memory_order_acquire) == 1; }, 1000));
        EXPECT_NE(0U, ctx.flags_after_inner & THREAD_PAUSE_WORK_STEALING);

        FastStealerVCpu stealer;
        // Block main_vcpu while th is READY inside the outer scope (after inner exited):
        // th must NOT be stolen.
        for (int i = 0; i < 100; i++) {
            ::usleep(1000);
            ASSERT_EQ(main_vcpu, load_thread_vcpu(th));
        }

        ctx.release_outer.store(true, std::memory_order_release);
        // Schedule until outer scope exits (phase == 2), then block main_vcpu so
        // stealer steals th now that the outer scope has restored pause=false.
        while (ctx.phase.load(std::memory_order_acquire) < 2)
            photon::thread_yield();
        EXPECT_EQ(main_vcpu, ctx.vcpu_after_inner);
        ASSERT_TRUE(wait_blocked([&] { return ctx.done.load(std::memory_order_acquire); }, 5000));
        EXPECT_EQ(stealer.vcpu, ctx.vcpu_after_outer);
        EXPECT_EQ(0U, ((partial_thread*)th)->flags & THREAD_PAUSE_WORK_STEALING);
    }

    // Case 2: explicit thread_pause_work_stealing(true) + inner SCOPED_PAUSE_WORK_STEALING.
    {
        struct ManualOuterCtx {
            photon::thread* self = nullptr;
            std::atomic<bool> inner_done{false};
            std::atomic<bool> unpause{false};
            std::atomic<bool> done{false};
            uint32_t flags_after_inner = 0;
            vcpu_base* final_vcpu = nullptr;
        } ctx;

        auto th = thread_create(+[](void* arg) -> void* {
            auto c = (ManualOuterCtx*)arg;
            thread_pause_work_stealing(true);
            {
                SCOPED_PAUSE_WORK_STEALING;
                photon::thread_yield();
            }
            c->flags_after_inner = ((partial_thread*)c->self)->flags;
            c->inner_done.store(true, std::memory_order_release);
            while (!c->unpause.load(std::memory_order_acquire))
                photon::thread_yield();
            thread_pause_work_stealing(false);
            photon::thread_yield();
            c->final_vcpu = load_thread_vcpu(c->self);
            c->done.store(true, std::memory_order_release);
            return nullptr;
        }, &ctx, 0, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        ASSERT_NE(nullptr, th);
        ctx.self = th;
        DEFER({
            ctx.unpause.store(true, std::memory_order_release);
            wait_sched([&] { return ctx.done.load(std::memory_order_acquire); }, 5000);
            thread_join((join_handle*)th);
        });

        ASSERT_TRUE(wait_sched([&] { return ctx.inner_done.load(std::memory_order_acquire); }, 1000));
        EXPECT_NE(0U, ctx.flags_after_inner & THREAD_PAUSE_WORK_STEALING);

        FastStealerVCpu stealer;
        for (int i = 0; i < 100; i++) {
            ::usleep(1000);
            ASSERT_EQ(main_vcpu, load_thread_vcpu(th));
        }

        ctx.unpause.store(true, std::memory_order_release);
        photon::thread_yield();
        ASSERT_TRUE(wait_blocked([&] { return ctx.done.load(std::memory_order_acquire); }, 5000));
        EXPECT_EQ(stealer.vcpu, ctx.final_vcpu);
        EXPECT_EQ(0U, ((partial_thread*)th)->flags & THREAD_PAUSE_WORK_STEALING);
    }
}

// thread_yield_to(th) when th is in states::STANDBY on CURRENT's vCPU (and
// thread_migrate(th, stealer_vcpu) when th is READY) must tolerate concurrent
// stealing of th from standbyq or runq without asserting on th->state or
// passing a changed th->get_vcpu() to resume_threads().
TEST(WorkStealing, yield_to_standby_and_migrate_race_with_stealer) {
    set_log_output_level(ALOG_FATAL);
    DEFER(set_log_output_level(ALOG_INFO));

    auto main_vcpu = photon::get_vcpu();
    IdlingVCpu park_vcpu(0);

    // Deterministic validation: yield_to rejects a SLEEPING thread on current vCPU
    // and rejects a thread owned by another vCPU with EINVAL.
    {
        std::atomic<bool> sleeping{false};
        auto sleeper = thread_create(+[](void* arg) -> void* {
            ((std::atomic<bool>*)arg)->store(true, std::memory_order_release);
            photon::thread_usleep(50 * 1000);
            return nullptr;
        }, &sleeping, 64 * 1024, 0, THREAD_JOINABLE);
        ASSERT_TRUE(wait_sched([&] { return sleeping.load(std::memory_order_acquire); }, 1000));
        errno = 0;
        EXPECT_EQ(-1, photon::thread_yield_to(sleeper));
        EXPECT_EQ(EINVAL, errno);
        thread_join((join_handle*)sleeper);

        WorkerRec foreign_rec;
        auto foreign = thread_create(&record_worker, &foreign_rec, 64 * 1024, 0, THREAD_JOINABLE);
        ASSERT_EQ(0, photon::thread_migrate(foreign, park_vcpu.vcpu));
        errno = 0;
        EXPECT_EQ(-1, photon::thread_yield_to(foreign));
        EXPECT_EQ(EINVAL, errno);
        ASSERT_TRUE(wait_sched([&] { return foreign_rec.done.load(std::memory_order_acquire); }, 2000));
        thread_join((join_handle*)foreign);
    }

    FastStealerVCpu s1, s2;

    constexpr int ROUNDS = 300;
    for (int r = 0; r < ROUNDS; r++) {
        struct TargetCtx {
            photon::vcpu_base* park_vcpu = nullptr;
            bool migrate_to_park = false;
            std::atomic<bool> done{false};
        } ctx;
        ctx.park_vcpu = park_vcpu.vcpu;
        ctx.migrate_to_park = (r & 1) != 0;

        photon::thread* target = nullptr;
        std::atomic<bool> ready_in_standby{false};
        auto producer = spawn_vcpu(0, [&] {
            target = thread_create(+[](void* arg) -> void* {
                auto c = (TargetCtx*)arg;
                // Hold states::RUNNING briefly or migrate to another vCPU's
                // standbyq right while main_vcpu may be inside thread_yield_to().
                photon::spin_wait_n(512);
                if (c->migrate_to_park && photon::get_vcpu() != c->park_vcpu) {
                    photon::thread_migrate(photon::CURRENT, c->park_vcpu);
                }
                c->done.store(true, std::memory_order_release);
                return nullptr;
            }, &ctx, 64 * 1024, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
            if (photon::thread_migrate(target, main_vcpu) == 0) {
                ready_in_standby.store(true, std::memory_order_release);
            }
        });
        while (!ready_in_standby.load(std::memory_order_acquire))
            photon::spin_wait();
        producer.join();

        if (load_thread_vcpu(target) == main_vcpu) {
            auto st = photon::thread_stat(target);
            if (st == states::STANDBY || st == states::READY) {
                photon::thread_yield_to(target);
            }
        }

        // Also exercise non-current thread_migrate(th2, s1.vcpu) racing with s1
        // stealing th2 from main_vcpu's runq.
        WorkerRec mrec;
        auto th2 = thread_create(&record_worker, &mrec, 64 * 1024, 0,
                                 THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        ASSERT_NE(nullptr, th2);
        photon::spin_wait_n(64);
        if (load_thread_vcpu(th2) == main_vcpu &&
            photon::thread_stat(th2) == states::READY) {
            photon::thread_migrate(th2, s1.vcpu);
        }

        ASSERT_TRUE(wait_sched([&] {
            return ctx.done.load(std::memory_order_acquire) &&
                   mrec.done.load(std::memory_order_acquire);
        }, 5000));
        thread_join((join_handle*)target);
        thread_join((join_handle*)th2);
    }
}

__attribute__((noinline))
static uint64_t ws_deep_stack_canary(int depth, uint64_t seed) {
    volatile uint64_t page_words[512]; // 4KB per frame
    for (int i = 0; i < 512; i += 64) {
        page_words[i] = seed ^ ((uint64_t)depth << 32) ^ (uint64_t)i;
    }
    uint64_t acc = 0;
    if (depth > 0) {
        acc = ws_deep_stack_canary(depth - 1, seed);
    } else {
        photon::spin_wait_n(256);
    }
    for (int i = 0; i < 512; i += 64) {
        uint64_t expected = seed ^ ((uint64_t)depth << 32) ^ (uint64_t)i;
        EXPECT_EQ(expected, page_words[i]);
        acc ^= page_words[i];
    }
    return acc;
}

// stack_pages_gc(th) on a non-current READY/STANDBY thread must prevent th
// from being stolen until madvise(MADV_DONTNEED) finishes; otherwise a stealer
// can resume th and grow its live stack below the stale saved RSP while
// MADV_DONTNEED zeroes those live stack pages.
TEST(WorkStealing, concurrent_non_current_stack_pages_gc_and_steal) {
    set_log_output_level(ALOG_FATAL);
    DEFER(set_log_output_level(ALOG_INFO));

    auto main_vcpu = photon::get_vcpu();

#if !defined(__aarch64__) && defined(__linux__)
    // Deterministic check: non-current stack_pages_gc(th) on a READY thread
    // reclaims unused stack pages above saved RSP without switching to th, and
    // rejects nullptr / DONE / foreign-vCPU targets with EINVAL.
    {
        errno = 0;
        EXPECT_EQ(-1, photon::stack_pages_gc(nullptr));
        EXPECT_EQ(EINVAL, errno);

        struct NonCurrentGcCtx {
            char* dirty_page = nullptr;
            bool yielded = false;
        } gctx;
        auto th = thread_create(+[](void* arg) -> void* {
            auto c = (NonCurrentGcCtx*)arg;
            recursive_call(256);
            c->dirty_page = topbuf;
            c->yielded = true;
            photon::thread_yield();
            return nullptr;
        }, &gctx, 0, 0, THREAD_JOINABLE);
        photon::thread_yield_to(th);
        ASSERT_TRUE(gctx.yielded);
        ASSERT_EQ(states::READY, photon::thread_stat(th));
        EXPECT_STREQ("Hello, stack", gctx.dirty_page);
        EXPECT_EQ(0, photon::stack_pages_gc(th));
        EXPECT_STRNE("Hello, stack", gctx.dirty_page);
        photon::thread_yield_to(th);
        ASSERT_EQ(states::DONE, photon::thread_stat(th));
        errno = 0;
        EXPECT_EQ(-1, photon::stack_pages_gc(th));
        EXPECT_EQ(EINVAL, errno);
        thread_join((join_handle*)th);
    }
#endif

    constexpr int N = 8;
    struct GcTargetArg {
        uint64_t id = 0;
        photon::thread* self = nullptr;
        photon::vcpu_base* home_vcpu = nullptr;
        std::atomic<bool>* stop = nullptr;
        std::atomic<uint64_t>* steals = nullptr;
        std::atomic<bool> done{false};
    };
    GcTargetArg args[N];
    photon::thread* ths[N];
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> steals{0};

    for (int i = 0; i < N; i++) {
        args[i].id = 0xabc000ULL + (uint64_t)i;
        args[i].home_vcpu = main_vcpu;
        args[i].stop = &stop;
        args[i].steals = &steals;
        ths[i] = thread_create(+[](void* arg) -> void* {
            auto a = (GcTargetArg*)arg;
            for (uint64_t iter = 0; !a->stop->load(std::memory_order_relaxed); iter++) {
                // Yield at shallow stack depth (high saved RSP), then immediately
                // recurse 16 * 4KB = 64KB deeper when resumed (possibly on a stealer).
                photon::thread_yield();
                ws_deep_stack_canary(16, (a->id << 20) ^ (iter + 1));
                if (load_thread_vcpu(a->self) != a->home_vcpu) {
                    a->steals->fetch_add(1, std::memory_order_relaxed);
                    photon::thread_migrate(a->self, a->home_vcpu);
                }
            }
            a->done.store(true, std::memory_order_release);
            return nullptr;
        }, &args[i], 128 * 1024, 0, THREAD_ENABLE_WORK_STEALING | THREAD_JOINABLE);
        ASSERT_NE(nullptr, ths[i]);
        args[i].self = ths[i];
    }

    FastStealerVCpu s1, s2;

    auto start = std::chrono::steady_clock::now();
    auto min_deadline = start + std::chrono::milliseconds(100);
    auto max_deadline = start + std::chrono::milliseconds(3000);
    uint64_t gc_calls = 0;
    while (std::chrono::steady_clock::now() < min_deadline ||
           (steals.load(std::memory_order_relaxed) == 0 &&
            std::chrono::steady_clock::now() < max_deadline)) {
        for (int i = 0; i < N; i++) {
            if (load_thread_vcpu(ths[i]) == main_vcpu &&
                photon::thread_stat(ths[i]) != states::RUNNING) {
                if (photon::stack_pages_gc(ths[i]) == 0)
                    gc_calls++;
            }
        }
        photon::thread_yield();
    }
    stop.store(true, std::memory_order_release);

    ASSERT_TRUE(wait_sched([&] {
        for (int i = 0; i < N; i++)
            if (!args[i].done.load(std::memory_order_acquire))
                return false;
        return true;
    }, 5000));

    for (int i = 0; i < N; i++)
        thread_join((join_handle*)ths[i]);
    EXPECT_GT(steals.load(std::memory_order_relaxed), 0U);
    EXPECT_GT(gc_calls, 0U);
}

TEST(future, test1) {
    Future<int> fut;
    auto th = thread_create(promise_worker, &fut);
    auto jh = thread_enable_join(th);
    thread_usleep(1000 * 4);
    LOG_DEBUG("before getting the value from future");
    fut.wait();
    auto v = fut.get_value();
    LOG_DEBUG("got value ` from worker via promise/future", v);
    EXPECT_EQ(v, PROMISE_VALUE);
    thread_join(jh);
}

static std::shared_ptr<Future<int>> get_future() {
    auto fut = std::make_shared<Future<int>>();
    thread_create11(_promise_worker, fut->get_promise());
    thread_usleep(1000 * 4);
    return fut;
}

TEST(future, test2) {
    auto fut = get_future();
    LOG_DEBUG("before getting the value from future");
    auto v = fut->get_value();;
    LOG_DEBUG("got value ` from worker via promise/future", v);
    EXPECT_EQ(v, PROMISE_VALUE);
}

TEST(qrwlock, BasicLockUnlock) {
    qrwlock mtx;

    // Test exclusive lock
    EXPECT_EQ(0, mtx.lock(WLOCK));
    mtx.unlock();

    // Test shared lock
    EXPECT_EQ(0, mtx.lock(RLOCK));
    mtx.unlock();
}

TEST(qrwlock, TryLock) {
    qrwlock mtx;

    // Try lock should succeed when unlocked
    EXPECT_TRUE(mtx.try_lock(WLOCK) == 0);
    // Try lock should fail when write-locked
    EXPECT_FALSE(mtx.try_lock(WLOCK) == 0);
    EXPECT_FALSE(mtx.try_lock(RLOCK) == 0);
    mtx.unlock();

    // Try shared lock should succeed when unlocked
    EXPECT_TRUE(mtx.try_lock(RLOCK) == 0);
    // Additional shared locks should succeed
    EXPECT_TRUE(mtx.try_lock(RLOCK) == 0);
    // Write lock should fail when read-locked
    EXPECT_FALSE(mtx.try_lock(WLOCK) == 0);
    mtx.unlock();
    mtx.unlock();
}

TEST(qrwlock, MultipleReaders) {
    qrwlock mtx;
    constexpr int NUM_READERS = 100;

    // Acquire many shared locks
    for (int i = 0; i < NUM_READERS; i++) {
        EXPECT_TRUE(mtx.try_lock(RLOCK) == 0);
    }

    // Write lock should fail
    EXPECT_FALSE(mtx.try_lock(WLOCK) == 0);

    // Release all shared locks
    for (int i = 0; i < NUM_READERS; i++) {
        mtx.unlock();
    }

    // Now write lock should succeed
    EXPECT_TRUE(mtx.try_lock(WLOCK) == 0);
    mtx.unlock();
}

TEST(qrwlock, ConcurrentReaders) {
    qrwlock mtx;
    std::atomic<int> active_readers{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<uint64_t> total_ops{0};
    constexpr int NUM_THREADS = 32;
    constexpr int OPS_PER_THREAD = 1000;

    std::vector<photon::join_handle*> handles;
    for (int i = 0; i < NUM_THREADS; i++) {
        handles.emplace_back(photon::thread_enable_join(
            photon::thread_create11([&]() {
                for (int j = 0; j < OPS_PER_THREAD; j++) {
                    mtx.lock(RLOCK);
                    int cur = active_readers.fetch_add(1, std::memory_order_relaxed) + 1;
                    // Track max concurrent readers
                    int prev_max = max_concurrent.load(std::memory_order_relaxed);
                    while (cur > prev_max &&
                           !max_concurrent.compare_exchange_weak(prev_max, cur)) {}
                    photon::thread_yield();
                    active_readers.fetch_sub(1, std::memory_order_relaxed);
                    mtx.unlock();
                    total_ops.fetch_add(1, std::memory_order_relaxed);
                }
            })));
    }

    for (auto& h : handles) {
        photon::thread_join(h);
    }

    EXPECT_EQ(0, active_readers.load());
    EXPECT_GT(max_concurrent.load(), 1);  // Should have concurrent readers
    EXPECT_EQ(NUM_THREADS * OPS_PER_THREAD, total_ops.load());
    LOG_INFO("Max concurrent readers: `", max_concurrent.load());
}

TEST(qrwlock, ReaderWriterExclusion) {
    qrwlock mtx;
    std::atomic<bool> writing{false};
    std::atomic<int> active_readers{0};
    std::atomic<uint64_t> read_ops{0};
    std::atomic<uint64_t> write_ops{0};
    constexpr int NUM_READERS = 16;
    constexpr int NUM_WRITERS = 4;
    constexpr int OPS_PER_THREAD = 500;

    std::vector<photon::join_handle*> handles;

    // Reader threads
    for (int i = 0; i < NUM_READERS; i++) {
        handles.emplace_back(photon::thread_enable_join(
            photon::thread_create11([&]() {
                for (int j = 0; j < OPS_PER_THREAD; j++) {
                    mtx.lock(RLOCK);
                    active_readers.fetch_add(1, std::memory_order_relaxed);
                    EXPECT_FALSE(writing.load(std::memory_order_acquire));
                    photon::thread_yield();
                    EXPECT_FALSE(writing.load(std::memory_order_acquire));
                    active_readers.fetch_sub(1, std::memory_order_relaxed);
                    mtx.unlock();
                    read_ops.fetch_add(1, std::memory_order_relaxed);
                }
            })));
    }

    // Writer threads
    for (int i = 0; i < NUM_WRITERS; i++) {
        handles.emplace_back(photon::thread_enable_join(
            photon::thread_create11([&]() {
                for (int j = 0; j < OPS_PER_THREAD; j++) {
                    mtx.lock(WLOCK);
                    EXPECT_EQ(0, active_readers.load(std::memory_order_acquire));
                    writing.store(true, std::memory_order_release);
                    photon::thread_yield();
                    EXPECT_EQ(0, active_readers.load(std::memory_order_acquire));
                    writing.store(false, std::memory_order_release);
                    mtx.unlock();
                    write_ops.fetch_add(1, std::memory_order_relaxed);
                }
            })));
    }

    for (auto& h : handles) {
        photon::thread_join(h);
    }

    EXPECT_EQ(NUM_READERS * OPS_PER_THREAD, read_ops.load());
    EXPECT_EQ(NUM_WRITERS * OPS_PER_THREAD, write_ops.load());
    LOG_INFO("Read ops: `, Write ops: `", read_ops.load(), write_ops.load());
}

TEST(qrwlock, LockTimeout) {
    qrwlock mtx;

    // Acquire write lock in main coroutine
    EXPECT_EQ(0, mtx.lock(WLOCK));

    // Try to acquire read lock with timeout in another coroutine
    auto jh = photon::thread_enable_join(
        photon::thread_create11([&]() {
            EXPECT_EQ(-1, mtx.lock(RLOCK, 100 * 1000));
            EXPECT_EQ(ETIMEDOUT, errno);
        }));

    // Must yield to let the waiter run and timeout
    photon::thread_usleep(200 * 1000);  // Sleep longer than timeout

    photon::thread_join(jh);
    mtx.unlock();
}

TEST(qrwlock, WriteLockTimeout) {
    qrwlock mtx;

    // Acquire read lock in main coroutine
    EXPECT_EQ(0, mtx.lock(RLOCK));

    // Try to acquire write lock with timeout in another coroutine
    auto jh = photon::thread_enable_join(
        photon::thread_create11([&]() {
            EXPECT_EQ(-1, mtx.lock(WLOCK, 100 * 1000));
            EXPECT_EQ(ETIMEDOUT, errno);
        }));

    // Must yield to let the waiter run and timeout
    photon::thread_usleep(200 * 1000);  // Sleep longer than timeout

    photon::thread_join(jh);
    mtx.unlock();
}

int main(int argc, char** arg)
{
    if (!photon::is_using_default_engine()) return 0;
    srand(time(nullptr));
    ::testing::InitGoogleTest(&argc, arg);
    gflags::ParseCommandLineFlags(&argc, &arg, true);
    default_audit_logger.log_output = log_output_stdout;
    photon::vcpu_init(VCPU_ENABLE_PASSIVE_WORK_STEALING);
    set_log_output_level(ALOG_INFO);

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
