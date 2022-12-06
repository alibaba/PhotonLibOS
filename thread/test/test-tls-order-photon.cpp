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
#include <photon/thread/std-compat.h>
#include <photon/common/alog.h>

struct Value {
    explicit Value(int val) : m_val(val) {
        printf("Construct %d\n", m_val);
    }
    ~Value() {
        printf("Destruct %d\n", m_val);
    }
    int m_val;
};

static Value& get_v1() {
    static photon::thread_local_ptr<Value, int> v1(1);
    return *v1;
}

static Value& get_v4() {
    static photon::thread_local_ptr<Value, int> v4(4);
    return *v4;
}

#define ASSERT(x) if (!(x)) abort();

struct GlobalEnv {
    GlobalEnv() {
        printf("Construct GlobalEnv\n");
        // WARING: No photon tls can be accessed BEFORE photon_init
        ASSERT(photon::init() == 0);
        ASSERT(photon::std::work_pool_init(4) == 0);
        get_v1().m_val = -1;
    }

    ~GlobalEnv() {
        printf("Destruct GlobalEnv\n");
        ASSERT(get_v1().m_val == -1);
        ASSERT(get_v4().m_val == 4);
        photon::std::work_pool_fini();
        photon::fini();
        // WARING: No photon tls can be accessed AFTER photon_fini
    }
    static photon::thread_local_ptr<Value, int> v3;
};

// GlobalEnv is placed in the middle of v0 and v2.
// v1 first appears in its constructor. v4 first appears in its destructor.
// The initialization order is: v0 -> v1 -> v2 -> v3 -> v4
static photon::thread_local_ptr<Value, int> v0(0);
static GlobalEnv env;
static photon::thread_local_ptr<Value, int> v2(2);
photon::thread_local_ptr<Value, int> GlobalEnv::v3(3);

TEST(global_init, basic) {
    auto th = photon::std::thread([] {
        ASSERT_EQ(0, v0->m_val);        v0->m_val = 0;
        ASSERT_EQ(1, get_v1().m_val);   get_v1().m_val = 0;
        ASSERT_EQ(2, v2->m_val);        v2->m_val = 0;
        ASSERT_EQ(3, env.v3->m_val);    env.v3->m_val = 0;
    });
    th.join();

    ASSERT_EQ(-1, get_v1().m_val);
}

int main(int argc, char** arg) {
    printf("Begin main\n");
    DEFER(printf("End main\n"));
    ::testing::InitGoogleTest(&argc, arg);
    return RUN_ALL_TESTS();
}

/* Output:
Construct GlobalEnv
Construct 1
Begin main
Construct 0
Construct 1
Construct 2
Construct 3
Destruct 0
Destruct 0
Destruct 0
Destruct 0
End main
Destruct GlobalEnv
Construct 4
Destruct -1
Destruct 4
*/

/* Analysis:
 * See test-tls-order-native.cpp
 */