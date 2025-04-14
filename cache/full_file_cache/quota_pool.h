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

#include "cache_pool.h"

namespace photon {
namespace fs {

class QuotaFilePool : public FileCachePool {
  public:
    typedef FileCachePool::FileNameMap::iterator FileIterator;
    QuotaFilePool(photon::fs::IFileSystem* mediaFs, uint64_t capacityInGB, uint64_t periodInUs,
      uint64_t diskAvailInBytes, uint64_t refillUnit, int quotaDirLevel);

    photon::fs::ICacheStore* do_open(std::string_view pathname, int flags, mode_t mode) override;

    void updateDirLru(FileIterator iter);

    bool dirSpaceIsFull(FileIterator iter);
    void updateDirSpace(FileIterator iter, uint64_t size);
    void updateDirQuota(FileIterator iter, size_t quota);

    int set_quota(std::string_view pathname, size_t quota) override;
    int stat(photon::fs::CacheStat* stat,
            std::string_view pathname = std::string_view(nullptr, 0)) override;

    int evict(std::string_view filename) override;

protected:
    void dirEviction();

    bool afterFtrucate(FileIterator iter) override;
    void eviction() override;
    int insertFile(std::string_view file) override;

  private:
    struct DirInfo {
      int64_t used = 0;
      int64_t quota = 0;
      int64_t waterMark = 0;
      int64_t riskMark = 0;
      LRUContainer lru;
      bool inEvicting = false;
      int fileCount = 0;
      void calcMark(QuotaFilePool* pool);
    };
    typedef map_string_key<DirInfo> DirMap;
    typedef DirMap::iterator DirIter;
    DirMap dirInfos_;
    int quotaDirLevel_;

    // TODO(suoshi.yf): maybe duplicate //
    const char* getQuotaCtrlPos(const char* pathname);

    std::pair<FileIterator, DirIter> insertNewFile(DirIter dir, std::string&& dirName,
      std::string_view file);

    struct QuotaLruEntry : public LruEntry {
      QuotaLruEntry(uint32_t lruIt, int openCnt, uint32_t quotaLruIt, uint64_t fileSize,
        DirMap::iterator it)
        : LruEntry(lruIt, openCnt, fileSize), QuotaLruIter(quotaLruIt),
          dir(it){
      }
      uint32_t QuotaLruIter;
      DirMap::iterator dir;
    };
};

}
}
