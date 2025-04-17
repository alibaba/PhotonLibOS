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
#include <cinttypes>
#include <photon/common/utility.h>
#ifdef private
#define _PHOTON_UNIT_TEST
#undef private
#undef protected
#endif
#include <chrono>
#ifdef _PHOTON_UNIT_TEST
#undef _PHOTON_UNIT_TEST
#define private public
#define protected public
#endif

namespace photon {

extern volatile uint64_t now;

class Timeout {
protected:
    uint64_t m_expiration = -1;  // time of expiration, in us

public:
    Timeout() = default; // never timeout
    Timeout(uint64_t x)              { m_expiration = x ? sat_add(now, x) : 0; }
    uint64_t timeout(uint64_t x)     { return m_expiration = sat_add(now, x); }
    uint64_t timeout() const         { return sat_sub(m_expiration, now); }
    operator uint64_t() const        { return timeout(); }
    bool expired() const             { return (m_expiration == 0) || (m_expiration <= now); }
    uint64_t timeout_us() const      { return timeout(); }
    uint64_t timeout_ms() const      { return divide(timeout(), 1000); }
    uint64_t timeout_MS() const      { return divide(timeout(), 1024); } // fast approximation
    uint64_t timeout_s() const       { return divide(timeout(), 1000 * 1000); }
    uint64_t timeout_S() const       { return divide(timeout(), 1024 * 1024); } // fast approximation
    uint64_t expiration() const      { return m_expiration; }
    uint64_t expiration(uint64_t x)  { return m_expiration = x; }
    Timeout& operator = (uint64_t x) { timeout(x); return *this; }
    Timeout& operator = (const Timeout& rhs) = default;
    bool operator < (const Timeout& rhs) const {
        return m_expiration < rhs.m_expiration;
    }
    Timeout& timeout_at_most(uint64_t x) {
        x = sat_add(now, x);
        if (x < m_expiration)
            m_expiration = x;
        return *this;
    }
    auto std_duration() const {
        using us = std::chrono::microseconds;
        uint64_t max = std::numeric_limits<us::rep>::max();
        return (m_expiration > max) ? us::max() : us(timeout());
    }

protected:
    operator bool() const = delete;
    static uint64_t divide(uint64_t x, uint64_t divisor) {
        return (x + divisor / 2) / divisor;
    }
};

}
