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

#include "cache_pool.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <sys/statvfs.h>

#include "cache_store.h"
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/enumerable.h>
#include <photon/common/utility.h>
#include <photon/fs/path.h>

namespace photon {
namespace fs {

const uint64_t kGB = 1024 * 1024 * 1024;
const uint64_t kMaxFreeSpace = 50 * kGB;
const int64_t kEvictionMark = 5ll * kGB;

FileCachePool::FileCachePool(IFileSystem* mediaFs, uint64_t capacityInGB,
    uint64_t periodInUs, uint64_t diskAvailInBytes, uint64_t refillUnit)
    : ICachePool(0),
      mediaFs_(mediaFs),
      capacityInGB_(capacityInGB),
      periodInUs_(periodInUs),
      diskAvailInBytes_(diskAvailInBytes),
      refillUnit_(refillUnit),
      totalUsed_(0),
      timer_(nullptr),
      running_(false),
      exit_(false),
      isFull_(false) {
    int64_t capacityInBytes = capacityInGB_ * kGB;
    waterMark_ = calcWaterMark(capacityInBytes, kMaxFreeSpace);
    // keep this relation : waterMark < riskMark < capacity
    riskMark_ = std::max(capacityInBytes - kEvictionMark, (static_cast<int64_t>(waterMark_) + capacityInBytes) >> 1);
}

FileCachePool::~FileCachePool() {
  exit_ = true;
  if (timer_) {
    while (running_) {
      photon::thread_usleep(1);
    }
    delete timer_;
  }
  this->stores_clear();
  delete mediaFs_;
}

void FileCachePool::Init() {
  traverseDir("/");
  timer_ = new photon::Timer(periodInUs_, {this, FileCachePool::timerHandler}, true, 8UL * 1024 * 1024);
}

ICacheStore* FileCachePool::do_open(std::string_view pathname, int flags, mode_t mode) {
  auto localFile = openMedia(pathname, flags, mode);
  if (!localFile) {
    return nullptr;
  }

  auto find = fileIndex_.find(pathname);
  if (find == fileIndex_.end()) {
    auto lruIter = lru_.push_front(fileIndex_.end());
    std::unique_ptr<LruEntry> entry(new LruEntry{lruIter, 1, 0});
    find = fileIndex_.emplace(pathname, std::move(entry)).first;
    lru_.front() = find;
  } else {
    lru_.access(find->second->lruIter);
    find->second->openCount++;
  }

  return new FileCacheStore(this, localFile, refillUnit_, find);
}

IFile* FileCachePool::openMedia(std::string_view name, int flags, int mode) {
   if (name.empty() || name[0] != '/') {
    LOG_ERROR_RETURN(EINVAL, nullptr, "pathname is invalid, path : `", name);
  }

  auto base_directory = Path(name.data()).dirname();
  auto ret = mkdir_recursive(base_directory, mediaFs_);
  if (ret) {
    LOG_ERRNO_RETURN(0, nullptr, "mkdir failed, path : `", name);
  }

  auto localFile = mediaFs_->open(name.data(), flags, mode);
  if (nullptr == localFile) {
    LOG_ERRNO_RETURN(0, nullptr,
        "cache store open failed, pathname : `, flags : `, mode : `", name, flags, mode);
  }
  return localFile;
}

int FileCachePool::set_quota(std::string_view pathname, size_t quota) {
  errno = ENOSYS;
  return -1;
}

int FileCachePool::stat(CacheStat* stat, std::string_view pathname) {
  errno = ENOSYS;
  return -1;
}

int FileCachePool::evict(std::string_view filename) {
  errno = ENOSYS;
  return -1;
}

int FileCachePool::evict(size_t size) {
  errno = ENOSYS;
  return -1;
}

int FileCachePool::rename(std::string_view oldname, std::string_view newname) {
  errno = ENOSYS;
  return -1;
}

bool FileCachePool::isFull() {
  return isFull_;
}

void FileCachePool::removeOpenFile(FileNameMap::iterator iter) {
  iter->second->openCount--;
}

void FileCachePool::forceRecycle() {
  timerHandler(this);
}

void FileCachePool::updateLru(FileNameMap::iterator iter) {
  lru_.access(iter->second->lruIter);
}

//  currently, we exist duplicate pwrite
uint64_t FileCachePool::updateSpace(FileNameMap::iterator iter, uint64_t size) {
  auto lruEntry = iter->second.get();
  uint64_t diff = 0;
  if (size > lruEntry->size) {
    diff = size - lruEntry->size;
    totalUsed_ += diff;
  }
  lruEntry->size = size;
  if (totalUsed_ >= riskMark_) {
    LOG_WARN("pwrite is so heavy, totalUsed:`,riskMark:` || lruEntry->size = `",totalUsed_, riskMark_, lruEntry->size);
    isFull_ = true;
    forceRecycle();
    if (lruEntry->size==0) diff = 0;//in some extream condition ,
                                    //forceRecycle maybe truncate current file to 0
  }
  return diff;
}

uint64_t FileCachePool::timerHandler(void* data) {
  auto cur = static_cast<FileCachePool*>(data);
  if (cur->running_) {
    return 0;
  }
  cur->running_ = true;
  DEFER(cur->running_ = false;);
  cur->eviction();
  return 0;
}

void FileCachePool::eviction() {
  uint64_t evictByDisk = 0;
  uint64_t evictByCache = 0;
  uint64_t fsCapacity = 0;

  DEFER(isFull_ = false);
  struct statvfs stFs = {};
  auto err = mediaFs_->statvfs("/", &stFs);
  if (err) {
    LOG_ERROR("statvfs failed, ret : `, error code : `", err, ERRNO());
    return;
  } else {
    fsCapacity = stFs.f_frsize * stFs.f_blocks;
    uint64_t diskAvailInBytes = stFs.f_bavail * stFs.f_frsize;
    if (diskAvailInBytes < diskAvailInBytes_) {
      evictByDisk = diskAvailInBytes_ - diskAvailInBytes;
    } else if (fsCapacity <= waterMark_) { // we occupy the whole disk
      return;
    }
  }

  if (totalUsed_ >= static_cast<int64_t>(waterMark_)) {
    evictByCache = totalUsed_ - waterMark_;
  }

  auto actualEvict = static_cast<int64_t>(std::max(evictByCache, evictByDisk));
  if (actualEvict <= 0) {
    return;
  }

  isFull_ = true;

  while (actualEvict > 0 && !lru_.empty() && !exit_) {
    auto fileIter = lru_.back();
    const auto& fileName = fileIter->first;
    auto lruEntry = fileIter->second.get();
    auto fileSize = lruEntry->size;
    if (lruEntry->openCount == 0){
      lru_.mark_key_cleared(fileIter->second->lruIter);
    } else {
      lru_.access(fileIter->second->lruIter);
    }
    //as soon as possible truncate and unlink
    if (0 == fileSize) {
        if (0 == fileIter->second->openCount) {
            afterFtrucate(fileIter);
        }
        continue;
    }

    {
      photon::scoped_rwlock rl(lruEntry->rw_lock_, photon::WLOCK);
      err = mediaFs_->truncate(fileName.data(), 0);
      lruEntry->truncate_done = false;
    }

    if (err) {
      ERRNO e;
      LOG_ERROR("truncate(0) failed, name : `, ret : `, error code : `", fileName, err, e);
      // truncate to 0 failed means unable to free the file, it should not consider as a part
      // of cache. Deal as it already release.
      // The only exception is errno EINTR, means truncate interrupted by signal, should try
      // again
      if (e.no == EINTR) {
        continue;
      }
    }
    afterFtrucate(fileIter);
    actualEvict -= fileSize;
    photon::thread_yield();
  }
}

uint64_t FileCachePool::calcWaterMark(uint64_t capacity, uint64_t maxFreeSpace) {
  return std::max(static_cast<uint64_t>(capacity * kWaterMarkRatio * 0.01),
    capacity > maxFreeSpace ? capacity - maxFreeSpace : 0);
}

bool FileCachePool::afterFtrucate(FileNameMap::iterator iter) {
  auto lruEntry = iter->second.get();
  totalUsed_ -= static_cast<int64_t>(lruEntry->size);
  lruEntry->size = 0;
  if (totalUsed_ < 0) {
    totalUsed_ = 0;
  }
  if (0 == iter->second->openCount) {
    auto err = mediaFs_->unlink(iter->first.data());
    ERRNO e;
    LOG_ERROR("unlink failed, name : `, ret : `, error code : `", iter->first, err, e);
    // unlik failed may caused by multiple reasons
    // only EBUSY should may be able to trying to unlink again
    // other reason should never try to clean it.
    if (err && (e.no == EBUSY)) {
      return false;
    }
    lru_.remove(iter->second->lruIter);
    fileIndex_.erase(iter);
  }
  return true;
}

int FileCachePool::traverseDir(const std::string& root) {
  for (auto file : enumerable(Walker(mediaFs_, root))) {
    insertFile(file);
  }
  return 0;
}

int FileCachePool::insertFile(std::string_view file) {
  struct stat st = {};
  auto ret = mediaFs_->stat(file.data(), &st);
  if (ret) {
    LOG_ERRNO_RETURN(0, -1, "stat failed, name : `", file.data());
  }
  auto fileSize = st.st_blocks * kDiskBlockSize;

  auto lruIter = lru_.push_front(fileIndex_.end());
  auto entry = std::unique_ptr<LruEntry>(new LruEntry{lruIter, 0, fileSize});
  auto iter = fileIndex_.emplace(file, std::move(entry)).first;
  lru_.front() = iter;
  totalUsed_ += fileSize;
  return 0;
}

}
}