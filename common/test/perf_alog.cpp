#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <chrono>
#include <thread>
#include <vector>
#include <iostream>

inline uint64_t GetSteadyTimeNs() {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

void *task(void *arg) {
    auto id = (int64_t*)arg;
    for (int i = 0; i < 1000000; ++i) {
        LOG_INFO("my id `", *id + i);
        photon::thread_yield();
    }
    delete id;
    return nullptr;
}

void test_alog(int id) {
    std::vector<photon::join_handle *> jhs;
    std::cout << "Perf alog" << std::endl;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; i++) {
        uint64_t* arg = new uint64_t(id * 100000000UL + i * 1000000UL);
        jhs.emplace_back(
            photon::thread_enable_join(photon::thread_create(task, arg)));
    }
    for (auto &x : jhs) photon::thread_join(x);
    auto done = std::chrono::steady_clock::now();
    std::cout << id << " spent " <<
             std::chrono::duration_cast<std::chrono::milliseconds>(done - start)
                 .count() << " ms" << std::endl;
}

int main() {
    //log_output_file("./out.log");
    default_logger.log_output = new_async_log_output(new_log_output_file("./out.log"), 8);
    log_output_level = ALOG_INFO;
    photon::init(0, 0);
    DEFER(photon::fini());
    std::vector<std::thread> ths;
    for (int i = 0; i < 8; i++) {
        ths.emplace_back([&, id = i] {
            photon::init(0, 0);
            DEFER(photon::fini());
            test_alog(id);
        });
    }
    for (auto &x : ths) {
        x.join();
    }
    uint64_t cost = 0;
    for (int i = 0; i < 100; ++i) {
        auto t0 = GetSteadyTimeNs();
        LOG_INFO("test async log");
        auto t1 = GetSteadyTimeNs();
        cost += (t1 - t0);
        photon::thread_usleep(100*1000UL);
    }
    std::cout << "async log cost " << cost/100 << std::endl;
    return 0;
}
