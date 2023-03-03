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

#include <gtest/gtest.h>

#include <photon/thread/thread11.h>
#include <photon/thread/thread-key.h>
#include <photon/common/alog.h>

struct Value {
    int v = 0;
    ~Value() {
        if (v == 0) {
            LOG_ERROR("The value is not changed. key dtor didn't work");
            abort();
        }
    }
    static void key_dtor(void* v) {
        auto p = (Value*) v;
        p->v = 1;
    }
};

static Value g_value;

TEST(key, basic) {
    photon::thread_key_t key0, key1, key2, key3, key4;
    ASSERT_EQ(0, photon::thread_key_create(&key0, nullptr));
    ASSERT_EQ(0, photon::thread_key_create(&key1, nullptr));
    ASSERT_EQ(0, photon::thread_key_create(&key2, nullptr));
    ASSERT_EQ(0UL, key0);
    ASSERT_EQ(1UL, key1);
    ASSERT_EQ(2UL, key2);
    ASSERT_EQ(0, photon::thread_key_delete(key0));
    ASSERT_EQ(0, photon::thread_key_create(&key3, nullptr));
    ASSERT_EQ(0UL, key3);
    ASSERT_EQ(0, photon::thread_key_delete(key2));
    ASSERT_EQ(0, photon::thread_key_create(&key4, nullptr));
    ASSERT_EQ(2UL, key4);
    ASSERT_EQ(0, photon::thread_key_delete(key1));
    ASSERT_EQ(0, photon::thread_key_delete(key3));
    ASSERT_EQ(0, photon::thread_key_delete(key4));
    ASSERT_EQ(EINVAL, photon::thread_key_delete(key4));
    ASSERT_EQ(EINVAL, photon::thread_key_delete(10000));

    ASSERT_EQ(nullptr, photon::thread_getspecific(0));
    ASSERT_EQ(nullptr, photon::thread_getspecific(1));
    ASSERT_EQ(nullptr, photon::thread_getspecific(2));
    ASSERT_EQ(nullptr, photon::thread_getspecific(10000));
    ASSERT_EQ(EINVAL, photon::thread_setspecific(1, &g_value));
    ASSERT_EQ(EINVAL, photon::thread_setspecific(2, &g_value));
    ASSERT_EQ(EINVAL, photon::thread_setspecific(10000, &g_value));

    photon::thread_key_t key;
    ASSERT_EQ(0, photon::thread_key_create(&key, nullptr));
    ASSERT_EQ(0UL, key);
    ASSERT_EQ(0, photon::thread_setspecific(key, &g_value));
    ASSERT_EQ(&g_value, photon::thread_getspecific(key));
    ASSERT_EQ(0, photon::thread_key_delete(key));
    ASSERT_EQ(nullptr, photon::thread_getspecific(key));
    ASSERT_EQ(EINVAL, photon::thread_setspecific(key, &g_value));
}

TEST(key, dtor) {
    g_value.v = 0;
    auto th = photon::thread_create11([&] {
        photon::thread_key_t key;
        ASSERT_EQ(0, photon::thread_key_create(&key, &Value::key_dtor));
        ASSERT_EQ(0UL, key);
        ASSERT_EQ(0, photon::thread_setspecific(key, &g_value));
    });
    photon::thread_enable_join(th);
    photon::thread_join((photon::join_handle*) th);
    ASSERT_EQ(1, g_value.v);
    ASSERT_EQ(0, photon::thread_key_delete(0));
}

TEST(key, overflow) {
    photon::thread_key_t key;
    for (uint64_t i = 0; i < photon::THREAD_KEYS_MAX; ++i) {
        ASSERT_EQ(0, photon::thread_key_create(&key, &Value::key_dtor));
        ASSERT_EQ(i, key);
        ASSERT_EQ(0, photon::thread_setspecific(key, &g_value));
    }
    ASSERT_EQ(EAGAIN, photon::thread_key_create(&key, &Value::key_dtor));
    for (uint64_t i = 0; i < photon::THREAD_KEYS_MAX; ++i) {
        ASSERT_EQ(0, photon::thread_key_delete(i));
    }
    ASSERT_EQ(0, photon::thread_key_create(&key, &Value::key_dtor));
    ASSERT_EQ(0UL, key);
    ASSERT_EQ(0, photon::thread_key_delete(0));
}

int main(int argc, char** arg) {
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());

    ::testing::InitGoogleTest(&argc, arg);
    int ret = RUN_ALL_TESTS();

    // Finally test dtor in main thread
    static Value v;
    photon::thread_key_t key;
    int ret_key = photon::thread_key_create(&key, &Value::key_dtor);
    assert(ret_key == 0);
    ret_key = photon::thread_setspecific(key, &v);
    assert(ret_key == 0);

    return ret;
}