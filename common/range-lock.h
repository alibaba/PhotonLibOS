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
#include <set>
#include <photon/thread/thread.h>

class RangeLock
{
public:
    // return 0 if successfully locked;
    // otherwise, wait and return -1 and the conflicted range
    int try_lock_wait(uint64_t& offset, uint64_t& length)
    {
        range_t r(offset, length);
        SCOPED_LOCK(m_lock);
        auto it = m_index.lower_bound(r);
        if (it != m_index.end() && it->offset < r.end()) {
            offset = it->offset;
            length = std::min(it->end(), r.end()) - offset;
            it->cond.wait(m_lock);
            return -1;
        } else {
            m_index.emplace_hint(it, r);
            return 0;
        }
    }

    void unlock(uint64_t offset, uint64_t length)
    {
        range_t r(offset, length);
        SCOPED_LOCK(m_lock);
        auto it = m_index.lower_bound(r);
        while (it != m_index.end() && it->offset < r.end())
        {
            if (r.contains(*it)) {
                it = m_index.erase(it);
            } else {
                ++it;
            }
        }
    }

    struct LockHandle;

    LockHandle* try_lock_wait2(uint64_t offset, uint64_t length)
    {
        range_t r(offset, length);
        SCOPED_LOCK(m_lock);
        auto it = m_index.lower_bound(r);
        if (it != m_index.end() && it->offset < r.end()) {
            it->cond.wait(m_lock);
            return nullptr;
        } else {
            it = m_index.emplace_hint(it, r);
            assert(it != m_index.end());
            static_assert(sizeof(it) == sizeof(LockHandle*), "...");
            return (LockHandle*&)it;
        }
    }

    LockHandle* lock(uint64_t offset, uint64_t length)
    {
        while (true)
        {
            auto h = try_lock_wait2(offset, length);
            if (h) return h;
        }
    }

    int adjust_range(LockHandle* h, uint64_t offset, uint64_t length)
    {
        if (!h) return -1;
        range_t r1(offset, length);
        SCOPED_LOCK(m_lock);
        auto it = (iterator&)h;
        auto r0 = (range_t*) &*it;
        if ((r1.offset < r0->offset && r1.offset < prev_end(it)) ||
            (r1.end()  > it->end()  && r1.end()  > next_offset(it)))
            return -1;
        r0->offset = r1.offset;
        r0->length = r1.length;
        return 0;
    }

    void unlock(LockHandle* h)
    {
        SCOPED_LOCK(m_lock);
        auto it = (iterator&)h;
        m_index.erase(it);
    }

protected:
    photon::spinlock m_lock;
    struct range_t
    {
        uint64_t offset;
        uint64_t length;
        range_t() { }
        range_t(uint64_t offset, uint64_t length)
        {
            this->offset = offset;
            this->length = length;
        }
        uint64_t end() const
        {
            return offset + length;
        }
        bool operator < (const range_t& rhs) const
        {
            return end() <= rhs.offset; // because end() is not inclusive
        }
        bool contains(const range_t& x) const
        {
            return offset <= x.offset && end() >= x.end();
        }
    }__attribute__((packed));
    struct Range : public range_t
    {
        using range_t::range_t;
        mutable photon::condition_variable cond;
        Range(const Range& rhs) : range_t(rhs) { }
        Range(const range_t& rhs) : range_t(rhs) { }
        ~Range() { cond.notify_all(); }
    };
    std::set<Range> m_index;
    typedef std::set<Range>::iterator iterator;
    uint64_t next_offset(iterator it)
    {
        return (++it == m_index.end()) ? (uint64_t)-1 : it->offset;
    }
    uint64_t prev_end(iterator it)
    {
        return (it == m_index.begin()) ? 0 : (--it)->end();
    }
};

class ScopedRangeLock
{
public:
    ScopedRangeLock(RangeLock& lock, uint64_t offset, uint64_t length) : _lock(&lock)
    {
        _h = _lock->lock(offset, length);
    }
    ~ScopedRangeLock()
    {
        _lock->unlock(_h);
    }

protected:
    RangeLock* _lock;
    RangeLock::LockHandle* _h;
};
