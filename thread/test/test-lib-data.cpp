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
#include <libgen.h>
#include <photon/common/utility.h>
#include <photon/photon.h>
#include <stdio.h>
#include <stdlib.h>

bool popen_test(const std::string& cmd, int expect = 0) {
    puts(cmd.c_str());
    auto p = popen(cmd.c_str(), "r");
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), p) != NULL)
        ;
    auto r = pclose(p);
    if (WIFEXITED(r)) return WEXITSTATUS(r) == expect;
    puts("Not exit");
    return false;
}

std::string popen_read(const std::string& cmd, int expect = 0) {
    std::string ret;
    puts(cmd.c_str());
    auto p = popen(cmd.c_str(), "r");
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), p) != NULL) {
        ret += buffer;
        puts(buffer);
    }
    pclose(p);
    return ret;
}

std::string libpath(uint64_t pid) {
    auto path =
        popen_read("cat /proc/" + std::to_string(pid) +
                   "/maps | grep libphoton | tr -s ' ' | cut -f 6 -d ' '"
                   " | head -n 1");
    EXPECT_FALSE(path.empty());
    char* tp = strdup(path.c_str());
    path = dirname(tp);
    puts(path.c_str());
    free(tp);
    return path;
}

TEST(static_lib, photon_thread_alloc) {
#ifdef __linux__
    auto pid = getpid();
    auto p = libpath(pid) + "/libphoton.a";
    EXPECT_TRUE(popen_test("objdump -tr \"" + p +
                           "\" | grep photon_thread_allocE | grep .data"));
    EXPECT_TRUE(popen_test("objdump -tr \"" + p +
                           "\" | grep photon_thread_deallocE | grep .data"));
#endif
}

TEST(shared_lib, photon_thread_alloc) {
#ifdef __linux__
    auto pid = getpid();
    auto p = libpath(pid) + "/libphoton.so";
    EXPECT_TRUE(popen_test("objdump -tr \"" + p +
                           "\" | grep photon_thread_allocE | grep .data"));
    EXPECT_TRUE(popen_test("objdump -tr \"" + p +
                           "\" | grep photon_thread_deallocE | grep .data"));
#endif
}

int main(int argc, char** argv) {
    photon::init(photon::INIT_EVENT_NONE, 0);
    DEFER(photon::fini());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}