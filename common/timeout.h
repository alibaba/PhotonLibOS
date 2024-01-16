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

namespace photon {

extern volatile uint64_t now;

class Timeout
{
public:
    Timeout() = default; // never
    Timeout(uint64_t x)         { timeout(x); }
    uint64_t timeout(uint64_t x){ return m_expire = sat_add(now, x); }
    uint64_t timeout() const    { return sat_sub(m_expire, now); }
    operator uint64_t() const   { return timeout(); }
    operator bool () const      { return m_expire > now; }
    uint64_t timeout_us() const { return timeout(); }
    uint64_t timeout_ms() const { return divide(timeout(), 1000); }
    uint64_t timeout_MS() const { return divide(timeout(), 1024); }        // fast approximation
    uint64_t timeout_s() const  { return divide(timeout(), 1000 * 1000); }
    uint64_t timeout_S() const  { return divide(timeout(), 1024 * 1024); } // fast approximation
    uint64_t expiration() const { return m_expire; }
    uint64_t expiration(uint64_t x) {
                                  return m_expire = x; }
    bool operator < (const Timeout& rhs) const {
                                  return m_expire < rhs.m_expire; }
    Timeout& operator = (uint64_t x)     { timeout(x); return *this; }
    Timeout& operator = (const Timeout& rhs) = default;

    Timeout& timeout_at_most(uint64_t x) {
        x = sat_add(now, x);
        if (x < m_expire)
            m_expire = x;
        return *this;
    }

    uint64_t m_expire = -1;  // time of expiration, in us

    static uint64_t divide(uint64_t x, uint64_t divisor)
    {
        return (x + divisor / 2) / divisor;
    }
};

}
