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
#include <string>
#include <vector>
#include <assert.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <photon/common/callback.h>
#include <photon/common/string_view.h>
#include <photon/common/object.h>
#include <photon/common/range-lock.h>
#include <photon/common/iovector.h>
#include <photon/fs/filesystem.h>

enum ListType : int {
  LIST_ALL = 0,
  LIST_FILES = 1,
  LIST_DIRS = 2,
};

// reset cache flags
enum ResetType : int {
  RST_ALL = 0x0,  // reset all cache's data, include file meta
  RST_MEMORY = 0x1,  // reset memory cache's data
  RST_DISK = 0x2,  // reset disk cache's data
  RST_UPPER = 0x10,  // reset upper layer cache's data
  RST_LOWER = 0x20,  // reset lower layer cache's data
};

// resize cache flags
enum ResizeType : int {
  RSZ_MEMORY = 0x1,  // resize memory cache's capacity
  RSZ_DISK = 0x2,  // resize disk cache's capacity
  RSZ_UPPER = 0x10,  // resize upper layer cache's capacity
  RSZ_LOWER = 0x20,  // resize lower layer cache's capacity
};

namespace photon
{
namespace fs
{
    // `CacheFnTransFunc` use to transform the filename in the cached store.
    // `std::string_view` is the filename before transformation (as src_name).
    // `char *` is the transformed filename (as store_key).
    // `size_t` is the max buffer length of store_key.
    // If transform occurs an error (such as result length more than buffer size)
    // or there is not necessary to transform, this function returns 0,
    // otherwise, it returns string length after transformation.
    using CacheFnTransFunc = Delegate<size_t, std::string_view, char *, size_t>;
    class ICacheStore;
    struct CacheStat
    {
        uint32_t struct_size = sizeof(CacheStat);
        uint32_t refill_unit;   // in bytes
        uint32_t total_size;    // in refill_unit
        uint32_t used_size;     // in refill_unit
        uint64_t evict_other;   // in bytes, initialized to -1UL means reset
        uint64_t evict_global;  // in bytes, initialized to -1UL means reset
        uint64_t evict_user;    // in bytes, initialized to -1UL means reset
    };

    class ICachePool : public Object
    {
    public:
        ICachePool(uint32_t pool_size = 128, uint32_t max_refilling = 128, uint32_t refilling_threshold = -1U, bool pin_write = false);
        ~ICachePool();

        ICacheStore* open(std::string_view filename, int flags, mode_t mode);

        // set quota to a dir or a file
        virtual int set_quota(std::string_view pathname, size_t quota) = 0;

        // if pathname is {nullptr, 0} or "/", returns the overall stat
        // if pathname is a dir, and it has quota set, returns its quota usage
        // if pathname is a file, returns the file's stat
        virtual int stat(CacheStat* stat,
            std::string_view pathname = std::string_view(nullptr, 0)) = 0;

        // force to evict specified files(s)
        virtual int evict(std::string_view filename) = 0;

        // try to evict at least `size` bytes, and also make sure
        // available space meet other requirements as well
        virtual int evict(size_t size = 0) = 0;

        int store_release(ICacheStore* store, bool detach = false);

        void stores_clear();

        void set_trans_func(CacheFnTransFunc fn_trans_func);

        virtual ICacheStore* do_open(std::string_view filename, int flags, mode_t mode) = 0;

        virtual int rename(std::string_view oldname, std::string_view newname) = 0;

        virtual ssize_t list(const char* dirname, ListType type,
            const struct iovec *iov, int iovcnt, const char* marker, uint32_t count)
        {
            errno = ENOSYS;
            return -1;
        }

        UNIMPLEMENTED_POINTER(void* get_underlay_object(int i = 0));

        // reset cache's data
        virtual int reset(int flags = 0)
        {
            errno = ENOSYS;
            return -1;
        }

        // resize cache's capacity
        virtual int resize(size_t n, int flags = 0)
        {
            errno = ENOSYS;
            return -1;
        }

        // flush all dirty pages to source fs, nonblocking
        // return 0 means flushed
        // return >0 means in flushing
        // return <0 means errors occurred
        virtual int sync_pool()
        {
            errno = ENOSYS;
            return -1;
        }

    protected:
        void* m_stores;
        CacheFnTransFunc fn_trans_func;
        void* m_thread_pool = nullptr;
        void* m_vcpu = nullptr;  // vcpu where m_thread_pool is created
        std::atomic<uint32_t> m_refilling{0};
        const uint32_t m_max_refilling = 128;
        const uint32_t m_refilling_threshold = -1U;
        bool m_pin_write = false;
        friend class ICacheStore;
    };

    class ICacheStore : public Object
    {
    public:
        virtual ~ICacheStore();
        // public interface for reading cache file store, dealing with cache-miss
        // and deduplication of concurrent reading of source file.
        ssize_t preadv2(const struct iovec* iov, int iovcnt, off_t offset, int flags);
        ssize_t pwritev2(const struct iovec* iov, int iovcnt, off_t offset, int flags);
        virtual ssize_t prefetch(size_t count, off_t offset, int flags) {
            if (offset < 0) offset = 0;
            return do_prefetch(count, offset, flags);
        }

        virtual int set_quota(size_t quota) = 0;
        virtual int stat(CacheStat* stat) = 0;
        virtual int evict(off_t offset, size_t count = -1, int flags = 0) = 0;
        // offset + size must <= origin file size
        virtual std::pair<off_t, size_t> queryRefillRange(off_t offset, size_t size) = 0;
        virtual int fstat(struct stat *buf) = 0;
        virtual int set_crc(uint32_t crc) { errno = ENOSYS; return -1; }
        virtual int get_crc(uint32_t* crc) { errno = ENOSYS; return -1; }
        virtual uint64_t get_handle() { return -1UL; }
        virtual int fdatasync() { errno = ENOSYS; return -1; }
        virtual int close() { return 0; }

        void release(bool detach = false)
        {
            SCOPED_LOCK(mt_);
            if ((detach || need_detach_) && !detached_) {
                detached_ = true;
                pool_->store_release(this, true);
            }

            auto ref = ref_.fetch_sub(1, std::memory_order_relaxed);
            if (ref == 1 && pool_) {
                if (!detached_) pool_->store_release(this); else delete this;
            } else if (ref == 0) delete this;  // call do_open directly
        }

        ssize_t pread(void *buf, size_t count, off_t offset)
        {
            struct iovec iov{buf, count};
            return do_preadv2(&iov, 1, offset, 0);
        }

        ssize_t pwrite(const void *buf, size_t count, off_t offset)
        {
            struct iovec iov{(void*)buf, count};
            return do_pwritev2(&iov, 1, offset, 0);
        }

        std::string_view get_src_name() { return src_name_; }
        void set_src_name(std::string_view pathname) { src_name_ = pathname.data(); }
        std::string_view get_store_key() { return store_key_; }
        void set_store_key(std::string_view pathname) { store_key_ = pathname.data(); }
        void set_pool(ICachePool* pool) { pool_ = pool; }
        void set_cached_size(off_t cached_size, int flags = 0);
        off_t get_actual_size() { return actual_size_; }
        void set_actual_size(off_t actual_size) { actual_size_ = actual_size; }
        int get_open_flags() { return open_flags_; }
        void set_open_flags(int open_flags) { open_flags_ = open_flags; }
        int get_src_file(photon::fs::IFile** src_file, int flags = O_RDONLY) {
            int ret = 0;
            if ((flags&O_ACCMODE) == O_RDONLY) {
                ret = open_src_file(&src_file_, flags);
                if (ret == 0 && src_file) *src_file = src_file_;
            } else {
                ret = open_src_file(&src_rwfile_, (flags & (~O_ACCMODE)) | O_RDWR);
                if (ret == 0 && src_file) *src_file = src_rwfile_;
            }
            return ret;
        }

        void set_src_file(photon::fs::IFile* src_file) { src_file_ = src_file; }
        photon::fs::IFileSystem* get_src_fs() { return src_fs_; }
        void set_src_fs(photon::fs::IFileSystem* src_fs) { src_fs_ = src_fs; }
        size_t get_page_size() { return page_size_; }
        void set_page_size(size_t page_size) { page_size_ = page_size; }
        IOAlloc* get_allocator() { return allocator_; }
        void set_allocator(IOAlloc* allocator) { allocator_ = allocator; }

        struct try_preadv_result
        {
            size_t iov_sum;             // sum of the iovec[]
            size_t refill_size;         // size in bytes to refill, 0 means cache hit
            union
            {
                off_t refill_offset;    // the offset to fill, if not hit
                ssize_t size;           // the return value of preadv(), if hit
            };
        };
        virtual try_preadv_result try_preadv2(const struct iovec* iov, int iovcnt, off_t offset, int flags);
        virtual ssize_t do_preadv2(const struct iovec* iov, int iovcnt, off_t offset, int flags);
        virtual ssize_t do_preadv2_mutable(struct iovec* iov, int iovcnt, off_t offset, int flags);
        virtual ssize_t do_pwritev2(const struct iovec* iov, int iovcnt, off_t offset, int flags);
        virtual ssize_t do_pwritev2_mutable(struct iovec* iov, int iovcnt, off_t offset, int flags);

    private:
        ssize_t pwritev2_extend(const struct iovec *iov, int iovcnt, off_t offset, int flags);
        ssize_t try_refill_range(off_t offset, size_t count);
        ssize_t do_refill_range(uint64_t refill_off, uint64_t refill_size, size_t count, off_t actual_size,
            IOVector* input = nullptr, off_t offset = 0, int flags = 0);
        static void* async_refill(void* args);

    protected:
        int open_src_file(photon::fs::IFile** src_file, int flags = O_RDONLY);
        int tryget_size();
        ssize_t do_prefetch(size_t count, off_t offset, int flags, uint64_t batch_size = 32 * 1024 * 1024UL);

        std::string src_name_;
        std::string store_key_;
        ICachePool* pool_ = nullptr;
        off_t cached_size_ = 0;
        off_t actual_size_ = 0;
        int open_flags_ = 0;
        std::atomic<uint32_t> ref_{0};
        photon::fs::IFile* src_file_ = nullptr;
        photon::fs::IFile* src_rwfile_ = nullptr;
        photon::fs::IFile* recycle_file_ = nullptr;
        photon::fs::IFileSystem* src_fs_ = nullptr;
        size_t page_size_ = 4096;
        IOAlloc* allocator_ = nullptr;
        RangeLock range_lock_;
        photon::mutex open_lock_;
        photon::spinlock mt_;
        bool truncated_ = false;
        bool recycled_ = false;
        bool detached_ = false;
        bool need_detach_ = false;
        friend class ICachePool;
    };

    class IMemCacheStore : public ICacheStore
    {
    public:
        virtual ssize_t pin_buffer(off_t offset, size_t count, int flags, /*OUT*/ iovector* iov, void** pin_result) = 0;

        virtual int unpin_buffer(void* pin_result) = 0;

        virtual int pin_wbuf(off_t offset, size_t count, /*OUT*/ iovector* iov, void** pin_wresult) = 0;

        virtual ssize_t unpin_wbuf(void* pin_wresult, int wret, int flags) = 0;
    };

    class IMemCachePool : public ICachePool
    {
    public:
        using ICachePool::ICachePool;

        virtual int get_rwbuf_addr(void** base, size_t* size) = 0;

        virtual ssize_t pin_buffer(uint64_t handle, off_t offset, size_t count, int flags, /*OUT*/ iovector* iov, void** pin_result) = 0;

        virtual int unpin_buffer(void* pin_result) = 0;

        virtual int pin_wbuf(off_t offset, size_t count, /*OUT*/ iovector* iov, void** pin_wresult) = 0;

        virtual ssize_t unpin_wbuf(uint64_t handle, void* pin_wresult, int wret, int flags) = 0;
    };
}
}
