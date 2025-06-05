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
#include <cinttypes>
#include <sys/uio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/cache/pool_store.h>

#define O_WRITE_THROUGH 0x01000000 // write backing store and cache
#define O_WRITE_AROUND 0x02000000  // write backing store only, default
#define O_WRITE_BACK 0x04000000    // write cache and async flush to backing store, not support yet
#define O_CACHE_ONLY 0x08000000    // write cache only
#define O_DIRECT_LOCAL 0x20000000  // read local
#define O_MMAP_READ 0x00800000     // mmap like read

#define RW_V2_HIGH_PRIORITY 0x00000001 // preadv2/pwritev2 high priority cache data
#define RW_V2_PROMOTE 0x00000002       // preadv2 promote flag
#define RW_V2_CACHE_ONLY 0x00000004    // preadv2 cache only flag
#define RW_V2_TO_BUFFER_WITHOUT_SYNC                                                               \
    0x00000010                       // pwritev2 to buffered accessor file's buffer without sync
#define RW_V2_MEMORY_ONLY 0x00000020 // pwritev2 memory cache only
#define RW_V2_WRITE_BACK    0x00000040  // pwritev2 with write back
#define RW_V2_SYNC_MODE     0x00000200  // pwritev2 memory and disk with sync mode
#define RW_V2_PIN_EXTERNAL  0x00001000  // pin buffer with external handle
#define EVICT_RECYCLE   0x00000001
#define EVICT_FTRUNCATE 0x00000002

#define IS_STRUCT_STAT_SETTED(x) ((*(uint64_t *)x) == 0xF19A336DB7CA28E7ull)
#define SET_STRUCT_STAT(x) ((*(uint64_t *)x) = 0xF19A336DB7CA28E7ull)

const int IOCTL_GET_PAGE_SIZE = 161;

struct IOAlloc;
namespace photon {
namespace fs {
class ICachedFileSystem : public IFileSystem {
public:
    // get the source file system
    UNIMPLEMENTED_POINTER(IFileSystem *get_source());

    // set the source file system
    UNIMPLEMENTED(int set_source(IFileSystem *src));

    UNIMPLEMENTED_POINTER(ICachePool *get_pool());

    UNIMPLEMENTED(int set_pool(ICachePool *pool));
};

class ICachedFile : public IFile {
public:
    // get the source file system
    UNIMPLEMENTED_POINTER(IFile *get_source());

    // set the source file system, and enable `auto_refill`
    UNIMPLEMENTED(int set_source(IFile *src));

    UNIMPLEMENTED_POINTER(ICacheStore *get_store());

    // client refill for an ICachedFile (without a source!)
    // is implemented as pwrite(), usually aligned
    ssize_t refill(const void *buf, size_t count, off_t offset) {
        return pwrite(buf, count, offset);
    }
    ssize_t refill(const struct iovec *iov, int iovcnt, off_t offset) {
        return pwritev(iov, iovcnt, offset);
    }
    ssize_t refill(struct iovec *iov, int iovcnt, off_t offset) {
        return pwritev(iov, iovcnt, offset);
    }

    // refilling a range without providing data, is treated as prefetching
    ssize_t refill(off_t offset, size_t count) {
        return fadvise(offset, count, POSIX_FADV_WILLNEED);
    }

    // query cached extents is implemented as fiemap()
    UNIMPLEMENTED(int query(off_t offset, size_t count))

    // eviction is implemented as trim()
    ssize_t evict(off_t offset, size_t count) {
        return trim(offset, count);
    }

    int vioctl(int request, va_list args) override {
        auto src = get_source();
        if (src)
            return src->vioctl(request, args);
        return -1;
    }
};

extern "C" {
ICachedFileSystem *new_cached_fs(IFileSystem *src, ICachePool *pool, uint64_t pageSize,
                                 IOAlloc *allocator, CacheFnTransFunc fn_trans_func = nullptr);

ICachedFile *new_cached_file(ICacheStore *store, uint64_t pageSize, IFileSystem *fs);

ICachedFileSystem *new_full_file_cached_fs(IFileSystem *srcFs,
                                           IFileSystem *media_fs, uint64_t refillUnit,
                                           uint64_t capacityInGB, uint64_t periodInUs,
                                           uint64_t diskAvailInBytes, IOAlloc *allocator,
                                           int quotaDirLevel,
                                           CacheFnTransFunc fn_trans_func = nullptr);

/**
 * @param blk_size The proper size for cache metadata and IO efficiency. Large writes to cache media
 *                 will be split into blk_size. Reads and small writes are not affected.
 * @param prefetch_unit Controls the expand prefetch size from src file. 0 means to disable this
 * feature.
 */
photon::fs::IFileSystem *new_ocf_cached_fs(IFileSystem *src_fs,
                                           IFileSystem *namespace_fs, size_t blk_size,
                                           size_t prefetch_unit, IFile *media_file,
                                           bool reload_media, IOAlloc *io_alloc);

/*
* `persistent_cache` is designed to store data into the `src_fs`.
* It just downloads the chunk data to the `src_fs` and never evicts them.
*/
photon::fs::IFileSystem *new_persistent_cached_fs(IFileSystem *src_fs, size_t blk_size,
                                                size_t refill_size, IOAlloc *io_alloc);
} // extern "C"
}
} // namespace photon::fs
