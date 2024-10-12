#pragma once

#include <photon/thread/thread.h>
#include <photon/common/utility.h>

namespace photon {

class throttle {
protected:
    photon::semaphore sem;
    uint64_t last_retrieve = 0;
    uint64_t m_limit = -1UL;
    uint64_t m_limit_per_slice = -1UL;
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
             uint64_t slice = 100) : m_slice_num(slice) {
        update(limit);
        // Equals to DIV_ROUND_UP
        m_time_window_per_slice = photon::sat_add(time_window, (m_slice_num - 1)) / m_slice_num;
        m_time_window = m_time_window_per_slice * m_slice_num;
        for (auto& each: m_starving_slice_num) each = 0;
        int i = 0;
        for (auto& each: m_starving_slice_percent) each = get_starving_percent(Priority(i++));
        sem.signal(m_limit);
    }

    enum class Priority {
        High,
        Medium,
        Low,
        NumPriorities,
    };

    void update(uint64_t limit) {
        // Equals to DIV_ROUND_UP
        m_limit_per_slice = photon::sat_add(limit, (m_slice_num - 1)) / m_slice_num;
        m_limit = m_limit_per_slice * m_slice_num;
    }

    int consume(uint64_t amount, Priority prio = Priority::High) {
        uint64_t fulfil_percent = get_fulfill_percent(prio);
        uint64_t starving_percent = m_starving_slice_percent[int(prio)];

        // TODO: Handle the situation when throttle limit is extremely low
        assert(amount < m_limit);

        int ret = -1;
        int err = ETIMEDOUT;
        do {
            try_signal();
            auto& starving_slice_num = m_starving_slice_num[int(prio)];
            if (starving_slice_num * 100 > m_slice_num * starving_percent) {
                // Avoid the high priority requests consumed all tokens,
                // and make the low priority ones starving.
                starving_slice_num = 0;
                goto break_starving;
            }
            if (sem.count() * 100 < m_limit * fulfil_percent) {
                // Request are fulfilled only if they saw enough percent of tokens,
                // otherwise wait a `time_window_per_slice`.
                int sleep_ret = photon::thread_usleep(m_time_window_per_slice);
                if (sleep_ret != 0) {
                    // Interrupted, just return
                    return -1;
                }
                starving_slice_num++;
                continue;
            }
break_starving:
            ret = sem.wait_interruptible(amount, m_time_window_per_slice);
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

protected:
    // High priority is actually realtime
    static uint64_t get_fulfill_percent(Priority prio) {
        assert(prio < Priority::NumPriorities);
        switch (prio) {
            case Priority::Low:
                return 60;
            case Priority::Medium:
                return 30;
            case Priority::High:
            default:
                return 0;
        }
    }

    static uint64_t get_starving_percent(Priority prio) {
        assert(prio < Priority::NumPriorities);
        switch (prio) {
            case Priority::Low:
                return 20;
            case Priority::Medium:
                return 10;
            case Priority::High:
            default:
                return 0;
        }
    }

    uint64_t m_starving_slice_num[int(Priority::NumPriorities)] = {};
    uint64_t m_starving_slice_percent[int(Priority::NumPriorities)] = {};
};
}  // namespace photon
