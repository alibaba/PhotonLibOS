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

#include <thread>
#include <gtest/gtest.h>
#include <photon/common/utility.h>

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
    static thread_local Value v1(1);
    return v1;
}

static Value& get_v4() {
    static thread_local Value v4(4);
    return v4;
}

struct GlobalEnv {
    GlobalEnv() {
        printf("Construct GlobalEnv\n");
        get_v1().m_val = -1;
    }

    ~GlobalEnv() {
        printf("Destruct GlobalEnv\n");
        assert(get_v1().m_val == -1);
        assert(get_v4().m_val == 4);
    }
    static thread_local Value v3;
};

// GlobalEnv is placed in the middle of v0 and v2.
// v1 first appears in its constructor. v4 first appears in its destructor.
// The initialization order is: v0 -> v1 -> v2 -> v3 -> v4
static thread_local Value v0(0);
static GlobalEnv env;
static thread_local Value v2(2);
thread_local Value GlobalEnv::v3(3);

TEST(global_init, basic) {
    auto th = std::thread([] {
        ASSERT_EQ(0, v0.m_val);         v0.m_val = 0;
        ASSERT_EQ(1, get_v1().m_val);   get_v1().m_val = 0;
        ASSERT_EQ(2, v2.m_val);         v2.m_val = 0;
        ASSERT_EQ(3, env.v3.m_val);     env.v3.m_val = 0;
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
Construct 2
Construct 3
Construct 1
Destruct 0
Destruct 0
Destruct 0
Destruct 0
End main
Destruct -1
Destruct GlobalEnv
Construct 4
*/

/* Analysis:
 * 1. The construction order of the thread_local global variable is slightly different from the one of Photon.
 * 2. If a thread_local value is first accessed before the main function (global variable construction),
 *    it's lifecycle will remain to the end of the program (global variable destruction).
 * 3. No Destruct 4 in the final destruction of GlobalEnv, and no mem-leak occurs.
 */