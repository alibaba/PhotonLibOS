#include <photon/common/checksum/crc32c.h>
#include <photon/common/checksum/crc64ecma.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include "../../../test/ci-tools.h"
#include "../../../test/gtest.h"

#ifndef DATA_DIR
#define DATA_DIR ""
#endif

#define xstr(arg) str(arg)
#define str(s) #s

struct test_case {
    std::string s;
    uint64_t crc64ecma;
    uint32_t crc32c;
};

std::vector<test_case> cases;

void setup() {
    chdir(xstr(DATA_DIR));
    std::ifstream in("checksum.in"),
                 in2("checksum.crc64");
    ASSERT_TRUE(in && in2);
    uint32_t bytes = 0;
    while (true) {
        uint32_t crc32;
        uint64_t crc64;
        std::string str;
        in >> crc32 >> str;
        in2 >> crc64;
        if (str.empty()) break;
        bytes += str.size();
        cases.push_back({std::move(str), crc64, crc32});
    }
    printf("Loaded %d cases, %d bytes\n", (int)cases.size(), (int)bytes);
}

typedef uint32_t (*CRC32C)(const uint8_t*, size_t, uint32_t);
void do_test32(const char* name, CRC32C crc32c) {
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 100; ++i)
    for (auto& c: cases) {
        auto crc32 = crc32c((uint8_t*)c.s.data(), c.s.length(), 0);
        EXPECT_EQ(crc32, c.crc32c);
    }
    int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - start).count();
    printf("%s time spent: %dns \n", name, time_cost);
}

TEST(TestChecksum, crc32c_hw) {
    if (!is_crc32c_hw_available()) {
        std::cout << "skip crc32c_hw test on unsupported paltform." << std::endl;
        return;
    }
    do_test32("crc32c_hw", crc32c_hw);
}

TEST(TestChecksum, crc32c_sw) {
    do_test32("crc32c_sw", crc32c_sw);
}

typedef uint64_t (*CRC64ECMA)(const uint8_t*, size_t, uint64_t);
void do_test64(const char* name, CRC64ECMA crc64ecma) {
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 100; ++i)
    for (auto& c: cases) {
        auto crc64 = crc64ecma((uint8_t*)c.s.data(), c.s.length(), 0);
        EXPECT_EQ(crc64, c.crc64ecma);
    }
    int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - start).count();
    printf("%s time spent: %dns \n", name, time_cost);
}

TEST(TestChecksum, crc64ecma_hw) {
    if (!is_crc64ecma_hw_available()) {
        std::cout << "skip crc64ecma_hw test on unsupported paltform." << std::endl;
        return;
    }
#ifdef __aarch64__
    do_test64("crc64ecma_hw (using pmull)", crc64ecma_hw);
#else
    do_test64("crc64ecma_hw", crc64ecma_hw);
#endif
}

TEST(TestChecksum, crc64ecma_sw) {
    do_test64("crc64ecma_sw", crc64ecma_sw);
}

int main(int argc, char **argv)
{
    if (!photon::is_using_default_engine()) return 0;
    ::testing::InitGoogleTest(&argc, argv);
    setup();
    return RUN_ALL_TESTS();
}