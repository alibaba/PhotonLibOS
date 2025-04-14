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

#include "cache.h"
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/io-alloc.h>
#include <photon/common/iovector.h>
#include <photon/common/expirecontainer.h>
#include <photon/thread/thread-pool.h>

#include "full_file_cache/cache_pool.h"

namespace photon {
namespace fs{

    ICachedFileSystem *new_full_file_cached_fs(IFileSystem *srcFs, IFileSystem *mediaFs,
                                           uint64_t refillUnit, uint64_t capacityInGB,
                                           uint64_t periodInUs, uint64_t diskAvailInBytes,
                                           IOAlloc *allocator, int quotaDirLevel,
                                           CacheFnTransFunc fn_trans_func) {
    if (refillUnit % 4096 != 0 || !is_power_of_2(refillUnit)) {
        LOG_ERROR_RETURN(EINVAL, nullptr, "refill Unit need to be aligned to 4KB and power of 2")
    }
    if (!allocator) {
        allocator = new IOAlloc;
    }
    FileCachePool *pool = nullptr;
    pool =
        new FileCachePool(mediaFs, capacityInGB, periodInUs, diskAvailInBytes, refillUnit);
    pool->Init();
    return new_cached_fs(srcFs, pool, 4096, allocator, fn_trans_func);
}

using OC = ObjectCache<std::string, ICacheStore*>;
ICachePool::ICachePool(uint32_t pool_size, uint32_t max_refilling, uint32_t refilling_threshold, bool pin_write)
    : m_stores(new OC(10UL * 1000 * 1000)),
        m_max_refilling(max_refilling),
        m_refilling_threshold(refilling_threshold),
        m_pin_write(pin_write) {
    if (pool_size != 0) {
        m_thread_pool = photon::new_thread_pool(pool_size, 128 * 1024UL);
        m_vcpu = photon::get_vcpu();
    };
}

#define cast(x) static_cast<OC*>(x)
ICachePool::~ICachePool() {
    stores_clear();
    delete cast(m_stores);
}

void ICachePool::stores_clear()
{
    if (m_thread_pool) {
        auto pool = static_cast<photon::ThreadPoolBase*>(m_thread_pool);
        m_thread_pool = nullptr;
        photon::delete_thread_pool(pool);
    }
    cast(m_stores)->clear();
}

ICacheStore* ICachePool::open(std::string_view filename, int flags, mode_t mode){
    char store_name[4096];
    auto len = this->fn_trans_func(filename, store_name, sizeof(store_name));
    std::string_view store_sv = len ? std::string_view(store_name, len) : filename;
    auto ctor = [&]() -> ICacheStore* {
        auto cache_store = this->do_open(store_sv, flags, mode);
        if (nullptr == cache_store) {
            LOG_ERRNO_RETURN(0, nullptr, "fileCachePool_ open file failed, name : `", filename.data());
        }
        cache_store->set_store_key(store_sv);
        cache_store->set_src_name(filename);
        cache_store->set_pool(this);
        struct stat st;
        SET_STRUCT_STAT(&st);
        st.st_size = -1;
        if (cache_store->fstat(&st) == 0) {
            cache_store->set_cached_size(st.st_size);
            cache_store->set_actual_size(st.st_size);
        }
        cache_store->set_open_flags(flags);
        return cache_store;
    };
    auto store = cast(m_stores)->acquire(store_sv, ctor);
    if (store) {
        auto cnt = store->ref_.fetch_add(1, std::memory_order_relaxed);
        if (cnt) cast(m_stores)->release(store_sv);
    }
    return store;
}

void ICachePool::set_trans_func(CacheFnTransFunc fn_trans_func) {
    this->fn_trans_func = fn_trans_func;
}

int ICachePool::store_release(ICacheStore* store, bool detach) {
    cast(m_stores)->release(store->get_store_key(), detach, !detach);
    return 0;
}
}
} // namespace photon::fs
