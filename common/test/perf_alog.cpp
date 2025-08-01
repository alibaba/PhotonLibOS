#include <photon/common/alog.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <chrono>
#include <thread>
#include <vector>

inline static uint64_t GetSteadyTimeNs() {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

void *task(void *arg) {
    auto id = *(uint64_t*)arg;
    for (int i = 0; i < 1000000; ++i) {
        LOG_AUDIT("my id `", id + i);
        photon::thread_yield();
    }
    return nullptr;
}

void test_alog(int id) {
    std::vector<photon::join_handle *> jhs;
    LOG_INFO("Perf alog ", id);
    auto start = GetSteadyTimeNs();
    for (int i = 0; i < 4; i++) {
        uint64_t arg = id * 100000000UL + i * 1000000UL;
        auto th = photon::thread_create(task, &arg);
        photon::thread_yield_to(th);
        jhs.emplace_back(photon::thread_enable_join(th));
    }
    for (auto &x : jhs) photon::thread_join(x);
    auto done = GetSteadyTimeNs();
    LOG_INFO("perf async log ` spent ` ms", id, (done - start) / 1000000);
}

int main() {
    photon::init(0, 0);
    DEFER(photon::fini());
    default_audit_logger.log_output = new_async_log_output(new_log_output_file("./out.log"), 8);
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
        LOG_AUDIT("test async log ", i);
        auto t1 = GetSteadyTimeNs();
        cost += (t1 - t0);
        photon::thread_usleep(100*1000UL);
    }
    LOG_INFO("single async log spent ` ns", cost/100);
    return 0;
}
