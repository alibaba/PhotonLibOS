#include <gtest/gtest.h>
#include <photon/common/checksum/crc32c.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef DATA_DIR
#define DATA_DIR ""
#endif

#define xstr(arg) str(arg)
#define str(s) #s

class TestChecksum : public ::testing::Test {
    virtual void SetUp() {
        in.open(xstr(DATA_DIR) "checksum.in");
        ASSERT_TRUE(!!in);
        uint32_t value;
        std::string str;

        while (getline(in, str)) {
            std::stringstream ss(str);
            ss.imbue(std::locale::classic());
            ss >> value >> str;
            cases.push_back(std::make_pair(value, str));
        }

        in.close();
    }

    virtual void TearDown() { cases.clear(); }

protected:
    std::ifstream in;
    std::vector<std::pair<uint32_t, std::string> > cases;
};

TEST_F(TestChecksum, crc32c_hw) {
    if (is_crc32c_hw_available()) {
        auto start = std::chrono::system_clock::now();
        for (size_t i = 0; i < cases.size(); ++i) {
            size_t len = cases[i].second.length();
            const char* data = cases[i].second.c_str();

            auto crc = crc32c_hw(reinterpret_cast<const uint8_t*>(data), len, 0);
            EXPECT_EQ(cases[i].first, crc);
        }
        int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - start).count();
        printf("crc32c_hw time spent: %dns \n", time_cost);
    } else {
        std::cout << "skip crc32c_hw test on unsupported paltform." << std::endl;
    }
}

TEST_F(TestChecksum, crc32c_sw) {
    auto start = std::chrono::system_clock::now();
    for (size_t i = 0; i < cases.size(); ++i) {
        size_t len = cases[i].second.length();
        const char* data = cases[i].second.c_str();

        auto crc = crc32c_sw(reinterpret_cast<const uint8_t*>(data), len, 0);
        EXPECT_EQ(cases[i].first, crc);
    }
    int time_cost = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - start).count();
    printf("crc32c_sw time spent: %dns \n", time_cost);
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}