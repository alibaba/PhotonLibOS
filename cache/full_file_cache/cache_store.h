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

#include <stddef.h>
#include <string>
#include <photon/common/range-lock.h>
#include "cache_pool.h"

namespace photon {
namespace fs {

class FileCachePool;

class FileCacheStore : public photon::fs::ICacheStore {
public:
    typedef FileCachePool::FileNameMap::iterator FileIterator;
    FileCacheStore(photon::fs::ICachePool *cachePool, photon::fs::IFile *localFile,
                   size_t refillUnit, FileIterator iterator);
    ~FileCacheStore();

    try_preadv_result try_preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override;

    ssize_t do_preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override;

    ssize_t do_pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override;

    int set_quota(size_t quota) override;
    int stat(photon::fs::CacheStat *stat) override;
    int evict(off_t offset, size_t count = -1, int flags = 0) override;

    std::pair<off_t, size_t> queryRefillRange(off_t offset, size_t size) override;

    int fstat(struct stat *buf) override;

protected:
    bool cacheIsFull();

    struct ReadRequest {
        off_t offset;
        size_t size;
    };

    //  merge from first extent to last extent(or encounter hole),
    //  because fiemap could return multiple continuous extents even though no any hole.
    std::pair<off_t, size_t> getFirstMergedExtents(struct fiemap *fie);

    std::pair<off_t, size_t> getLastMergedExtents(struct fiemap *fie);

    std::pair<off_t, off_t> getHoleFromCacheHitResult(off_t offset, size_t alignSize,
                                                      struct fiemap *fie);

    FileCachePool *cachePool_;     //  owned by extern class
    photon::fs::IFile *localFile_; //  owned by current class
    size_t refillUnit_;
    FileIterator iterator_;
    RangeLock rangeLock_;

    ssize_t do_pwritev(const struct iovec *iov, int iovcnt, off_t offset);
};

}
}
