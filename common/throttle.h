#pragma once

#include <photon/thread/thread.h>

namespace photon {
class throttle {
protected:
    photon::semaphore sem;
    uint64_t last_retrieve = 0;
    uint64_t m_limit = -1UL;
    uint64_t m_limit_per_slice;
    uint64_t m_time_window;
    uint64_t m_time_window_per_slice;
    uint64_t m_slice_num;

    void try_signal() {
        auto duration = photon::now - last_retrieve;
        if (duration > m_time_window) duration = m_time_window;
        if (duration >= m_time_window_per_slice) {
            auto free = m_limit_per_slice * (duration / m_time_window_per_slice);
            auto current = photon::sat_sub(m_limit, sem.count());
            if (current < free) {
                free = current;
            }
            sem.signal(free);
            last_retrieve = photon::now;
        }
    }

public:
    /**
     * @param limit -1UL means no limit, 0 means lowest speed (hang)
     */
    explicit throttle(uint64_t limit, uint64_t time_window = 1000UL * 1000,
             uint64_t slice = 10) : m_slice_num(slice) {
        update(limit);
        m_time_window_per_slice = photon::sat_add(time_window, (m_slice_num - 1)) / m_slice_num;
        m_time_window = m_time_window_per_slice * m_slice_num;
        sem.signal(m_limit);
    }

    void update(uint64_t limit) {
        m_limit_per_slice = photon::sat_add(limit, (m_slice_num - 1)) / m_slice_num;
        m_limit = m_limit_per_slice * m_slice_num;
    }

    int consume(uint64_t amount) {
        int ret = 0;
        int err = 0;
        do {
            try_signal();
            ret = sem.wait(amount, m_time_window_per_slice);
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