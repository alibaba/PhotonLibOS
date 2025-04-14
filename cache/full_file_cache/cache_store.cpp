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

#include "cache_store.h"

#include <sys/stat.h>
#include "sys/statvfs.h"
#include <sys/uio.h>

#include <photon/common/alog.h>
#include <photon/common/alog-audit.h>
#include <photon/fs/fiemap.h>
#include <photon/fs/filesystem.h>
#include <photon/common/iovector.h>
#include "cache_pool.h"



namespace photon {
namespace fs {

const uint64_t kDiskBlockSize = 512; // stat(2)
constexpr int kFieExtentSize = 1000;
const int kBlockSize = 4 * 1024;

FileCacheStore::FileCacheStore(photon::fs::ICachePool* cachePool, IFile* localFile,
  size_t refillUnit, FileIterator iterator)
    : cachePool_(static_cast<FileCachePool*>(cachePool)),
      localFile_(localFile),
      refillUnit_(refillUnit),
      iterator_(iterator) {
}

FileCacheStore::~FileCacheStore() {
  delete localFile_;  //  will close file
  cachePool_->removeOpenFile(iterator_);
}

ICacheStore::try_preadv_result FileCacheStore::try_preadv2(const struct iovec *iov, int iovcnt,
                                                           off_t offset, int flags) {
  auto lruEntry = static_cast<FileCachePool::LruEntry *>(iterator_->second.get());
  photon::scoped_rwlock rl(lruEntry->rw_lock_, photon::RLOCK);
  return this->ICacheStore::try_preadv2(iov, iovcnt, offset, flags);
}

ssize_t FileCacheStore::do_preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  // TODO(suoshi.yf): maybe a new interface for updating lru is better for avoiding
  // multiple cacheStore preadvs but cacheFile preadv only once
  ssize_t ret = 0;
  cachePool_->updateLru(iterator_);
  SCOPE_AUDIT_THRESHOLD(1UL * 1000, "file:read", AU_FILEOP("", offset, ret));
  ret = localFile_->preadv(iov, iovcnt, offset);
  return ret;
}

ssize_t FileCacheStore::do_pwritev(const struct iovec *iov, int iovcnt, off_t offset)
{
  ssize_t ret;
  iovector_view view((iovec*)iov, iovcnt);
  auto lruEntry = static_cast<FileCachePool::LruEntry *>(iterator_->second.get());
  photon::scoped_rwlock rl(lruEntry->rw_lock_, photon::RLOCK);
  if (!lruEntry->truncate_done) {
    // May repeated ftruncate() here, but it doesn't matter
    ret = localFile_->ftruncate(actual_size_);
    if (ret) {
      LOG_ERRNO_RETURN(0, -1, "failed to truncate media file: ", VALUE(ret));
    }
    lruEntry->truncate_done = true;
  }
  ScopedRangeLock lock(rangeLock_, offset, view.sum());
  SCOPE_AUDIT_THRESHOLD(10UL * 1000, "file:write", AU_FILEOP("", offset, ret));
  ret = localFile_->pwritev(iov, iovcnt, offset);
  return ret;
}

ssize_t FileCacheStore::do_pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  if (cacheIsFull()) {
    errno = ENOSPC;
    return -1;
  }

  auto ret = do_pwritev(iov, iovcnt, offset);
  if (ret < 0 && ENOSPC == errno) {
    cachePool_->forceRecycle();
  }

  if (ret > 0) {
    struct stat st = {};
    auto err = localFile_->fstat(&st);
    if (err) {
      LOG_ERRNO_RETURN(0, ret, "fstat failed")
    }
    cachePool_->updateLru(iterator_);
    cachePool_->updateSpace(iterator_, kDiskBlockSize * st.st_blocks);
  }
  return ret;
}

std::pair<off_t, size_t> FileCacheStore::queryRefillRange(off_t offset, size_t size) {
  ScopedRangeLock lock(rangeLock_, offset, size);
  off_t alignLeft = align_down(offset, kBlockSize);
  off_t alignRight = align_up(offset + size, kBlockSize);
  ReadRequest request{alignLeft, static_cast<size_t>(alignRight - alignLeft)};
  struct fiemap_t<kFieExtentSize> fie(request.offset, request.size);
  fie.fm_mapped_extents = 0;

  if (request.size > 0) { //  fiemap cannot handle size zero.
    auto ok = localFile_->fiemap(&fie);
    if (ok != 0) {
      LOG_ERRNO_RETURN(0, std::make_pair(-1, 0),
                       "media fiemap failed : `, offset : `, size : `",
                       ok, request.offset, request.size);
    }
    if (fie.fm_mapped_extents >= kFieExtentSize) { //TODO: qisheng.ds
                                                   //it's supposed to be solved by
                                                   //get fie extents twice,
                                                   //but just do not concern much
                                                   //about it rightnow.
      LOG_ERROR_RETURN(EINVAL, std::make_pair(-1, 0),
                       "read size is too big : `", request.size);
    }
  }

  uint64_t holeStart = request.offset;
  uint64_t holeEnd = request.offset + request.size;

  for (ssize_t i = (ssize_t)(fie.fm_mapped_extents) - 1; i >= 0; i--) {
    auto& extent = fie.fm_extents[i];
    if ((extent.fe_flags == FIEMAP_EXTENT_UNKNOWN) || (extent.fe_flags == FIEMAP_EXTENT_UNWRITTEN)) continue;
      if (extent.fe_logical < holeEnd){
        if (extent.fe_logical_end() >= holeEnd){
          holeEnd = extent.fe_logical;
        } else break;
      }
  }

  for (uint32_t i = 0; i < fie.fm_mapped_extents; i++) {
    auto& extent = fie.fm_extents[i];
    if ((extent.fe_flags == FIEMAP_EXTENT_UNKNOWN) || (extent.fe_flags == FIEMAP_EXTENT_UNWRITTEN)) continue;
      if (extent.fe_logical_end() > holeStart){
        if (extent.fe_logical <= holeStart){
          holeStart = extent.fe_logical_end();
        } else break;
      }
  }

  if (holeStart >= holeEnd) return std::make_pair(0, 0);
  // CacheMiss
  auto left = align_down(holeStart, refillUnit_);
  auto right = align_up(holeEnd, refillUnit_);
  return std::make_pair(left, right - left);
}

int FileCacheStore::set_quota(size_t quota) {
  errno = ENOSYS;
  return -1;
}

int FileCacheStore::stat(CacheStat* stat) {
  errno = ENOSYS;
  return -1;
}

int FileCacheStore::evict(off_t offset, size_t count, int flags) {
  if (static_cast<size_t>(-1) == count) {
    return localFile_->ftruncate(offset);
  } else {
    #ifndef FALLOC_FL_KEEP_SIZE
    #define FALLOC_FL_KEEP_SIZE     0x01 /* default is extend size */
    #endif
    #ifndef FALLOC_FL_PUNCH_HOLE
    #define FALLOC_FL_PUNCH_HOLE	0x02 /* de-allocates range */
    #endif
    int mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
    return localFile_->fallocate(mode, offset, count);
  }
}

int FileCacheStore::fstat(struct stat *buf) {
  return localFile_->fstat(buf);
}

bool FileCacheStore::cacheIsFull() {
  return cachePool_->isFull();
}

}
}
