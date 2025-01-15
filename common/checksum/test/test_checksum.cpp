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
    uint64_t crc32c;
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
typedef uint64_t (*CRC64ECMA)(const uint8_t*, size_t, uint64_t);

void do_test_crc(const char* name, CRC64ECMA calcurlator,
            size_t offset = offsetof(test_case, crc64ecma)) {
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 100; ++i)
    for (auto& c: cases) {
        auto crc = calcurlator((uint8_t*)c.s.data(), c.s.length(), 0);
        auto shouldbe = *(uint64_t*)((const char*)&c + offset);
        if (crc != shouldbe) puts(c.s.data());
        EXPECT_EQ(crc, shouldbe);
    }
    auto duration = std::chrono::system_clock::now() - start;
    int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    printf("%s time spent: %d us \n", name, time_cost);
}

#pragma GCC diagnostic ignored "-Wstrict-aliasing"
void do_test_crc(const char* name, CRC32C calcurlator) {
    return do_test_crc(name, (CRC64ECMA&)calcurlator, offsetof(test_case, crc32c));
}

void do_test_crc_small(CRC64ECMA calc_sw, CRC64ECMA calc_hw, uint16_t begin, uint16_t end) {
    alignas(16) unsigned char buf[64 * 1024 + 16];
    for (uint16_t i = begin; i < end; ++i) {
        buf[i] = 0;
        auto crc_sw = calc_sw(buf, i, 0);
        auto crc_hw = calc_hw(buf, i, 0);
        if (crc_sw != crc_hw) printf("i=%d\n", i);
        EXPECT_EQ(crc_sw, crc_hw);
        buf[i] = 'a' + i % 26;
    }
}

inline void do_test_crc_small(CRC32C calc_sw, CRC32C calc_hw, uint16_t begin, uint16_t end) {
    do_test_crc_small((CRC64ECMA&)calc_sw, (CRC64ECMA&)calc_hw, begin, end);
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

#if defined(__x86_64__)
#define crc32c_hw_asm crc32_iscsi_00
#define crc32c_hw_asm_name "crc32c_hw_asm(crc32_iscsi_00)"
#elif defined(__aarch64__)
#define crc32c_hw_asm crc32_iscsi_crc_ext
#define crc32c_hw_asm_name "crc32c_hw_asm(crc32_iscsi_crc_ext)"
#endif

TEST(TestChecksum, crc32c_hw_simple) {
    do_test_crc("crc32c_hw_simple", crc32c_hw_simple);
}

TEST(TestChecksum, crc32c_hw_portable) {
    do_test_crc("crc32c_hw_portable", crc32c_hw_portable);
}

TEST(TestChecksum, crc32c_hw_asm) {
    do_test_crc(crc32c_hw_asm_name, crc32c_hw_asm);
}

TEST(TestChecksum, crc32c_sw) {
    do_test_crc("crc32c_sw", crc32c_sw);
}

TEST(TestChecksum, crc32c_hw_small) {
    do_test_crc_small(crc32c_sw, crc32c_hw_portable, 0, 4000);
}

void do_test_crc_big(const char* name, CRC32C crc32c) {
    static unsigned char buf[512 * 1024 * 1024];
    memset(buf, 0, sizeof(buf));
    auto start = std::chrono::system_clock::now();
    const uint64_t MAX = 100;
    for (auto i = MAX; i; --i) {
        crc32c(buf, sizeof(buf), 0);
    }
    int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - start).count();
    printf("%s time spent: %d us (%0.2f GB/s)\n", name, time_cost,
        sizeof(buf) * MAX / 1024 / 1024 / 1024 / (double(time_cost) / 1000 / 1000));
}

inline void do_test_crc_big(const char* name, CRC64ECMA crc64ecma) {
    return do_test_crc_big(name, (CRC32C&)crc64ecma);
}

TEST(TestChecksumBig, crc32c_hw_simple) {
    do_test_crc_big("crc32c_hw_simple", crc32c_hw_simple);
}

TEST(TestChecksumBig, crc32c_hw_portable) {
    do_test_crc_big("crc32c_hw_portable", crc32c_hw_portable);
}

TEST(TestChecksumBig, crc32c_hw_asm) {
    do_test_crc_big(crc32c_hw_asm_name, crc32c_hw_asm);
}
/*
TEST(TestChecksumBig, crc32c_sw) {
    do_test_crc_big("crc32c_sw", crc32c_sw);
}
*/
void do_test64(const char* name, CRC64ECMA crc64ecma) {
    auto start = std::chrono::system_clock::now();
    for (int i = 0; i < 100; ++i)
    for (auto& c: cases) {
        auto crc64 = crc64ecma((uint8_t*)c.s.data(), c.s.length(), 0);
        if (crc64 != c.crc64ecma) puts(c.s.data());
        EXPECT_EQ(crc64, c.crc64ecma);
    }
    int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - start).count();
    printf("%s time spent: %d us \n", name, time_cost);
}

extern "C" uint64_t crc64_ecma_refl_pmull(uint64_t seed, const uint8_t *buf, uint64_t len);
#if !defined(__APPLE__) || !defined(__x86_64__)
extern "C" uint64_t crc64_ecma_refl_by8  (uint64_t seed, const uint8_t *buf, uint64_t len);
#else
extern "C" uint64_t crc64_ecma_refl_by8  (uint64_t seed, const uint8_t *buf, uint64_t len)
               asm("crc64_ecma_refl_by8");
#endif

inline uint64_t crc64ecma_hw_asm(const uint8_t *data, size_t nbytes, uint64_t crc) {
#if defined(__x86_64__)
#define crc64ecma_hw_asm_name "crc64ecma_hw_asm(crc64_ecma_refl_by8)"
    return crc64_ecma_refl_by8(crc, data, nbytes);
#elif defined(__aarch64__)
#define crc64ecma_hw_asm_name "crc64ecma_hw_asm(crc64_ecma_refl_pmull)"
    return crc64_ecma_refl_pmull(crc, data, nbytes);
#endif
}

TEST(TestChecksum, crc64ecma_hw) {
    do_test64("crc64ecma_hw_portable", crc64ecma_hw);
}

TEST(TestChecksumBig, crc64ecma_hw) {
    do_test_crc_big("crc64ecma_hw_portable", crc64ecma_hw);
}

TEST(TestChecksumBig, crc64ecma_hw_asm) {
    do_test_crc_big(crc64ecma_hw_asm_name, crc64ecma_hw_asm);
}

TEST(TestChecksum, crc64ecma_small) {
    do_test_crc_small(&crc64ecma_sw, &crc64ecma_hw, 0, 4000);
}

TEST(TestChecksum, crc64ecma_sw) {
    do_test64("crc64ecma_sw", crc64ecma_sw);
}

TEST(TestChecksum, crc64ecma_hw_asm) {
    do_test64(crc64ecma_hw_asm_name, crc64ecma_hw_asm);
}

int main(int argc, char **argv)
{
    if (!photon::is_using_default_engine()) return 0;
    ::testing::InitGoogleTest(&argc, argv);
    setup();
    return RUN_ALL_TESTS();
}