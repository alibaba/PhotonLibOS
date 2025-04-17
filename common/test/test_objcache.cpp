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

#include "../expirecontainer.h"
#include "../objectcachev2.h"

#undef private
#undef protected

#include <thread>
#include <gtest/gtest.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include "../../test/ci-tools.h"

static int thread_local release_cnt = 0;
struct ShowOnDtor {
    int id;

    ShowOnDtor(int id) : id(id) { LOG_INFO("Ctor ", VALUE(id)); }

    ~ShowOnDtor() {
        LOG_INFO("Dtor ", VALUE(id));
        release_cnt++;
    }
};
struct OCArg {
    ObjectCache<int, ShowOnDtor*>* oc;
    int id;
};

static thread_local int cycle_cnt = 0;

void* objcache(void* arg) {
    auto args = (OCArg*)arg;
    auto oc = args->oc;
    auto id = args->id;
    auto ctor = [&]() {
        cycle_cnt++;
        return new ShowOnDtor(id);
    };
    // acquire every 10ms
    photon::thread_usleep(10 * 1000UL * id);
    auto ret = oc->acquire(0, ctor);
    LOG_DEBUG("Acquired ", VALUE(id));
    EXPECT_NE(nullptr, ret);
    // object holds for 50ms
    photon::thread_usleep(50 * 1000UL);
    if (id % 10 == 0) LOG_INFO("Cycle ", VALUE(id));
    // every 10 objs will recycle
    oc->release(0, id % 10 == 0);
    LOG_DEBUG("Released ", VALUE(id));
    return 0;
}

TEST(ObjectCache, release_cycle) {
    set_log_output_level(ALOG_INFO);
    DEFER(set_log_output_level(ALOG_DEBUG));
    ObjectCache<int, ShowOnDtor*> ocache(1000UL * 1000 * 10);
    // 10s, during the test, nothing will be free if not set recycle
    std::vector<photon::join_handle*> handles;
    std::vector<OCArg> args;
    cycle_cnt = 0;
    release_cnt = 0;
    for (int i = 0; i < 100; i++) {
        args.emplace_back(OCArg({&ocache, i + 1}));
    }
    for (auto& arg : args) {
        handles.emplace_back(
            photon::thread_enable_join(photon::thread_create(&objcache, &arg)));
    }
    for (const auto& handle : handles) {
        photon::thread_join(handle);
    }
    EXPECT_EQ(cycle_cnt, release_cnt);
}

TEST(ObjectCache, timeout_refresh) {
    release_cnt = 0;
    set_log_output_level(ALOG_INFO);
    DEFER(set_log_output_level(ALOG_DEBUG));
    ObjectCache<int, ShowOnDtor*> ocache(1000UL * 1000);
    // 1s
    auto ctor = [] { return new ShowOnDtor(0); };
    auto ret = ocache.acquire(0, ctor); (void)ret;
    photon::thread_usleep(1100UL * 1000);
    ocache.expire();
    ocache.release(0);
    EXPECT_EQ(0, release_cnt);
    ocache.expire();
    EXPECT_EQ(0, release_cnt);
    photon::thread_usleep(1100UL * 1000);
    ocache.expire();
    EXPECT_EQ(1, release_cnt);
}
struct ph_arg {
    ObjectCache<int, ShowOnDtor *> *ocache;
    photon::semaphore *sem;
};

void *ph_act(void *arg) {
    auto a = (ph_arg *)arg;
    DEFER(a->sem->signal(1));
    auto ctor = [] {
      photon::thread_usleep(1000);
      return nullptr;
    };
    a->ocache->acquire(0, ctor);
    return nullptr;
}

TEST(ObjectCache, ctor_may_yield_and_null) {
    release_cnt = 0;
    set_log_output_level(ALOG_INFO);
    DEFER(set_log_output_level(ALOG_DEBUG));
    ObjectCache<int, ShowOnDtor*> ocache(1000UL * 1000);
    photon::semaphore sem(0);
    ph_arg a{&ocache, &sem};
    // 1s
    for (int i = 0; i < 10; i++) {
        photon::thread_create(&ph_act, &a);
    }
    sem.wait(10);
    EXPECT_EQ(1UL, ocache._set.size());
    ocache.expire();
    photon::thread_usleep(1100UL * 1000);
    ocache.expire();
    EXPECT_EQ(0UL, ocache._set.size());
}

TEST(ObjectCache, multithread) {
    set_log_output_level(ALOG_INFO);
    DEFER(set_log_output_level(ALOG_DEBUG));
    ObjectCache<int, ShowOnDtor*> ocache(1000UL * 1000 * 10);
    cycle_cnt = 0;
    release_cnt = 0;
    // 10s, during the test, nothing will be free if not set recycle
    std::vector<std::thread> ths;
    for (int i = 0; i < 10; i++) {
        ths.emplace_back([&] {
            photon::vcpu_init();
            DEFER(photon::vcpu_fini());
            std::vector<photon::join_handle*> handles;
            std::vector<OCArg> args;
            for (int i = 0; i < 100; i++) {
                args.emplace_back(OCArg({&ocache, i + 1}));
            }
            for (auto& arg : args) {
                handles.emplace_back(photon::thread_enable_join(
                    photon::thread_create(&objcache, &arg)));
            }
            for (const auto& handle : handles) {
                photon::thread_join(handle);
            }
        });
    }
    for (auto& x : ths) {
        x.join();
    }

    EXPECT_EQ(cycle_cnt, release_cnt);
}

void* objcache_borrow(void* arg) {
    auto args = (OCArg*)arg;
    auto oc = args->oc;
    auto id = args->id;
    auto ctor = [&]() { return new ShowOnDtor(id); };
    // acquire every 10ms
    photon::thread_usleep(10 * 1000UL * id);
    {
        auto ret = oc->borrow(0, ctor);
        LOG_DEBUG("Acquired ", VALUE(id));
        EXPECT_TRUE(ret);
        // object holds for 50ms
        photon::thread_usleep(50 * 1000UL);
        if (id % 10 == 0) {
            LOG_INFO("Cycle ", VALUE(id));
            ret.recycle(true);
        }
    }
    // every 10 objs will recycle
    if (id % 10 == 0) cycle_cnt++;
    LOG_DEBUG("Released ", VALUE(id));
    return 0;
}

TEST(ObjectCache, borrow) {
    set_log_output_level(ALOG_INFO);
    DEFER(set_log_output_level(ALOG_DEBUG));
    ObjectCache<int, ShowOnDtor*> ocache(1000UL * 1000 * 10);
    cycle_cnt = 0;
    release_cnt = 0;
    // 10s, during the test, nothing will be free if not set recycle
    std::vector<std::thread> ths;
    for (int i = 0; i < 10; i++) {
        ths.emplace_back([&] {
            photon::vcpu_init();
            DEFER(photon::vcpu_fini());
            std::vector<photon::join_handle*> handles;
            std::vector<OCArg> args;
            for (int i = 0; i < 100; i++) {
                args.emplace_back(OCArg({&ocache, i + 1}));
            }
            for (auto& arg : args) {
                handles.emplace_back(photon::thread_enable_join(
                    photon::thread_create(&objcache_borrow, &arg)));
            }
            for (const auto& handle : handles) {
                photon::thread_join(handle);
            }
        });
    }
    for (auto& x : ths) {
        x.join();
    }

    EXPECT_EQ(cycle_cnt, release_cnt);
}

struct OCArg2 {
    ObjectCache<int, ShowOnDtor*>* oc;
    int id;
    std::atomic<int>* count;
};

void* objcache_borrow_once(void* arg) {
    auto args = (OCArg2*)arg;
    auto oc = args->oc;
    // auto id = args->id;
    auto& count = *args->count;
    auto ctor = [&]() {
        // failed after 1s;
        photon::thread_usleep(1000 * 1000UL);
        count++;
        return nullptr;
    };
    auto ret = oc->borrow(0, ctor, 1000UL * 1000);
    // every 10 objs will recycle
    return 0;
}

TEST(ObjectCache, borrow_with_once) {
    set_log_output_level(ALOG_INFO);
    DEFER(set_log_output_level(ALOG_DEBUG));
    ObjectCache<int, ShowOnDtor*> ocache(1000UL * 1000 * 10);
    cycle_cnt = 0;
    release_cnt = 0;
    std::atomic<int> count(0);
    // 10s, during the test, nothing will be free if not set recycle
    std::vector<photon::join_handle*> handles;
    std::vector<OCArg2> args;
    for (int i = 0; i < 100; i++) {
        args.emplace_back(OCArg2{&ocache, i + 1, &count});
    }
    for (auto& arg : args) {
        handles.emplace_back(photon::thread_enable_join(
            photon::thread_create(&objcache_borrow_once, &arg)));
    }
    for (const auto& handle : handles) {
        photon::thread_join(handle);
    }
    EXPECT_EQ(1, count.load());
    ocache.borrow(0, [&] {
        photon::thread_usleep(1000 * 1000UL);
        count++;
        return new ShowOnDtor(1);
    });
    EXPECT_EQ(2, count.load());
}

struct OCArgV2 {
    ObjectCacheV2<int, ShowOnDtor*>* oc;
    int id;
    std::atomic<int>* count;
};
void* objcache_borrow_v2(void* arg) {
    auto args = (OCArgV2*)arg;
    auto oc = args->oc;
    auto id = args->id;
    auto ctor = [&]() { return new ShowOnDtor(id); };
    // acquire every 10ms
    photon::thread_usleep(10 * 1000UL * id);
    {
        auto ret = oc->borrow(0, ctor);
        LOG_DEBUG("Acquired ", VALUE(id));
        EXPECT_TRUE(ret);
        // object holds for 50ms
        photon::thread_usleep(50 * 1000UL);
        if (id % 10 == 0) {
            LOG_INFO("Cycle ", VALUE(id));
            ret.recycle(true);
        }
    }
    // every 10 objs will recycle
    if (id % 10 == 0) cycle_cnt++;
    LOG_DEBUG("Released ", VALUE(id));
    return 0;
}


TEST(ObjectCacheV2, borrow) {
    set_log_output_level(ALOG_INFO);
    DEFER(set_log_output_level(ALOG_DEBUG));
    ObjectCacheV2<int, ShowOnDtor*> ocache(1000UL * 1000 * 10);
    cycle_cnt = 0;
    release_cnt = 0;
    // 10s, during the test, nothing will be free if not set recycle
    std::vector<std::thread> ths;
    for (int i = 0; i < 10; i++) {
        ths.emplace_back([&] {
            photon::vcpu_init();
            DEFER(photon::vcpu_fini());
            std::vector<photon::join_handle*> handles;
            std::vector<OCArgV2> args;
            for (int i = 0; i < 100; i++) {
                args.emplace_back(OCArgV2({&ocache, i + 1}));
            }
            for (auto& arg : args) {
                handles.emplace_back(photon::thread_enable_join(
                    photon::thread_create(&objcache_borrow_v2, &arg)));
            }
            for (const auto& handle : handles) {
                photon::thread_join(handle);
            }
        });
    }
    for (auto& x : ths) {
        x.join();
    }

    EXPECT_EQ(cycle_cnt, release_cnt);
}

void* objcache_borrow_once_v2(void* arg) {
    auto args = (OCArgV2*)arg;
    auto oc = args->oc;
    // auto id = args->id;
    auto& count = *args->count;
    auto ctor = [&]()->ShowOnDtor* {
        // failed after 1s;
        photon::thread_usleep(1000 * 1000UL);
        count++;
        return nullptr;
    };
    auto ret = oc->borrow(0, ctor, 1000UL * 1000);
    (void)ret;
    // every 10 objs will recycle
    return 0;
}

TEST(ObjectCacheV2, borrow_with_once) {
    set_log_output_level(ALOG_INFO);
    DEFER(set_log_output_level(ALOG_DEBUG));
    ObjectCacheV2<int, ShowOnDtor*> ocache(1000UL * 1000 * 10);
    cycle_cnt = 0;
    release_cnt = 0;
    std::atomic<int> count(0);
    // 10s, during the test, nothing will be free if not set recycle
    std::vector<photon::join_handle*> handles;
    std::vector<OCArgV2> args;
    for (int i = 0; i < 100; i++) {
        args.emplace_back(OCArgV2{&ocache, i + 1, &count});
    }
    for (auto& arg : args) {
        handles.emplace_back(photon::thread_enable_join(
            photon::thread_create(&objcache_borrow_once_v2, &arg)));
    }
    for (const auto& handle : handles) {
        photon::thread_join(handle);
    }
    EXPECT_EQ(1, count.load());
    ocache.borrow(0, [&] {
        photon::thread_usleep(1000 * 1000UL);
        count++;
        return new ShowOnDtor(1);
    });
    EXPECT_EQ(2, count.load());
}

TEST(ExpireContainer, expire_container) {
    char key[10] = "hello";
    char key2[10] = "hello";
    ExpireContainer<std::string, int, bool> expire(1000 *
                                                   1000);  // expire in 100ms
    expire.insert(key2, -1, false);
    memset(key2, 0, sizeof(key2));
    auto it = expire.find(key);
    EXPECT_NE(expire.end(), it);
    auto ref = *it;
    EXPECT_EQ(0, strcmp(ref->key().data(), key));
    EXPECT_EQ(0, strcmp(ref->get_payload<0>().data(), key));
    EXPECT_EQ(-1, ref->get_payload<1>());
    EXPECT_FALSE(ref->get_payload<2>());
    ref->get_payload<2>() = true;
    EXPECT_TRUE(ref->get_payload<2>());
    expire.expire();
    it = expire.find(key);
    EXPECT_NE(expire.end(), it);
    photon::thread_usleep(1000 * 1000);  // time pass
    expire.expire();                     // manually expire
    it = expire.find(key);
    EXPECT_EQ(expire.end(), it);
}

TEST(ExpireContainer, refresh) {
    char key[] = "hello";
    char key2[] = "wtf";
    ExpireContainer<std::string, int, bool> expire(2000 * 1000);
    auto it = expire.insert(key, 0, true);
    expire.insert(key2, 1, true);
    photon::thread_usleep(1100 * 1000);
    expire.expire();
    expire.refresh(*it);
    photon::thread_usleep(1100 * 1000);
    expire.expire();
    EXPECT_NE(expire.end(), expire.find(key));
    EXPECT_EQ(expire.end(), expire.find(key2));
}

TEST(ExpireList, expire_container) {
    char key[10] = "hello";
    ExpireList<std::string> expire(1000 * 1000);  // expire in 100ms
    expire.keep_alive(key, true);
    auto it = expire.find(key);
    EXPECT_NE(expire.end(), it);
    auto ref = *it;
    EXPECT_EQ(0, strcmp(ref->key().data(), key));
    expire.expire();
    it = expire.find(key);
    EXPECT_NE(expire.end(), it);

    photon::thread_usleep(500 * 1000);  // time pass
    expire.keep_alive(key, false);
    photon::thread_usleep(500 * 1000);
    expire.expire();  // manually expire
    it = expire.find(key);
    EXPECT_NE(expire.end(), it);
    photon::thread_usleep(501 * 1000);
    expire.expire();  // manually expire
    it = expire.find(key);
    EXPECT_EQ(expire.end(), it);
}

struct simple_node : intrusive_list_node<simple_node> {
    int id;

    simple_node(int x):id(x) {}
};

struct OCArgL {
    ObjectCache<int, intrusive_list<simple_node>>* oc;
    int id;
};

TEST(ObjCache, with_list) {
    set_log_output_level(ALOG_INFO);
    DEFER(set_log_output_level(ALOG_DEBUG));
    ObjectCache<int, intrusive_list<simple_node>> ocache(1000UL * 1000 * 10);
    for (int i=0;i<10;i++) {
        auto &list = ocache.acquire(0, []()->intrusive_list<simple_node> {return {};});
        list.push_back(new simple_node(i));
    }
    for (int i=0;i<10;i++) {
        ocache.release(0);
    }
    {
        int cnt = 0;
        for (;;) {
            auto b = ocache.borrow(0);
            LOG_INFO(VALUE(b->pop_front()->id));
            cnt ++;
            if (b->empty()) break;
        }
        EXPECT_EQ(10, cnt);
    }
}

TEST(ObjectCache, no_destroy) {
    ObjectCache<int, int*> oc(1000UL * 1000);
    int* ptr = nullptr;
    auto th1 = photon::thread_enable_join(photon::thread_create11([&oc, &ptr] {
        auto a = oc.acquire(0, [] { return new int(1); });
        ptr = a;
        photon::thread_yield();
        auto x = oc.release(0, true, false);
        EXPECT_EQ(x, a);
    }));
    auto th2 = photon::thread_enable_join(photon::thread_create11([&oc, &ptr] {
        auto a = oc.acquire(0, [] { return new int(2); });
        EXPECT_EQ(a, ptr);
        photon::thread_yield();
        auto x = oc.release(0);
        EXPECT_EQ(nullptr, x);
    }));
    photon::thread_join(th1);
    photon::thread_join(th2);
    auto x = oc.acquire(0, [] { return new int(3);});
    EXPECT_EQ(3, *x);
    oc.release(0);
}
TEST(ObjectCache, movedout) {
    ObjectCache<int, int*> oc(1000UL * 1000);
    int* ptr = nullptr;
    auto th1 = photon::thread_enable_join(photon::thread_create11([&oc, &ptr] {
        auto a = oc.borrow(0, [] { return new int(1); });
        ptr = &*a;
        photon::thread_yield();
        a.recycle(true);
        a.moveout(true);
        EXPECT_TRUE(a.moved());
    }));
    auto th2 = photon::thread_enable_join(photon::thread_create11([&oc, &ptr] {
        auto a = oc.borrow(0, [] { return new int(2); });
        EXPECT_EQ(ptr, &*a);
        photon::thread_yield();
    }));
    photon::thread_join(th1);
    photon::thread_join(th2);
    auto x = oc.borrow(0, [] { return new int(3);});
    EXPECT_EQ(3, *x);
}

int main(int argc, char** argv) {
    if (!photon::is_using_default_engine()) return 0;
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    ::testing::InitGoogleTest(&argc, argv);
    int ret = RUN_ALL_TESTS();
    return ret;
}
