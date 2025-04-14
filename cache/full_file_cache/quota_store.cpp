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

#include "quota_store.h"

#include <sys/stat.h>
#include <sys/uio.h>

#include <photon/common/alog.h>
#include <photon/fs/filesystem.h>

#include "quota_pool.h"



namespace photon {
namespace fs {

const uint64_t kDiskBlockSize = 512; // stat(2)

QuotaFileStore::QuotaFileStore(photon::fs::ICachePool* cachePool, IFile* localFile,
  uint64_t refillUnit, FileIterator iterator)
 : FileCacheStore(cachePool, localFile, refillUnit, iterator) {
}

ssize_t QuotaFileStore::do_preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) {

  auto dirPool = static_cast<QuotaFilePool*>(cachePool_);
  dirPool->updateDirLru(iterator_);
  return FileCacheStore::do_preadv2(iov, iovcnt, offset, flags);
}

ssize_t QuotaFileStore::do_pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  auto dirPool = static_cast<QuotaFilePool*>(cachePool_);
  if (cacheIsFull()) {
    errno = ENOSPC;
    return -1;
  }

  if (dirPool->dirSpaceIsFull(iterator_)) {
    errno = ENOSPC;
    return -1;
  }
  ssize_t ret;
  {
    auto lruEntry = static_cast<FileCachePool::LruEntry*>(iterator_->second.get());
    photon::scoped_rwlock wl(lruEntry->rw_lock_, photon::WLOCK);
    ret = localFile_->pwritev(iov, iovcnt, offset);
  }
  if (ret < 0 && ENOSPC == errno) {
    cachePool_->forceRecycle();
  }

  if (ret > 0) {
    struct stat st = {};
    auto err = localFile_->fstat(&st);
    if (err) {
      LOG_ERRNO_RETURN(0, ret, "fstat failed")
    }
    dirPool->updateDirLru(iterator_);
    cachePool_->updateLru(iterator_);
    auto diff = dirPool->updateSpace(iterator_, QuotaFilePool::kDiskBlockSize * st.st_blocks);
    dirPool->updateDirSpace(iterator_, diff);
  }
  return ret;
}

int QuotaFileStore::set_quota(size_t quota) {
  auto dirPool = static_cast<QuotaFilePool*>(cachePool_);
  dirPool->updateDirQuota(iterator_, quota);
  return 0;
}

int QuotaFileStore::stat(CacheStat* stat) {
  struct stat st = {};
  auto ret = localFile_->fstat(&st);
  if (ret) {
    LOG_ERRNO_RETURN(0, -1, "stat failed, ret:`,name:`", ret, iterator_->first.data())
  }
  stat->used_size = st.st_blocks * kDiskBlockSize / refillUnit_;
  stat->total_size = st.st_size / refillUnit_;
  stat->refill_unit = refillUnit_;
  return 0;
}

}
}
