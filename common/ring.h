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
#include <utility>
#include <unistd.h>
#include <cstdlib>
#include <photon/common/utility.h>
#include <photon/thread/thread.h>

// RingBase exists only to reduce the size of template RingQueue
class RingBase
{
public:
    uint32_t capacity() const               { return m_capacity; }
    uint32_t size() const                   { return mod(m_end - m_begin); }
    bool empty() const                      { return m_begin == m_end; }
    bool full() const                       { return next_end() == m_begin; }
    void wait_for_push(uint64_t timeout=-1) { m_cond_push.wait_no_lock(timeout); }
    void wait_for_pop(uint64_t timeout=-1)  { m_cond_pop.wait_no_lock(timeout); }

protected:
    uint32_t m_begin = 0, m_end = 0, m_capacity, m_mask;
    photon::condition_variable m_cond_pop, m_cond_push;

    uint32_t set_capacity(uint32_t& capacity)
    {
        m_capacity = round_up_to_exp2(capacity);
        m_mask = m_capacity - 1;
        return m_capacity;
    }

    int ensure_not_full()
    {
        while(full())
            if (m_cond_pop.wait_no_lock() < 0)
                return -1;
        return 0;
    }

    int ensure_not_empty()
    {
        while(empty())
            if (m_cond_push.wait_no_lock() < 0)
                return -1;
        return 0;
    }

    RingBase() { }
    uint32_t mod(uint32_t i) const { return i & m_mask; }
    uint32_t next_end() const      { return mod(m_end + 1); }
    uint32_t next_begin() const    { return mod(m_begin + 1); }
};


template<typename T>
class RingQueue : public RingBase
{
public:
    // `capacity` will be rounded up to 2^n
    RingQueue(uint32_t capacity_2expn)
    {
        set_capacity(capacity_2expn);

        // don't use new [], as it calls constructors
        m_buf = (T*)malloc(m_capacity * sizeof(T));
    }
    ~RingQueue()                { clear(); free(m_buf); }
    T& front()                  { return m_buf[m_begin]; }
    T& back()                   { return m_buf[mod(m_end - 1)]; }
    T& operator [] (uint64_t i) { return m_buf[mod(m_begin + i)]; }
    const T& front() const      { return m_buf[m_begin]; }
    const T& back()  const      { return m_buf[mod(m_end - 1)]; }
    const T& operator [] (uint64_t i) const { return m_buf[mod(m_begin + i)]; }

    template<typename U>
    void push_back(U&& x)
    {
        if (ensure_not_full() < 0)
            return;
        m_buf[m_end] = std::forward<U>(x);
        m_end = next_end();
        m_cond_push.notify_one();
    }
    void pop_front()
    {
        if (ensure_not_empty() < 0)
            return;
        m_buf[m_begin].~T();
        m_begin = next_begin();
        m_cond_pop.notify_one();
    }
    void pop_front(T& lhs)
    {
        if (ensure_not_empty() < 0)
            return;
        lhs = front();
        pop_front();
    }
    void clear()
    {
        while(!empty())
            pop_front();
    }

protected:
    T* m_buf;
};


class RingBuffer : public RingQueue<char>
{
public:
    using RingQueue::RingQueue;
    ssize_t read(void *buf, size_t count)
    {
        photon::scoped_lock lock(m_read_lock);
        return do_read(buf, count);
    }
    ssize_t write(const void *buf, size_t count)
    {
        photon::scoped_lock lock(m_write_lock);
        return do_write(buf, count);
    }
    ssize_t readv(const struct iovec *iov, int iovcnt);
    ssize_t writev(const struct iovec *iov, int iovcnt);

protected:
    photon::mutex m_read_lock, m_write_lock;
    ssize_t do_read(void* buf, size_t end);
    ssize_t do_write(const void* buf, size_t end);
    ssize_t do_read(void*& buf, uint64_t& count, uint64_t end);
    ssize_t do_write(const void*& buf, uint64_t& count, uint64_t end);
};
