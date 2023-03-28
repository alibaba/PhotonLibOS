#pragma once

#include <photon/thread/thread.h>

namespace photon {
class throttle {
protected:
    photon::semaphore sem;
    uint64_t last_retrieve = 0;
    uint64_t m_limit = -1UL;
    uint64_t m_limit_slice;
    uint64_t m_time_window;
    uint64_t m_time_slice;
    uint64_t m_slice_num;

    void try_signal() {
        auto duration = photon::now - last_retrieve;
        if (duration > m_time_window) duration = m_time_window;
        if (duration >= m_time_slice) {
            auto free = m_limit_slice * (duration / m_time_slice);
            auto current = photon::sat_sub(m_limit, sem.count());
            if (current < free) {
                free = current;
            }
            sem.signal(free);
            last_retrieve = photon::now;
        }
    }

public:
    throttle(uint64_t limit, uint64_t time_window = 1000UL * 1000,
             uint64_t slice = 10) {
        m_slice_num = slice;
        m_limit_slice = photon::sat_add(limit, (slice - 1)) / slice;
        m_limit = m_limit_slice * slice;
        m_time_slice = photon::sat_add(time_window, (slice - 1)) / slice;
        m_time_window = m_time_slice * slice;
        sem.signal(m_limit);
    }

    int consume(uint64_t amount) {
        int ret = 0;
        int err = 0;
        do {
            try_signal();
            ret = sem.wait(amount, m_time_slice);
            err = errno;
        } while (ret < 0 && err == ETIMEDOUT);
        if (ret < 0) {
            errno = err;
            return ret;
        }
        return 0;
    }

    int try_consume(uint64_t amount) {
        try_signal();
        return sem.wait(amount, 0);
    }

    void restore(uint64_t amount) {
        auto free = amount;
        auto current = photon::sat_sub(m_limit, sem.count());
        if (current < free) free = current;
        sem.signal(free);
    }
};
}  // namespace photon