#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

#include "../expirecontainer.h"
#include "../objectcachev2.h"

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

template <template <class, class> class OC>
void *task(void *arg) {
    auto oc = (OC<std::string, std::string *> *)arg;
    std::array<uint64_t, count> k = keys;
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(k.begin(), k.end(), g);

    auto start = std::chrono::steady_clock::now();
    for (const auto &x : k) {
        auto strx = std::to_string(x);
        auto b = oc->borrow(strx, [&strx] {
            photon::thread_usleep(1 * 1000);
            // LOG_INFO("CTOR `", photon::now);
            return new std::string(strx);
        });
    }
    auto done = std::chrono::steady_clock::now();
    LOG_INFO("spent ` ms",
             std::chrono::duration_cast<std::chrono::milliseconds>(done - start)
                 .count());
    return nullptr;
}

template <template <class, class> class OC>
void test_objcache(OC<std::string, std::string *> &oc, const char *name) {
    std::vector<photon::join_handle *> jhs;
    LOG_INFO("Query to ", name);
    for (int i = 0; i < 4; i++)
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create(task<OC>, &oc)));
    for (auto &x : jhs) photon::thread_join(x);
}

template <template <class, class> class OC>
void test(const char *name) {
    OC<std::string, std::string *> oc(0);
    std::vector<std::thread> ths;
    photon::semaphore sem(0);
    for (int i = 0; i < 10; i++) {
        ths.emplace_back([&] {
            photon::vcpu_init();
            DEFER(photon::vcpu_fini());
            test_objcache<OC>(oc, name);
            sem.signal(1);
        });
    }
    sem.wait(10);
    for (auto &x : ths) {
        x.join();
    }
}

int main() {
    photon::vcpu_init();
    DEFER(photon::vcpu_fini());
    ready();
    test<ObjectCache>("ObjectCache");
    test<ObjectCacheV2>("ObjectCacheV2");
    return 0;
}