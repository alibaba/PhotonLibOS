/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <type_traits>

#include <photon/thread/thread.h>
#include <photon/common/utility.h>

namespace Metric {

class ValueCounter {
public:
    int64_t counter = 0;

    void set(int64_t x) { counter = x; }
    void reset() { counter = 0; }
    int64_t val() { return counter; }
};

class AddCounter {
public:
    int64_t counter = 0;

    void inc() { counter++; }
    void dec() { counter--; }
    void add(int64_t x) { counter += x; }
    void sub(int64_t x) { counter -= x; }
    void reset() { counter = 0; }
    int64_t val() { return counter; }
};

class AverageCounter {
public:
    int64_t sum = 0;
    int64_t cnt = 0;
    uint64_t time = 0;
    uint64_t m_interval = 60UL * 1000 * 1000;

    void normalize() {
        auto now = photon::now;
        if (now - time > m_interval * 2) {
            reset();
        } else if (now - time > m_interval) {
            sum = photon::sat_sub(sum, sum * (now - time - m_interval) / m_interval);
            cnt = photon::sat_sub(cnt, cnt * (now - time - m_interval) / m_interval);
            time = now - m_interval;
        }
    }
    void put(int64_t val) {
        normalize();
        sum += val;
        cnt++;
    }
    void reset() {
        sum = 0;
        cnt = 0;
        time = photon::now;
    }
    int64_t interval() { return m_interval; }
    int64_t interval(int64_t x) { return m_interval = x; }
    int64_t val() {
        normalize();
        return cnt ? sum / cnt : 0;
    }
};

class QPSCounter {
public:
    int64_t counter = 0;
    uint64_t time = photon::now;
    uint64_t m_interval = 1UL * 1000 * 1000;
    static constexpr uint64_t SEC = 1UL * 1000 * 1000;

    void normalize() {
        auto now = photon::now;
        if (now - time >= m_interval * 2) {
            reset();
        } else if (now - time > m_interval) {
            counter =
                photon::sat_sub(counter, counter * (now - time - m_interval) / m_interval);
            time = now - m_interval;
        }
    }
    void put(int64_t val = 1) {
        normalize();
        counter += val;
    }
    void reset() {
        counter = 0;
        time = photon::now;
    }
    uint64_t interval() { return m_interval; }
    uint64_t interval(uint64_t x) { return m_interval = x; }
    int64_t val() {
        normalize();
        return counter;
    }
};

class MaxCounter {
public:
    int64_t maxv = 0;

    void put(int64_t val) {
        if (val > maxv) {
            maxv = val;
        }
    }
    void reset() { maxv = 0; }
    int64_t val() { return maxv; }
};

class IntervalMaxCounter {
public:
    int64_t maxv = 0, last_max = 0;
    uint64_t time = 0;
    uint64_t m_interval = 5UL * 1000 * 1000;

    void normalize() {
        if (photon::now - time >= 2 * m_interval) {
            // no `val` or `put` call in 2 intervals
            // last interval max must become 0
            reset();
        } else if (photon::now - time > m_interval) {
            // one interval passed
            // current maxv become certainly max val in last interval
            last_max = maxv;
            maxv = 0;
            time = photon::now;
        }
    }

    void put(int64_t val) {
        normalize();
        maxv = val > maxv ? val : maxv;
    }

    void reset() {
        maxv = 0;
        last_max = 0;
        time = photon::now;
    }

    uint64_t interval() { return m_interval; }

    uint64_t interval(uint64_t x) { return m_interval = x; }

    int64_t val() {
        normalize();
        return maxv > last_max ? maxv : last_max;
    }
};

template <typename LatencyCounter>
class LatencyMetric {
public:
    LatencyCounter& counter;
    uint64_t start;

    explicit LatencyMetric(LatencyCounter& counter)
        : counter(counter), start(photon::now) {}

    // no copy or move;
    LatencyMetric(LatencyMetric&&) = delete;
    LatencyMetric(const LatencyMetric&) = delete;
    LatencyMetric& operator=(LatencyMetric&&) = delete;
    LatencyMetric& operator=(const LatencyMetric&) = delete;

    ~LatencyMetric() { counter.put(photon::now - start); }
};

class AverageLatencyCounter : public AverageCounter {
public:
    using MetricType = LatencyMetric<AverageLatencyCounter>;
};

class MaxLatencyCounter : public IntervalMaxCounter {
public:
    using MetricType = LatencyMetric<IntervalMaxCounter>;
};

#define SCOPE_LATENCY(x)                                                    \
    std::decay<decltype(x)>::type::MetricType _CONCAT(__audit_start_time__, \
                                                      __LINE__)(x);

}  // namespace Metric
