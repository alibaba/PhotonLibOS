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

#include <photon/fs/cache/pool_store.h>
#include <photon/fs/cache/cache.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog-audit.h>
#include <photon/common/io-alloc.h>
#include <photon/common/iovector.h>
#include <photon/common/expirecontainer.h>
#include <photon/thread/thread-pool.h>


namespace photon{
namespace fs {

static const uint32_t MAX_REFILLING = 128;

ICacheStore::~ICacheStore()
{
    delete recycle_file_;
    if (src_file_ == src_rwfile_) {
        delete src_file_;
    } else {
        delete src_file_;
        delete src_rwfile_;
    }
}

ssize_t ICacheStore::preadv2(const struct iovec* iov, int iovcnt, off_t offset, int flags)
{
    if (offset < 0) LOG_ERROR_RETURN(EINVAL, -1, "offset is invalid, offset : `", offset);
    iovector_view view(const_cast<struct iovec*>(iov), iovcnt);
    size_t vsize = view.sum();
    if (0u == vsize) return 0;
    if (offset >= actual_size_ || offset + static_cast<off_t>(vsize) > actual_size_) {
        if (tryget_size() != 0) {
            LOG_ERROR_RETURN(0, -1,
                "try get size failed, actual_size_ : `, offset : `, count : `",
                actual_size_, offset, vsize);
        }
    }

again:
    off_t actual_size = actual_size_;
    if (offset >= actual_size) return 0;
    IOVector input(iov, iovcnt);
    size_t iov_size = vsize;
    if (offset + static_cast<off_t>(iov_size) > actual_size) {
        input.extract_back(offset + static_cast<off_t>(iov_size) - actual_size);
        iov_size = actual_size - offset;
    }

    if ((flags&RW_V2_CACHE_ONLY) || (open_flags_&O_CACHE_ONLY)) {
        auto tr = try_preadv2(input.iovec(), input.iovcnt(), offset, flags|RW_V2_CACHE_ONLY);
        if (tr.refill_size == 0 && tr.size >= 0) { return tr.size; } else { return -1; }
    }

    auto tr = try_preadv2(input.iovec(), input.iovcnt(), offset, flags);
    if (tr.refill_size == 0 && tr.size >= 0) return tr.size;
    // open src file only when cache miss
    if (open_src_file(&src_file_) != 0 || !src_file_) {
        LOG_ERROR_RETURN(0, -1,
            "cache preadv2 failed, offset : `, count : `, flags : `", offset, iov_size, flags);
    }

    if (tr.refill_offset < 0) {
        if (open_flags_&O_WRITE_BACK)
            LOG_ERROR_RETURN(0, tr.refill_offset,
                "cache preadv2 failed, offset : `, count : `, flags : `", offset, iov_size, flags);
        SCOPE_AUDIT("download", AU_FILEOP(get_src_name(), offset, tr.size));
        tr.size = src_file_->preadv2(input.iovec(), input.iovcnt(), offset, flags);
        return tr.size;
    }

    ssize_t ret = do_refill_range(tr.refill_offset, tr.refill_size, iov_size, actual_size, &input, offset, flags);
    if (ret == -EAGAIN) goto again;
    return ret;
}

ssize_t ICacheStore::pwritev2(const struct iovec* iov, int iovcnt, off_t offset, int flags)
{
    if ((open_flags_&(O_WRITE_THROUGH|O_CACHE_ONLY|O_WRITE_BACK)) ||
        (flags&(RW_V2_CACHE_ONLY|RW_V2_WRITE_BACK))) {
        return pwritev2_extend(iov, iovcnt, offset, flags);
    }

    iovector_view view(const_cast<struct iovec*>(iov), iovcnt);
    size_t size = view.sum();
    if (offset >= actual_size_ || offset + static_cast<off_t>(size) > actual_size_) {
        if (tryget_size() != 0) {
            LOG_ERROR_RETURN(0, -1,
                "try get size failed, actual_size_ : `, offset : `, count : `",
                actual_size_, offset, size);
        }
    }

    off_t actual_size = actual_size_;
    if (offset >= actual_size) return 0;
    if (offset % page_size_ != 0 || (size % page_size_ != 0 && offset + static_cast<off_t>(size) < actual_size)) {
        LOG_ERROR_RETURN(EINVAL, -1,
            "size or offset is not aligned to `, size : `, offset : `", page_size_, size, offset);
    }

    if (offset + static_cast<off_t>(size) <= actual_size) {
        return do_pwritev2(iov, iovcnt, offset, flags);
    }

    IOVector io_vector(iov, iovcnt);
    if (offset + static_cast<off_t>(size) > actual_size) {
        auto ret = io_vector.extract_back(size - (actual_size - offset));
        if (ret != size - (actual_size - offset))
            LOG_ERRNO_RETURN(EINVAL, -1,
                "extract failed, extractSize : `, expected : ", ret, size - (actual_size - offset))
    }

    auto write = do_pwritev2(io_vector.iovec(), io_vector.iovcnt(), offset, flags);
    if (write != static_cast<ssize_t>(io_vector.sum())) {
        if (ENOSPC != errno)
            LOG_ERROR("cache file write failed : `, error : `, actual_size : `, offset : `, sum : `",
                write, ERRNO(errno), actual_size, offset, io_vector.sum());
    }

    return write;
}

ssize_t ICacheStore::try_refill_range(off_t offset, size_t count) {
    if (offset >= actual_size_ || offset + count > (size_t)actual_size_) {
        if (tryget_size() != 0) {
            LOG_ERROR_RETURN(0, -1,
                "try get size failed, actual_size_ : `, offset : `, count : `",
                actual_size_, offset, count);
        }
    }

again:
    off_t actual_size = actual_size_;
    if (offset >= actual_size) return 0;
    if (offset + count > (size_t)actual_size) {
        count = actual_size - offset;
    }

    auto qres = queryRefillRange(offset, count);
    if (qres.first < 0) return -1;
    if (qres.second == 0) return static_cast<ssize_t>(count);
    // open src file only when cache miss
    if (open_src_file(&src_file_) != 0 || !src_file_) {
        LOG_ERROR_RETURN(0, -1,
            "try refill_range failed due to null src file, offset : `, count : `", offset, count);
    }

    ssize_t ret = do_refill_range(qres.first, qres.second, count, actual_size);
    if (ret == -EAGAIN) goto again;
    return ret;
}

struct RefillContext {
    ICacheStore* store;
    IOVector buffer;
    uint64_t refill_off;
    uint64_t refill_size;
    int flags;
    int pinRet;
    void* pin_wresult;
};

void* ICacheStore::async_refill(void* args) {
    auto ctx = (RefillContext*)args;
    ssize_t write = 0;
    if (ctx->pinRet == 0) write = static_cast<IMemCacheStore*>(ctx->store)->unpin_wbuf(ctx->pin_wresult, 0, ctx->flags);
        else write = ctx->store->do_pwritev2(ctx->buffer.iovec(), ctx->buffer.iovcnt(), ctx->refill_off, ctx->flags);
    if (write != static_cast<ssize_t>(ctx->refill_size)) {
        if (ENOSPC != errno)
            LOG_ERROR("cache file write failed : `, error : `, actual_size_ : `, offset : `, sum : `",
                write, ERRNO(errno), ctx->store->actual_size_, ctx->refill_off, ctx->buffer.sum());
    }

    ctx->store->pool_->m_refilling.fetch_sub(1, std::memory_order_relaxed);
    ctx->store->range_lock_.unlock(ctx->refill_off, ctx->refill_size);
    ctx->store->release();
    photon::thread_migrate(photon::CURRENT, static_cast<photon::vcpu_base*>(ctx->store->pool_->m_vcpu));
    delete ctx;
    return nullptr;
}

ssize_t ICacheStore::do_refill_range(uint64_t refill_off, uint64_t refill_size, size_t count, off_t actual_size, IOVector* input, off_t offset, int flags) {
    ssize_t ret = 0;
    if (!(open_flags_&O_WRITE_BACK) && input && !(flags&(RW_V2_WRITE_BACK|RW_V2_SYNC_MODE)) && pool_ &&
        pool_->m_refilling.load(std::memory_order_relaxed) >= pool_->m_refilling_threshold) {
        SCOPE_AUDIT("download", AU_FILEOP(get_src_name(), offset, ret));
        ret = src_file_->preadv2(input->iovec(), input->iovcnt(), offset, flags);
        return ret;
    }

    if (refill_off + refill_size > static_cast<uint64_t>(actual_size)) refill_size = actual_size - refill_off;
    ret = range_lock_.try_lock_wait(refill_off, refill_size);
    if (ret < 0) return -EAGAIN;
    {
        static uint32_t max_refilling = pool_ ? pool_->m_max_refilling : MAX_REFILLING;
        uint32_t refilling = max_refilling;
        DEFER({ if (refilling >= max_refilling) range_lock_.unlock(refill_off, refill_size); });
        if (actual_size != actual_size_) return -EAGAIN;
        IOVector buffer(*allocator_);
        void* pin_wresult = nullptr;
        int pinRet = -1;
        if (pool_ && pool_->m_pin_write && !(open_flags_&O_WRITE_BACK) && !(flags&RW_V2_WRITE_BACK))
            pinRet = static_cast<IMemCacheStore*>(this)->pin_wbuf(refill_off, refill_size, &buffer, &pin_wresult);
        if (pinRet != 0) {
            auto alloc = buffer.push_back(refill_size);
            if (alloc < refill_size) {
                LOG_ERROR("memory allocate failed, refill_size:`, alloc:`", refill_size, alloc);
                if (!(open_flags_&O_WRITE_BACK) && input) {
                    SCOPE_AUDIT("download", AU_FILEOP(get_src_name(), offset, ret));
                    ret = src_file_->preadv2(input->iovec(), input->iovcnt(), offset, flags);
                    return ret;
                } else return -1;
            }
        }

        {
            SCOPE_AUDIT("download", AU_FILEOP(get_src_name(), refill_off, ret));
            ret = src_file_->preadv2(buffer.iovec(), buffer.iovcnt(), refill_off, flags);
        }

        if (ret != static_cast<ssize_t>(refill_size)) {
            if (pinRet == 0) (void)static_cast<IMemCacheStore*>(this)->unpin_wbuf(pin_wresult, -1, flags);
            LOG_ERRNO_RETURN(0, -1,
                "src file read failed, read : `, expectRead : `, actual_size_ : `, offset : `, sum : `",
                ret, refill_size, actual_size_, refill_off, buffer.sum());
        }

        // buffer need async refill
        IOVector refill_buf(buffer.iovec(), buffer.iovcnt());
        if ((open_flags_&O_WRITE_BACK) || !input) {
            ret = 0;
        } else if ((off_t)refill_off <= offset) {
            auto view = input->view();
            refill_buf.extract_front(offset - refill_off);
            ret = refill_buf.memcpy_to(&view, count);
            offset += ret;
        } else if (refill_off + refill_size >= offset + count) {
            iovector_view tail_iov;
            tail_iov.iovcnt = 0;
            input->slice(count - (refill_off- offset), refill_off- offset, &tail_iov);
            ret = refill_buf.memcpy_to(&tail_iov);
            input->extract_back(ret);
        } else ret = 0;

        if (!(open_flags_&O_WRITE_BACK) && input && ret != 0 && !(flags&(RW_V2_WRITE_BACK|RW_V2_SYNC_MODE)) && pool_ &&
            pool_->m_thread_pool && (refilling=pool_->m_refilling.load(std::memory_order_relaxed)) < pool_->m_max_refilling) {
            pool_->m_refilling.fetch_add(1, std::memory_order_relaxed);
            ref_.fetch_add(1, std::memory_order_relaxed);
            auto ctx = new RefillContext{this, std::move(buffer), refill_off, refill_size, flags, pinRet, pin_wresult};
            auto th = static_cast<photon::ThreadPoolBase*>(pool_->m_thread_pool)->thread_create(&async_refill, ctx);
            photon::thread_migrate(th, photon::get_vcpu());
        } else {
            if (pool_) pool_->m_refilling.fetch_add(1, std::memory_order_relaxed);
            ssize_t write = 0;
            if (pinRet == 0) write = static_cast<IMemCacheStore*>(this)->unpin_wbuf(pin_wresult, 0, flags);
                else write = do_pwritev2(buffer.iovec(), buffer.iovcnt(), refill_off, flags);
            if (pool_) pool_->m_refilling.fetch_sub(1, std::memory_order_relaxed);
            if (write != static_cast<ssize_t>(refill_size)) {
                if (ENOSPC != errno)
                    LOG_ERROR("cache file write failed : `, error : `, actual_size_ : `, offset : `, sum : `",
                        write, ERRNO(errno), actual_size_, refill_off, buffer.sum());
                if (!input) return -1;
            }
        }
    }

    if (input && ret != (ssize_t)count) {
        auto tr = try_preadv2(input->iovec(), input->iovcnt(), offset, flags|RW_V2_CACHE_ONLY);
        if (tr.refill_size != 0 || tr.size < 0) {
            if (open_flags_&O_WRITE_BACK)
               LOG_ERROR_RETURN(0, -1,
                   "preadv2 failed, ret:`, offset:`,sum:`,actual_size_:`", tr.size, offset, input->sum(), actual_size_);
            SCOPE_AUDIT("download", AU_FILEOP(get_src_name(), offset, tr.size));
            tr.size = src_file_->preadv2(input->iovec(), input->iovcnt(), offset, flags);
            if (tr.size + ret != static_cast<ssize_t>(count))
                LOG_ERRNO_RETURN(0, -1, "read failed, ret:`, offset:`,sum:`,actual_size_:`",
                    tr.size, offset, input->sum(), actual_size_);
        }
    }

    return count;
}

void ICacheStore::set_cached_size(off_t cached_size, int flags)
{
    if (cached_size == cached_size_) return;
    if (cached_size < cached_size_) {
        evict(cached_size, -1UL, flags);
        return;
    }

    off_t size = cached_size_;
    if (size % page_size_ != 0) {
        evict(size / page_size_ * page_size_, -1UL, flags);
    }
}

ICacheStore::try_preadv_result ICacheStore::try_preadv2(
    const struct iovec* iov, int iovcnt, off_t offset, int flags)
{
    try_preadv_result rst;
    iovector_view view((iovec*)iov, iovcnt);
    rst.iov_sum = view.sum();
    auto q = queryRefillRange(offset, rst.iov_sum);
    if (q.first >= 0 && q.second == 0) { // no need to refill
        rst.refill_size = 0;
        rst.size = do_preadv2(iov, iovcnt, offset, flags);
        if (rst.size != (ssize_t)rst.iov_sum) {
            rst.refill_size = (size_t)-1;
            rst.refill_offset = -1;
        }
   } else {
        rst.refill_size = q.second;
        rst.refill_offset = q.first;
    }

    return rst;
}

ssize_t ICacheStore::do_preadv2(const struct iovec* iov, int iovcnt, off_t offset, int flags)
{
    SmartCloneIOV<32> ciov(iov, iovcnt);
    return do_preadv2_mutable(ciov.iov, iovcnt, offset, flags);
}

ssize_t ICacheStore::do_preadv2_mutable(struct iovec* iov, int iovcnt, off_t offset, int flags)
{
    return do_preadv2(iov, iovcnt, offset, flags);
}

ssize_t ICacheStore::do_pwritev2(const struct iovec* iov, int iovcnt, off_t offset, int flags)
{
    SmartCloneIOV<32> ciov(iov, iovcnt);
    return do_pwritev2_mutable(ciov.iov, iovcnt, offset, flags);
}

ssize_t ICacheStore::do_pwritev2_mutable(struct iovec* iov, int iovcnt, off_t offset, int flags)
{
    return do_pwritev2(iov, iovcnt, offset, flags);
}

int ICacheStore::open_src_file(IFile** src_file, int flags)
{
    if (!src_fs_ || (open_flags_&O_CACHE_ONLY)) return 0;
    photon::scoped_lock l(open_lock_);
    if (*src_file) return 0;
    *src_file = src_fs_->open(src_name_.c_str(), flags, 0644);
    if (!(*src_file)) LOG_ERRNO_RETURN(0, -1, "open source ` failed flags `", src_name_.c_str(), flags);
    if (*src_file == src_rwfile_) {
        recycle_file_ = src_file_;
        src_file_ = *src_file;
    }

    return 0;
}

ssize_t ICacheStore::pwritev2_extend(const struct iovec *iov, int iovcnt, off_t offset, int flags)
{
    iovector_view view(const_cast<struct iovec*>(iov), iovcnt);
    size_t len = 0, size = view.sum();
    off_t cached_size = 0;
    if ((offset > actual_size_ || size % page_size_ != 0) &&
        src_fs_ && !(open_flags_&O_CACHE_ONLY) && !(flags&RW_V2_CACHE_ONLY) &&
        tryget_size() != 0 && errno != ENOENT)
        LOG_ERROR_RETURN(0, -1,
            "try get size failed, actual_size_ : `, offset : `, size: `", actual_size_, offset, size);
rewrite:
    cached_size = cached_size_;
    len = size;
    if (offset % page_size_ != 0 ||
        (offset > actual_size_ && actual_size_ % page_size_ != 0) ||
        (size % page_size_ != 0 && offset + static_cast<off_t>(size) < actual_size_)) {
        LOG_ERROR_RETURN(EINVAL, -1,
            "offset or size is invalid, page size: `, actual_size_ : `, offset : `, size: `",
            page_size_, actual_size_, offset, size);
    }

    if (offset + (off_t)size >= cached_size) {
        if (cached_size % page_size_ != 0) {
            evict(cached_size / page_size_ * page_size_);
            goto rewrite;
        } else if (size % page_size_ != 0) len = -1UL;
    }

    auto lh = range_lock_.lock(offset, len);
    DEFER(range_lock_.unlock(lh));
    if (len == -1UL && cached_size != cached_size_) goto rewrite;
    auto write = do_pwritev2(iov, iovcnt, offset, flags);
    if (write != static_cast<ssize_t>(size)) {
        if (ENOSPC != errno)
            LOG_ERROR("cache file write failed : `, error : `, cached_size: `, offset : `, sum : `",
                write, ERRNO(errno), cached_size, offset, size);
    }

    return write;
}

int ICacheStore::tryget_size()
{
    if (actual_size_ % page_size_ != 0 || truncated_) return 0;
    if (open_src_file(&src_file_) != 0) return -1;
    struct stat buf;
    buf.st_size = 0;
    if ((src_file_ && src_file_->fstat(&buf) != 0) || (!src_file_ && fstat(&buf) != 0)) return -1;
    if (cached_size_ < buf.st_size) set_cached_size(buf.st_size);
    SCOPED_LOCK(mt_);
    if (actual_size_ < buf.st_size && !truncated_) {
        actual_size_ = buf.st_size;
    }
    return 0;
}

ssize_t ICacheStore::do_prefetch(size_t count, off_t offset, int flags, uint64_t batch_size) {
    uint64_t end = photon::sat_add(offset, count);
    if (offset % page_size_ != 0) {
        offset = offset / page_size_ * page_size_;
    }
    if (end % page_size_ != 0) {
        end = photon::sat_add(end, page_size_ - 1) / page_size_ * page_size_;
    }
    uint64_t remain = end - offset;
    ssize_t read = 0;
    while (remain > 0) {
        off_t min = std::min(batch_size, remain);
        remain -= min;
        auto ret = try_refill_range(offset, static_cast<size_t>(min));
        if (ret < 0) {
            LOG_ERRNO_RETURN(0, -1, "try_refill_range failed, ret : `, len : `, offset : `, flags : `",
                             ret, min, offset, flags);
        }
        read += ret;
        //  read end of file.
        if (ret < min) {
            return read;
        }
        offset += ret;
    }
    return read;
}

}
} // namespace photon::fs
