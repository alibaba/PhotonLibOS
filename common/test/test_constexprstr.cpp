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

#include <array>
#include <tuple>

#include "../alog-stdstring.h"
#include "../alog.h"
#include "../conststr.h"

DEFINE_ENUM_STR(VERBS, verbs, UNKNOW, DELETE, GET, HEAD, POST, PUT, CONNECT,
                OPTIONS, TRACE, COPY, LOCK, MKCOL, MOV, PROPFIND, PROPPATCH,
                SEARCH, UNLOCK, BIND, REBIND, UNBIND, ACL, REPORT, MKACTIVITY,
                CHECKOUT, MERGE, MSEARCH, NOTIFY, SUBSCRIBE, UNSUBSCRIBE, PATCH,
                PURGE, MKCALENDAR, LINK, UNLINK);

__attribute__((noinline)) void print_sample() {
    for (auto x = VERBS::UNKNOW; x <= VERBS::UNLINK; x = VERBS((int)x + 1)) {
        puts(verbs[x].data());
    }
}
TEST(Basic_simple_tests, HTTPVerb) {
    print_sample();
    EXPECT_TRUE(verbs[VERBS::UNKNOW] == "UNKNOW");
    EXPECT_TRUE(verbs[VERBS::DELETE] == "DELETE");
    EXPECT_TRUE(verbs[VERBS::GET] == "GET");
    EXPECT_TRUE(verbs[VERBS::HEAD] == "HEAD");
    EXPECT_TRUE(verbs[VERBS::PUT] == "PUT");
    EXPECT_TRUE(verbs[VERBS::CONNECT] == "CONNECT");
    EXPECT_TRUE(verbs[VERBS::OPTIONS] == "OPTIONS");
    EXPECT_TRUE(verbs[VERBS::TRACE] == "TRACE");
    EXPECT_TRUE(verbs[VERBS::COPY] == "COPY");
    EXPECT_TRUE(verbs[VERBS::LOCK] == "LOCK");
    EXPECT_TRUE(verbs[VERBS::MKCOL] == "MKCOL");
    EXPECT_TRUE(verbs[VERBS::MOV] == "MOV");
    EXPECT_TRUE(verbs[VERBS::PROPFIND] == "PROPFIND");
    EXPECT_TRUE(verbs[VERBS::PROPPATCH] == "PROPPATCH");
    EXPECT_TRUE(verbs[VERBS::SEARCH] == "SEARCH");
    EXPECT_TRUE(verbs[VERBS::UNLOCK] == "UNLOCK");
    EXPECT_TRUE(verbs[VERBS::BIND] == "BIND");
    EXPECT_TRUE(verbs[VERBS::REBIND] == "REBIND");
    EXPECT_TRUE(verbs[VERBS::UNBIND] == "UNBIND");
    EXPECT_TRUE(verbs[VERBS::ACL] == "ACL");
    EXPECT_TRUE(verbs[VERBS::REPORT] == "REPORT");
    EXPECT_TRUE(verbs[VERBS::MKACTIVITY] == "MKACTIVITY");
    EXPECT_TRUE(verbs[VERBS::CHECKOUT] == "CHECKOUT");
    EXPECT_TRUE(verbs[VERBS::MERGE] == "MERGE");
    EXPECT_TRUE(verbs[VERBS::MSEARCH] == "MSEARCH");
    EXPECT_TRUE(verbs[VERBS::NOTIFY] == "NOTIFY");
    EXPECT_TRUE(verbs[VERBS::SUBSCRIBE] == "SUBSCRIBE");
    EXPECT_TRUE(verbs[VERBS::UNSUBSCRIBE] == "UNSUBSCRIBE");
    EXPECT_TRUE(verbs[VERBS::PATCH] == "PATCH");
    EXPECT_TRUE(verbs[VERBS::PURGE] == "PURGE");
    EXPECT_TRUE(verbs[VERBS::MKCALENDAR] == "MKCALENDAR");
    EXPECT_TRUE(verbs[VERBS::LINK] == "LINK");
    EXPECT_TRUE(verbs[VERBS::UNLINK] == "UNLINK");
}

TEST(Basic_simple_tests, whole) {
    DEFINE_ENUM_STR(A, a, AAA, bbb, CCC, ddd);
    puts(a.whole().chars);
    puts(&a.whole().chars[4]);
    puts(&a.whole().chars[8]);
    puts(&a.whole().chars[12]);
    for (int i = 0; i < 4; i++) {
        printf("%d\n", a.arr().offset.arr[i]);
    }
}

auto out_of_func = TSTRING("Hello");

TEST(Static, memuse) {
    auto a = TSTRING("Hello");
    auto b = TSTRING("Hello");
    EXPECT_EQ(&a.chars, &b.chars);
    EXPECT_EQ(&out_of_func.chars, &a.chars);
}

TEST(TString, JoinAndSplit) {
    auto a = TSTRING("Hello");
    auto b = TSTRING(" world");
    auto c = ConstString::make_tstring_array(a, b);
    EXPECT_STREQ("Hello, world", c.join<','>().chars);

    auto d = TSTRING(
        "1,2, 3, "
        "4, 5, 6");
    // seperate by ',' and ignore ' '
    auto sp = d.split<',', ' '>();
    EXPECT_EQ(6UL, sp.size);
    EXPECT_TRUE("1" == sp.views[0]);
    EXPECT_TRUE("2" == sp.views[1]);
    EXPECT_TRUE("3" == sp.views[2]);
    EXPECT_TRUE("4" == sp.views[3]);
    EXPECT_TRUE("5" == sp.views[4]);
    EXPECT_TRUE("6" == sp.views[5]);
    EXPECT_STREQ("1|2|3|4|5|6", sp.join<'|'>().chars);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    int ret = RUN_ALL_TESTS();
    LOG_ERROR_RETURN(0, ret, VALUE(ret));
}
