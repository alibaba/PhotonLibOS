#pragma once
#include <random>
#include <algorithm>

// #pragma GCC diagnostic push
// #ifdef __clang__
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-result"

// #endif
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
// #pragma GCC diagnostic pop

template< class RandomIt> inline
void shuffle( RandomIt first, RandomIt last) {
    std::random_device rd;
    std::mt19937 g(rd());
    return std::shuffle(first, last, g);
}

