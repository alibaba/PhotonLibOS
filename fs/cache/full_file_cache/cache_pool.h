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

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <photon/thread/thread.h>
#include <photon/thread/timer.h>
#include <photon/common/string-keyed.h>
#include "../policy/lru.h"
#include <photon/fs/cache/pool_store.h>

#include <photon/fs/filesystem.h>

namespace photon {
namespace fs {

class FileCachePool : public photon::fs::ICachePool {
public:
    FileCachePool(photon::fs::IFileSystem *mediaFs, uint64_t capacityInGB, uint64_t periodInUs,
                  uint64_t diskAvailInBytes, uint64_t refillUnit);
    ~FileCachePool();

    static const uint64_t kDiskBlockSize = 512; // stat(2)
    static const uint64_t kDeleteDelayInUs = 1000;
    static const uint32_t kWaterMarkRatio = 90;

    void Init();

    //  pathname must begin with '/'
    photon::fs::ICacheStore *do_open(std::string_view pathname, int flags, mode_t mode) override;

    int set_quota(std::string_view pathname, size_t quota) override;
    int stat(photon::fs::CacheStat *stat,
             std::string_view pathname = std::string_view(nullptr, 0)) override;

    int evict(std::string_view filename) override;
    int evict(size_t size = 0) override;
    int rename(std::string_view oldname, std::string_view newname) override;

    struct LruEntry {
        LruEntry(uint32_t lruIt, int openCnt, uint64_t fileSize)
            : lruIter(lruIt), openCount(openCnt), size(fileSize), truncate_done(false) {
        }
        ~LruEntry() = default;
        uint32_t lruIter;
        int openCount;
        uint64_t size;
        photon::rwlock rw_lock_;
        bool truncate_done;
    };

    // Normally, fileIndex(std::map) always keep growing, so its iterators always
    // keep valid, iterator will be erased when file be unlinked in period of eviction,
    // but on that time corresponding CachedFile had been destructed, so nobody hold
    // erased iterator.
    typedef map_string_key<std::unique_ptr<LruEntry>> FileNameMap;

    bool isFull();
    void removeOpenFile(FileNameMap::iterator iter);
    void forceRecycle();
    void updateLru(FileNameMap::iterator iter);
    uint64_t updateSpace(FileNameMap::iterator iter, uint64_t size);

protected:
    photon::fs::IFile *openMedia(std::string_view name, int flags, int mode);

    static uint64_t timerHandler(void *data);
    virtual void eviction();
    uint64_t calcWaterMark(uint64_t capacity, uint64_t maxFreeSpace);

    photon::fs::IFileSystem *mediaFs_; //  owned by current class
    uint64_t capacityInGB_;
    uint64_t periodInUs_;
    uint64_t diskAvailInBytes_;
    size_t refillUnit_;
    int64_t totalUsed_;
    int64_t riskMark_;
    uint64_t waterMark_;

    photon::Timer *timer_;
    bool running_;
    bool exit_;

    bool isFull_;

    virtual bool afterFtrucate(FileNameMap::iterator iter);

    int traverseDir(const std::string &root);
    virtual int insertFile(std::string_view file);

    typedef photon::fs::LRU<FileNameMap::iterator, uint32_t> LRUContainer;
    LRUContainer lru_;
    // filename -> lruEntry
    FileNameMap fileIndex_;
};

}
}