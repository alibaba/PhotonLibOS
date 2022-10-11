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
#include "../identity-pool.cpp"
#include <photon/thread/thread-pool.h>
#include "../io-alloc.h"
#undef protected

#include "../alog.h"
#include <gtest/gtest.h>
#include <photon/thread/thread.h>
#include <vector>

TEST(IdentityPoolGC, basic) {
    auto pool = new_identity_pool<int>(128);
    DEFER(delete_identity_pool(pool));
    pool->enable_autoscale();
    std::vector<int*> spc;
    for (int i=0;i<100;i++) {
        auto ptr = pool->get();
        spc.emplace_back(ptr);
    }
    for (auto x : spc) {
        pool->put(x);
    }
    EXPECT_EQ(100U, pool->m_size);
    int cnt = 0;
    while (pool->m_size > 0 && cnt++ < 12) {
        photon::thread_usleep(1000*1000);
        LOG_DEBUG(VALUE(pool->m_size));
    }
    EXPECT_EQ(0U, pool->m_size);
}

void* nothing(void*) {
    photon::thread_usleep(1000*1000);
    return nullptr;
}

TEST(ThreadPoolGC, single_pool) {
    auto pool = photon::new_thread_pool(128);
    pool->enable_autoscale();
    DEFER(photon::delete_thread_pool(pool));
    for (int i=0;i<100;i++) {
        pool->thread_create(nothing, nullptr);
    }
    while (pool->m_size < 100)
        photon::thread_usleep(1*1000);
    while (pool->m_size > 0) {
        photon::thread_usleep(1000*1000);
        LOG_DEBUG(VALUE(pool->m_size));
    }
    EXPECT_EQ(0U, pool->m_size);
}

void* pool_in_pool(void*) {
    auto pool = photon::new_thread_pool(16);
    pool->enable_autoscale();
    DEFER(photon::delete_thread_pool(pool));
    for (int i=0;i<10;i++) {
        pool->thread_create(nothing, nullptr);
    }
    while (pool->m_size < 10)
        photon::thread_usleep(1*1000);
    while (pool->m_size > 0) {
        photon::thread_usleep(1000*1000);
        LOG_DEBUG(VALUE(pool->m_size));
    }
    EXPECT_EQ(0U, pool->m_size);
    return nullptr;
}

TEST(ThreadPoolGC, pool_in_pool) {
    auto pool = photon::new_thread_pool(16);
    pool->enable_autoscale();
    DEFER(photon::delete_thread_pool(pool));
    for (int i=0;i<10;i++) {
        pool->thread_create(pool_in_pool, nullptr);
    }
    while (pool->m_size < 10)
        photon::thread_usleep(1*1000);
    while (pool->m_size > 0) {
        photon::thread_usleep(1000*1000);
        LOG_DEBUG(VALUE(pool->m_size));
    }
    EXPECT_EQ(0U, pool->m_size);
}

TEST(IOAlloc, basic) {
    PooledAllocator<1024*1024> mpool;
    mpool.enable_autoscale();
    auto alloc = mpool.get_io_alloc();
    std::vector<void*> mem;
    for (int i=0 ;i<32;i++) {
        mem.emplace_back(alloc.alloc(64 * 1024));
    }
    for (auto &x:mem) {
        alloc.dealloc(x);
    }
    while (mpool.slots[mpool.get_slot(64*1024, true)].m_size < 32) {
        photon::thread_usleep(1000);
    }
    while (mpool.slots[mpool.get_slot(64*1024, true)].m_size > 0) {
        photon::thread_usleep(1000 * 1000);
        LOG_DEBUG(VALUE(mpool.slots[mpool.get_slot(64*1024, true)].m_size));
    }
    EXPECT_EQ(0U, mpool.slots[mpool.get_slot(64*1024, true)].m_size);
}
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    int ret = RUN_ALL_TESTS();
    LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
