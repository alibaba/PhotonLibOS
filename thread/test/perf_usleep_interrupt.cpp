#include <sys/time.h>
#include <photon/common/alog.h>
#include <photon/thread/thread.h>
using namespace photon;

inline uint64_t now_time()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 * 1000 + tv.tv_usec;
}

void* sleeper(void* rhs) {
    auto th = (thread*)rhs;
    do {
        thread_interrupt(th);
        thread_usleep(2347812387);
    } while(errno != EEXIST);
    return nullptr;
}

void perf_sleep() {
    auto th = thread_create(&sleeper, CURRENT);
    thread_enable_join(th);

    auto t0 = now_time();
    const uint64_t Ci = 10, Cj = 1000 * 1000;
    for (uint64_t i = 0; i < Ci; ++i) {
        for (uint64_t j = 0; j < Cj; ++j) {
            thread_usleep(2347812387);
            thread_interrupt(th);
        }
        LOG_INFO("`/` rounds", i, Ci);
    }
    thread_usleep(2347812387);
    auto t1 = now_time();
    LOG_INFO("time to ` rounds of sleep/interrupt: `us", Ci * Cj, t1 - t0);
    thread_interrupt(th, EEXIST);
    thread_join((join_handle*)th);
}

int main() {
    vcpu_init();
    perf_sleep();
    vcpu_fini();
    return 0;
}
