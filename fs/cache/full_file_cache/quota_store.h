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

#include "cache_store.h"

namespace photon {
namespace fs {

class QuotaFileStore : public FileCacheStore {
  public:
    typedef FileCachePool::FileNameMap::iterator FileIterator;
    QuotaFileStore(photon::fs::ICachePool* cachePool, photon::fs::IFile* localFile, uint64_t refillUnit,
      FileIterator iterator);

    ssize_t do_preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override;

    ssize_t do_pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override;

    int set_quota(size_t quota) override;

    int stat(photon::fs::CacheStat* stat) override;
};

}
}
