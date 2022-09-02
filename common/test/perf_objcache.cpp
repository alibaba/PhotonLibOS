#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <array>
#include <chrono>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "../expirecontainer.h"

constexpr size_t count = 10UL * 1024;

std::array<uint64_t, count> keys;

void ready() {
    LOG_INFO("Get ready");

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> nd;

    for (auto &x : keys) {
        x = nd(rng);
    }
    LOG_INFO("Random key generated");
}

void *task(void *arg) {
    auto oc = (ObjectCache<std::string, std::string *> *)arg;
    std::array<uint64_t, count> k = keys;
    std::random_shuffle(k.begin(), k.end());

    for (const auto &x : k) {
        auto strx = std::to_string(x);
        auto b = oc->borrow(strx, [&strx] {
            photon::thread_usleep(1 * 1000);
            // LOG_INFO("CTOR `", photon::now);
            return new std::string(strx);
        });
    }
    return nullptr;
}

void test_objcache(ObjectCache<std::string, std::string *> &oc) {
    std::vector<photon::join_handle *> jhs;
    LOG_INFO("Query to ObjectCache");
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; i++)
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create(task, &oc)));
    for (auto &x : jhs) photon::thread_join(x);
    auto done = std::chrono::steady_clock::now();
    LOG_INFO("spent ` ms",
             std::chrono::duration_cast<std::chrono::milliseconds>(done - start)
                 .count());
}

int main() {
    photon::init(0, 0);
    DEFER(photon::fini());
    ObjectCache<std::string, std::string *> oc(-1UL);
    ready();
    std::vector<std::thread> ths;
    for (int i = 0; i < 10; i++) {
        ths.emplace_back([&] {
            photon::init(0, 0);
            DEFER(photon::fini());
            test_objcache(oc);
        });
    }
    for (auto &x : ths) {
        x.join();
    }
    return 0;
}