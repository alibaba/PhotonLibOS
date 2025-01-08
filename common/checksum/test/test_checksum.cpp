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
        if (crc32 != c.crc32c) puts(c.s.data());
        EXPECT_EQ(crc32, c.crc32c);
    }
    int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - start).count();
    printf("%s time spent: %dus \n", name, time_cost);
}

uint32_t crc32c_hw_simple(const uint8_t *data, size_t nbytes, uint32_t crc);
uint32_t crc32c_hw_portable(const uint8_t *data, size_t nbytes, uint32_t crc);
extern "C" uint32_t crc32_iscsi_crc_ext(const uint8_t* buffer, size_t len, uint32_t crc_init);
#if !defined(__APPLE__)
extern "C" uint32_t crc32_iscsi_00(const uint8_t* buffer, size_t len, uint32_t crc_init);
#else
extern "C" uint32_t crc32_iscsi_00(const uint8_t* buffer, size_t len, uint32_t crc_init)
               asm("crc32_iscsi_00");
#endif
inline uint32_t crc32c_hw_asm(const uint8_t *data, size_t nbytes, uint32_t crc) {
#if defined(__x86_64__)
    return crc32_iscsi_00(data, nbytes, crc);
#elif defined(__aarch64__)
    return crc32_iscsi_crc_ext(data, nbytes, crc);
#endif
}

TEST(TestChecksum, crc32c_hw_simple) {
    if (!is_crc32c_hw_available()) {
        std::cout << "skip crc32c_hw test on unsupported paltform." << std::endl;
        return;
    }
    do_test32("crc32c_hw_simple", crc32c_hw_simple);
}

TEST(TestChecksum, crc32c_hw_portable) {
    if (!is_crc32c_hw_available()) {
        std::cout << "skip crc32c_hw test on unsupported paltform." << std::endl;
        return;
    }
    do_test32("crc32c_hw_portable", crc32c_hw_portable);
}

TEST(TestChecksum, crc32c_hw_asm) {
    if (!is_crc32c_hw_available()) {
        std::cout << "skip crc32c_hw test on unsupported paltform." << std::endl;
        return;
    }
    do_test32("crc32c_hw_asm", crc32c_hw_asm);
}

TEST(TestChecksum, crc32c_sw) {
    do_test32("crc32c_sw", crc32c_sw);
}

TEST(TestChecksum, crc32c_hw_small) {
    char d[4096];
    for (int i = 0; i < 4000; ++i) {
        d[i] = 0;
        uint32_t crc_sw = crc32c_sw((uint8_t*)d, strlen(d), 0);
        uint32_t crc_hw = crc32c_hw_portable((uint8_t*)d, strlen(d), 0);
        if (crc_sw != crc_hw) printf("i=%d\n", i);
        EXPECT_EQ(crc_sw, crc_hw);
        d[i] = 'a' + i % 26;
    }
}

void do_test32_big(const char* name, CRC32C crc32c) {
    static unsigned char buf[512 * 1024 * 1024];
    memset(buf, 0, sizeof(buf));
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 100; ++i) {
        crc32c(buf, sizeof(buf), 0);
    }
    int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - start).count();
    printf("%s time spent: %dus \n", name, time_cost);
}

TEST(TestChecksumBig, crc32c_hw_simple) {
    do_test32_big("crc32c_hw_simple", crc32c_hw_simple);
}

TEST(TestChecksumBig, crc32c_hw_portable) {
    do_test32_big("crc32c_hw_portable", crc32c_hw_portable);
}

TEST(TestChecksumBig, crc32c_hw_asm) {
#if defined(__x86_64__)
    do_test32_big("crc32c_hw_asm(crc32_iscsi_00)", crc32c_hw_asm);
#elif defined(__aarch64__)
    do_test32_big("crc32c_hw_asm(crc32_iscsi_crc_ext)", crc32c_hw_asm);
#endif
}
/*
TEST(TestChecksumBig, crc32c_sw) {
    do_test32_big("crc32c_sw", crc32c_sw);
}
*/
typedef uint64_t (*CRC64ECMA)(const uint8_t*, size_t, uint64_t);
void do_test64(const char* name, CRC64ECMA crc64ecma) {
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 100; ++i)
    for (auto& c: cases) {
        auto crc64 = crc64ecma((uint8_t*)c.s.data(), c.s.length(), 0);
        if (crc64 != c.crc64ecma) puts(c.s.data());
        EXPECT_EQ(crc64, c.crc64ecma);
    }
    int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - start).count();
    printf("%s time spent: %dus \n", name, time_cost);
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