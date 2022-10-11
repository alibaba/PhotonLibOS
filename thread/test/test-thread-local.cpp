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
#include <photon/thread/thread-local.h>
#include <photon/common/alog.h>

static int expected_times = 0;

// Finalizer is destructed after Value
struct Finalizer {
    ~Finalizer() {
        if (destruct_times != expected_times) {
            LOG_ERROR("destruct times (`) is not expected as (`)", destruct_times, expected_times);
            abort();
        }
        LOG_INFO("Success: empty value has destructed ` times", destruct_times);
    }
    int destruct_times = 0;
};

struct Value {
    Value() : empty(true) {}
    explicit Value(std::string s) : v(std::move(s)) {}
    ~Value() {
        static Finalizer f;
        if (empty)
            f.destruct_times++;
    }
    std::string v;
    bool empty = false;
};

static photon::thread_local_ptr<Value>& get_v1() {
    static photon::thread_local_ptr<Value> v1;
    return v1;
}

static photon::thread_local_ptr<Value, const char*> v2("value");

static photon::thread_local_ptr<int, int> v3(3);
static photon::thread_local_ptr<int> v4;

TEST(tls, tls_variable) {
    auto th1 = photon::thread_create11([] {
        auto& v1 = get_v1();
        v1->v = "value";
        expected_times++;
    });
    photon::thread_enable_join(th1);
    photon::thread_join((photon::join_handle*) th1);

    auto th2 = photon::thread_create11([] {
        auto& v1 = get_v1();
        ASSERT_TRUE(v1->v.empty());
        expected_times++;
    });
    photon::thread_enable_join(th2);
    photon::thread_join((photon::join_handle*) th2);

    auto& v1 = get_v1();
    ASSERT_TRUE(v1->v.empty());
    expected_times++;
}

TEST(tls, tls_variable_with_param) {
    auto th1 = photon::thread_create11([] {
        ASSERT_FALSE(v2->v.empty());
        v2->v = "";
    });
    photon::thread_enable_join(th1);
    photon::thread_join((photon::join_handle*) th1);

    ASSERT_FALSE(v2->v.empty());
}

TEST(tls, tls_variable_POD) {
    auto th1 = photon::thread_create11([] {
        ASSERT_EQ(3, *v3);
        *v3 = 4;
        ASSERT_EQ(4, *v3);
        ASSERT_EQ(0, *v4);
    });
    photon::thread_enable_join(th1);
    photon::thread_join((photon::join_handle*) th1);

    ASSERT_EQ(3, *v3);
}

int main(int argc, char** arg) {
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}