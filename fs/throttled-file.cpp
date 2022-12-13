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

#include "throttled-file.h"
#include <inttypes.h>
#include <photon/common/ring.h>
#include "filesystem.h"
#include "forwardfs.h"
#include <photon/common/iovector.h>
#include <photon/common/utility.h>
#include <photon/common/alog.h>

using namespace std;

namespace photon {
namespace fs
{
    class StatisticsQueue
    {
    public:
        struct Sample
        {
            uint32_t time_stamp;
            uint32_t amount;
        };
        StatisticsQueue(uint32_t rate, uint32_t capacity) : m_events(capacity), m_rate(rate)
        {
            m_limit = (uint64_t)m_rate * m_time_window;
            m_timestamp_base = (photon::now / 1024UL) & ~((1UL<<29)-1);
        }
        const Sample& back() const
        {
            return m_events.back();
        }

        uint64_t try_pop()
        {
            auto now = photon::now / 1024UL;
            _update_timestamp_base(now);
            if (rate()) {
                auto w0 = now - m_time_window * 1024UL;
                uint64_t head_working_time = m_events.front().amount / rate() * 1024UL;
                while (!m_events.empty() && _get_time(m_events.front().time_stamp) < w0 &&
                    _get_time(m_events.front().time_stamp) + head_working_time <= now)
                {
                    m_sum -= m_events.front().amount;
                    m_events.pop_front();
                }
            }
            return now;
        }
        void push_back(uint32_t amount)
        {
            auto now = photon::now / 1024UL;
            if (rate()) {
                while (sum() >= limit()) {
                    uint64_t next_check = _get_time(m_events.front().time_stamp) + m_time_window * 1024UL;
                    if (next_check > now)
                        wait_for_pop((next_check - now) * 1024UL);
                    photon::thread_yield();
                    now = try_pop();
                }
                now = try_pop();
                if (m_events.empty() || _get_time(m_events.front().time_stamp) != now) {
                    while(m_events.full()) {
                        photon::thread_yield();
                        try_pop();
                    }
                    m_events.push_back(Sample{_get_stamp(now), amount});
                } else {
                    m_events.front().amount += amount;
                }
                m_sum += amount;
            }
        }
        uint64_t sum()
        {
            return m_sum;
        }
        uint64_t limit()
        {
            return m_limit;
        }
        uint32_t rate()
        {
            return m_rate;
        }
        uint64_t min_duration() // in us
        {
            if (rate())
                return sum() <= limit() ? 0 :   // use 1024 for 1000, for optimization
                    (sum() - limit()) * 1024 * 1024 / rate();
            else
                return 0;
        }
        void wait_for_push(uint64_t timeout = -1) {
            m_events.wait_for_push(timeout);
        }
        void wait_for_pop(uint64_t timeout = -1) {
            m_events.wait_for_pop(timeout);
        }

        RingQueue<Sample> m_events;
        uint32_t m_time_window = 1, m_rate;
        uint64_t m_sum = 0, m_limit;
        uint64_t m_timestamp_base = 0;

    protected:
        inline __attribute__((always_inline))
        void _update_timestamp_base(uint64_t now) {
            if (now > m_timestamp_base + ((1UL<<30) - 1)) {
                uint64_t new_base = now & ~((1UL<<29) - 1);
                for (size_t i = 0; i< m_events.size(); i++) {
                    m_events[i].time_stamp = m_events[i].time_stamp + m_timestamp_base - new_base;
                }
                m_timestamp_base = new_base;
            }
        }

        inline __attribute__((always_inline))
        uint64_t _get_time(uint32_t timestamp) {
            return m_timestamp_base + timestamp;
        }

        inline __attribute__((always_inline))
        uint32_t _get_stamp(uint64_t timems) {
            return timems - m_timestamp_base;
        }
    };

    struct scoped_queue
    {
        StatisticsQueue& _q;
        uint32_t _amount;
        uint64_t _ts_end;
        scoped_queue(StatisticsQueue& q, uint64_t count) :
            _q(q), _amount((uint32_t)(count))
        {
            _q.push_back(_amount);
            _ts_end = photon::now + q.min_duration();
        }
        ~scoped_queue()
        {
            if (photon::now < _ts_end)
                photon::thread_usleep(_ts_end - photon::now);
            _q.try_pop();
        }
    };

    struct scoped_semaphore
    {
        uint64_t m_count;
        photon::semaphore& m_sem;
        scoped_semaphore(photon::semaphore& sem, uint64_t count) :
            m_count(count), m_sem(sem)
        {
            m_sem.wait(m_count);
        }
        ~scoped_semaphore()
        {
            m_sem.signal(m_count);
        }
    };

    // 在iovector_view中分离出前`block_size`字节的部分区域
    // 会改写iovec[]中的边界元素，并在使用后恢复
    struct split_iovector_view : public iovector_view
    {
        iovec* end;
        iovec f0, b0;
        uint64_t remaining, count, block_size;
        split_iovector_view(const iovec* iov, int iovcnt, uint64_t block_size_) :
            iovector_view((iovec*)iov, iovcnt)
        {
            end = (iovec*)iov + iovcnt;
            block_size = block_size_;
            count = sum();
            init(block_size);
        }
        void init(uint64_t block_size)
        {
            f0 = front();
            do_shrink(block_size);
        }
        void do_shrink(uint64_t block_size)
        {
            remaining = shrink_less_than(block_size);
            b0 = back();
            back().iov_len -= remaining;
        }
        void next()
        {
            if (iovcnt > 1)
            {
                front() = f0;
            }
            if (remaining == 0)
            {
                back() = b0;
                iov += iovcnt;
                iovcnt = (int)(end - iov);
                init(block_size);
                return;
            }

            iov = &back();
            (char*&)iov->iov_base += iov->iov_len;
            if (remaining < block_size)
            {
                iov->iov_len = remaining;
                iovcnt = (int)(end - iov);
                f0 = b0;
                do_shrink(block_size);
            }
            else // if (remaining >= block_size)
            {
                iov->iov_len = block_size;
                iovcnt = 1;
                remaining -= block_size;
            }
        }
    };

    template<typename Func, typename Adv>
    static inline __attribute__((always_inline))
    ssize_t split_io(ALogStringL name, size_t count, size_t block_size,
                     const Func& func, const Adv& adv)
    {
        if (block_size == 0 || count <= block_size)
            return func(count);

        ssize_t cnt = 0;
        while(count > 0)
        {
            auto len = count;
            if (len > block_size)
                len = block_size;
            ssize_t ret = func(len);
            assert(ret <= (ssize_t)block_size);
            if (ret < (ssize_t)len)
            {
                if (ret >= 0) {
                    ret += cnt;
                    LOG_ERRNO_RETURN(0, ret, "failed to m_file->`(), EoF?", name);
                } else {
                    LOG_ERRNO_RETURN(0, -1, "failed to m_file->`()", name);
                }
            }
            adv((size_t)ret);
            count -= ret;
            cnt += ret;
        }
        return cnt;
    }

    struct Throttle
    {
        photon::semaphore num_io;
        StatisticsQueue iops;
        StatisticsQueue throughput;
        Throttle(const ThrottleLimits::UpperLimits& limits, uint32_t window) :
            num_io(limits.concurent_ops ? limits.concurent_ops : UINT32_MAX), // 0 for no limit, uint32_max to avoid overflow
            iops(limits.IOPS, window * 1024U),
            throughput(limits.throughput, window * 1024U)
        {
        }
    };

    struct ThrottleBundle{
        Throttle t_all, t_read, t_write;
        uint32_t r_block_size, w_block_size;
        ThrottleBundle(const ThrottleLimits &limits) :
            t_all(limits.RW, limits.time_window),
            t_read(limits.R, limits.time_window),
            t_write(limits.W, limits.time_window),
            r_block_size(limits.R.block_size),
            w_block_size(limits.W.block_size) {}
    };

    class ThrottledFile : public ForwardFile_Ownership
    {
    public:

        struct scoped_throttle
        {
            uint64_t count;
            scoped_semaphore sem1, sem2;
            scoped_queue q11, q12, q21, q22;
            scoped_throttle(Throttle& t1, Throttle& t2, uint64_t cnt) :
                count(cnt),
                sem1(t1.num_io, 1),
                sem2(t2.num_io, 1),
                q11(t1.iops, 1),
                q12(t2.iops, 1),
                q21(t1.throughput, count),
                q22(t2.throughput, count)
            {
            }
            scoped_throttle(Throttle& t1, Throttle& t2, const iovec* iov, int iovcnt) :
                scoped_throttle(t1, t2, iovector_view((iovec*)iov, iovcnt).sum())
            {
            }
        };

        ThrottleBundle *throttles;
        bool throttles_ownership;

        ThrottledFile(IFile *file, ThrottleBundle *throttle_bundle,
                      bool file_ownership = false, bool throttles_ownership = false)
            : ForwardFile_Ownership(file, file_ownership), throttles(throttle_bundle),
              throttles_ownership(throttles_ownership) {}

        ~ThrottledFile() {
            if (throttles_ownership)
                delete throttles;
        }

        virtual ssize_t pread(void *buf, size_t count, off_t offset) override
        {
            scoped_throttle t(throttles->t_all, throttles->t_read, count);
            return split_io(__func__, count, throttles->r_block_size,
                [&](size_t len) { return m_file->pread(buf, len, offset); },
                [&](size_t len) { offset += len; (char*&)buf += len; });
        }
        virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            return preadv2(iov, iovcnt, offset, 0);
        }
        virtual ssize_t preadv_mutable(struct iovec *iov, int iovcnt, off_t offset) override
        {
            return preadv2_mutable(iov, iovcnt, offset, 0);
        }
        virtual ssize_t preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            SmartCloneIOV<32> ciov(iov, iovcnt);
            split_iovector_view v(ciov.ptr, iovcnt, throttles->r_block_size);
            scoped_throttle t(throttles->t_all, throttles->t_read, v.count);
            return split_io(__func__, v.count, throttles->r_block_size,
                [&](size_t len) { return m_file->preadv2(v.iov, v.iovcnt, offset, flags);},
                [&](size_t len) { offset += len; v.next(); });
        }
        virtual ssize_t preadv2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            split_iovector_view v((iovec*)iov, iovcnt, throttles->r_block_size);
            scoped_throttle t(throttles->t_all, throttles->t_read, v.count);
            return split_io(__func__, v.count, throttles->r_block_size,
                [&](size_t len) { return m_file->preadv2(v.iov, v.iovcnt, offset, flags);},
                [&](size_t len) { offset += len; v.next(); });
        }
        virtual ssize_t read(void *buf, size_t count) override
        {
            scoped_throttle t(throttles->t_all, throttles->t_read, count);
            return split_io(__func__, count, throttles->r_block_size,
                [&](size_t len) { return m_file->read(buf, len);},
                [&](size_t len) { (char*&)buf += len; });
        }
        virtual ssize_t readv(const struct iovec *iov, int iovcnt) override
        {
            SmartCloneIOV<32> ciov(iov, iovcnt);
            split_iovector_view v(ciov.ptr, iovcnt, throttles->r_block_size);
            scoped_throttle t(throttles->t_all, throttles->t_read, v.count);
            return split_io(__func__, v.count, throttles->r_block_size,
                [&](size_t len) { return m_file->readv(v.iov, v.iovcnt);},
                [&](size_t len) { v.next(); });
        }
        virtual ssize_t readv_mutable(struct iovec *iov, int iovcnt) override
        {
            split_iovector_view v((iovec*)iov, iovcnt, throttles->r_block_size);
            scoped_throttle t(throttles->t_all, throttles->t_read, v.count);
            return split_io(__func__, v.count, throttles->r_block_size,
                [&](size_t len) { return m_file->readv((iovec*)v.iov, v.iovcnt);},
                [&](size_t len) { v.next(); });
        }
        virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override
        {
            scoped_throttle t(throttles->t_all, throttles->t_write, count);
            return split_io(__func__, count, throttles->w_block_size,
                [&](size_t len) { return m_file->pwrite(buf, len, offset); },
                [&](size_t len) { offset += len; (char*&)buf += len; });
        }
        virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            return pwritev2(iov, iovcnt, offset, 0);
        }
        virtual ssize_t pwritev_mutable(struct iovec *iov, int iovcnt, off_t offset) override
        {
            return pwritev2_mutable(iov, iovcnt, offset, 0);
        }
        virtual ssize_t pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            SmartCloneIOV<32> ciov(iov, iovcnt);
            split_iovector_view v(ciov.ptr, iovcnt, throttles->w_block_size);
            scoped_throttle t(throttles->t_all, throttles->t_write, v.count);
            return split_io(__func__, v.count, throttles->w_block_size,
                [&](size_t len) { return m_file->pwritev2(v.iov, v.iovcnt, offset, flags);},
                [&](size_t len) { offset += len; v.next(); });
        }
        virtual ssize_t pwritev2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags) override
        {
            split_iovector_view v((iovec*)iov, iovcnt, throttles->w_block_size);
            scoped_throttle t(throttles->t_all, throttles->t_write, v.count);
            return split_io(__func__, v.count, throttles->w_block_size,
                [&](size_t len) { return m_file->pwritev2(v.iov, v.iovcnt, offset, flags);},
                [&](size_t len) { offset += len; v.next(); });
        }
        virtual ssize_t write(const void *buf, size_t count) override
        {
            scoped_throttle t(throttles->t_all, throttles->t_write, count);
            return split_io(__func__, count, throttles->w_block_size,
                [&](size_t len) { return m_file->write(buf, len); },
                [&](size_t len) { (char*&)buf += len; });
        }
        virtual ssize_t writev(const struct iovec *iov, int iovcnt) override
        {
            SmartCloneIOV<32> ciov(iov, iovcnt);
            split_iovector_view v(ciov.ptr, iovcnt, throttles->w_block_size);

            scoped_throttle t(throttles->t_all, throttles->t_write, v.count);
            return split_io(__func__, v.count, throttles->w_block_size,
                [&](size_t len) { return m_file->writev(v.iov, v.iovcnt);},
                [&](size_t len) { v.next(); });
        }
        virtual ssize_t writev_mutable(struct iovec *iov, int iovcnt) override
        {
            split_iovector_view v((iovec*)iov, iovcnt, throttles->w_block_size);
            scoped_throttle t(throttles->t_all, throttles->t_write, v.count);
            return split_io(__func__, v.count, throttles->w_block_size,
                [&](size_t len) { return m_file->writev(v.iov, v.iovcnt);},
                [&](size_t len) { v.next(); });
        }
    };

    class ThrottledFs : public ForwardFS_Ownership {
    public:
        ThrottleBundle throttles;
        ThrottledFs(IFileSystem *fs, const ThrottleLimits &limits,
                    bool ownership = false)
            : ForwardFS_Ownership(fs, ownership), throttles(limits) {}

        ~ThrottledFs(){
        }
        virtual IFile *open(const char *pathname, int flags) override {
            auto file = m_fs->open(pathname, flags);
            if(file == nullptr) return nullptr;
            return new ThrottledFile(file, &throttles, true);
        }
        virtual IFile *open(const char *pathname, int flags, mode_t mode) override {
            auto file = m_fs->open(pathname, flags, mode);
            if(file == nullptr) return nullptr;
            return new ThrottledFile(file, &throttles, true);
        }
        virtual IFile *creat(const char *pathname, mode_t mode) override {
            auto file = m_fs->creat(pathname, mode);
            if(file == nullptr) return nullptr;
            return new ThrottledFile(file, &throttles, true);
        }
    };

    IFile *new_throttled_file(IFile *file, const ThrottleLimits &limits, bool ownership) {
        if (file == nullptr)
            LOG_ERROR_RETURN(EINVAL, nullptr, "cannot open file");
        auto bundle = new ThrottleBundle(limits);
        return new ThrottledFile(file, bundle, ownership, true);
    }

    IFileSystem *new_throttled_fs(IFileSystem *fs, const ThrottleLimits &limits,
                                  bool ownership) {
        return new ThrottledFs(fs, limits, ownership);
    }
}
}
