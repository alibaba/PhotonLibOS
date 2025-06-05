/*
   Copyright The Overlaybd Authors

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
#include <string>
#include <photon/common/alog.h>
#include <photon/common/alog-audit.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/iovector.h>
#include <photon/fs/range-split.h>
#include <photon/fs/localfs.h>
#include <photon/fs/virtual-file.h>
#include <photon/fs/forwardfs.h>
#include <photon/fs/fiemap.h>
#include <photon/thread/thread.h>
#include <photon/common/io-alloc.h>
#include <photon/common/expirecontainer.h>
#include <photon/common/range-lock.h>
#include <photon/fs/cache/cache.h>

#define SET_LOCAL_DIR 118
#define SET_SIZE 119

namespace photon {
namespace fs {

class PersistentCacheFs;

class PersistentCacheStore : public ForwardFile_Ownership {
public:
    PersistentCacheStore(IFile *file, PersistentCacheFs *fs)
        : ForwardFile_Ownership(file, true), m_fs(fs) {
    }

    using ForwardFile_Ownership::ftruncate;
    using ForwardFile_Ownership::preadv;
    using ForwardFile_Ownership::pwritev;

    int fallocate(int mode, off_t offset, off_t len) override {
        ScopedRangeLock lock(m_range_lock, offset, len);
        return m_file->fallocate(mode, offset, len);
    }
    int ftruncate(off_t length) override {
        // set when initialization, no need lock (length too large for range lock)
        return m_file->ftruncate(length);
    }

    std::pair<off_t, size_t> query_refill_range(off_t offset, size_t size);

    int try_lock_wait(uint64_t offset, uint64_t length) {
        return m_range_lock.try_lock_wait(offset, length);
    }
    void lock(uint64_t offset, uint64_t length) {
        m_range_lock.lock(offset, length);
    }
    void unlock(uint64_t offset, uint64_t length) {
        m_range_lock.unlock(offset, length);
    }

private:
    RangeLock m_range_lock;
    PersistentCacheFs *m_fs;
};

class PersistentCacheFile : public VirtualFile {
public:
    PersistentCacheFile(IFile *file, const char *pathname, PersistentCacheFs *fs)
        : m_file(file), m_name(pathname), m_fs(fs) {
    }
    ~PersistentCacheFile();

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override;
    int fstat(struct stat *buf) override;
    int vioctl(int request, va_list args) override;
    // used for evict
    int fallocate(int mode, off_t offset, off_t len);

    UNIMPLEMENTED_POINTER(IFileSystem *filesystem() override);
    UNIMPLEMENTED(off_t lseek(off_t offset, int whence) override);
    UNIMPLEMENTED(int fsync() override);
    UNIMPLEMENTED(int fdatasync() override);
    UNIMPLEMENTED(int fchmod(mode_t mode) override);
    UNIMPLEMENTED(int fchown(uid_t owner, gid_t group) override);
    UNIMPLEMENTED(int ftruncate(off_t length) override);
    UNIMPLEMENTED(int close() override);

private:
    std::string m_local_path;
    photon::fs::IFile *m_file = nullptr;
    PersistentCacheStore *m_local_file = nullptr;
    size_t m_size = 0;
    bool ready = false;
    std::string m_name;
    PersistentCacheFs *m_fs;
};

class PersistentCacheFs : public IFileSystem {
public:
    PersistentCacheFs(IFileSystem *fs, size_t bs, size_t rs, IOAlloc *io_alloc)
        : block_size(bs), refill_size(rs), io_alloc(io_alloc),
          m_file_pool(1 * 1000 * 1000), m_src_fs(fs){
        LOG_INFO("new PersistentCacheFs");
    }
    ~PersistentCacheFs() {
        LOG_INFO("delete PersistentCacheFs");
    }
    virtual IFile *open(const char *pathname, int flags) override {
        auto src_file = m_src_fs->open(pathname, flags);
        if (src_file == nullptr) {
            LOG_ERRNO_RETURN(0, nullptr, "failed to open src: `", pathname);
        }
        return new PersistentCacheFile(src_file, pathname, this);
    }
    IFile *open(const char *pathname, int flags, mode_t mode) {
        return open(pathname, flags, 0644);
    }

    UNIMPLEMENTED(int stat(const char *pathname, struct stat *buf) override);
    UNIMPLEMENTED(int lstat(const char *path, struct stat *buf) override);
    UNIMPLEMENTED_POINTER(IFile *creat(const char *, mode_t) override);
    UNIMPLEMENTED(int mkdir(const char *, mode_t) override);
    UNIMPLEMENTED(int rmdir(const char *) override);
    UNIMPLEMENTED(int link(const char *, const char *) override);
    UNIMPLEMENTED(int symlink(const char *, const char *) override);
    UNIMPLEMENTED(ssize_t readlink(const char *, char *, size_t) override);
    UNIMPLEMENTED(int rename(const char *, const char *) override);
    UNIMPLEMENTED(int chmod(const char *, mode_t) override);
    UNIMPLEMENTED(int chown(const char *, uid_t, gid_t) override);
    UNIMPLEMENTED(int statfs(const char *path, struct statfs *buf) override);
    UNIMPLEMENTED(int statvfs(const char *path, struct statvfs *buf) override);
    UNIMPLEMENTED(int access(const char *pathname, int mode) override);
    UNIMPLEMENTED(int truncate(const char *path, off_t length) override);
    UNIMPLEMENTED(int syncfs() override);
    UNIMPLEMENTED(int unlink(const char *filename) override);
    UNIMPLEMENTED(int lchown(const char *pathname, uid_t owner, gid_t group) override);
    UNIMPLEMENTED_POINTER(DIR *opendir(const char *) override);
    UNIMPLEMENTED(int utime(const char *path, const struct utimbuf *file_times) override);
    UNIMPLEMENTED(int utimes(const char *path, const struct timeval times[2]) override);
    UNIMPLEMENTED(int lutimes(const char *path, const struct timeval times[2]) override);
    UNIMPLEMENTED(int mknod(const char *path, mode_t mode, dev_t dev) override);

    size_t block_size;
    size_t refill_size;
    IOAlloc *io_alloc;
    ObjectCache<std::string, PersistentCacheStore *> m_file_pool;

private:
    photon::fs::IFileSystem *m_src_fs;
};

PersistentCacheFile::~PersistentCacheFile() {
    safe_delete(m_file);
    m_fs->m_file_pool.release(m_local_path);
}

ssize_t PersistentCacheFile::preadv(const struct iovec *iov, int iovcnt, off_t offset) {
    ssize_t ret = 0;
    if (!ready) {
        LOG_WARN("local path or size not set, read from source ", VALUE(offset));
        SCOPE_AUDIT("download", AU_FILEOP(m_name, offset, ret));
        ret = m_file->preadv(iov, iovcnt, offset);
        return ret;
    }

    iovector_view view((iovec *)iov, iovcnt);
    size_t count = view.sum();
    if (count == 0) {
        return 0;
    }

    std::pair<off_t, size_t> q;

    off_t align_left = align_down(offset, m_fs->block_size);
    off_t align_right = align_up(offset + count, m_fs->block_size);

again:

     m_local_file->lock(align_left, align_right - align_left);
    {
        DEFER({m_local_file->unlock(align_left, align_right - align_left);});
        q = m_local_file->query_refill_range(offset, count);
        if (q.second == 0) { // no need to refill
            return m_local_file->preadv(iov, iovcnt, offset);
        }
    }

    // refill
    uint64_t r_offset = q.first;
    uint64_t r_count = q.second;
    if (r_offset + r_count > m_size) {
        r_count = m_size - r_offset;
    }

    IOVector buffer(*m_fs->io_alloc);

    auto lr = m_local_file->try_lock_wait(r_offset, r_count);
    if (lr < 0) {
        goto again;
    }
    DEFER({ m_local_file->unlock(r_offset, r_count); });

    auto alloc = buffer.push_back(r_count);
    if (alloc < r_count) {
        LOG_ERROR("memory allocate failed, refill size:`, alloc:`", r_count, alloc);
        SCOPE_AUDIT("download", AU_FILEOP(m_name, offset, ret));
        ret = m_file->preadv(iov, iovcnt, offset);
        return ret;
    }

    ssize_t read = 0;
    {
        SCOPE_AUDIT("download", AU_FILEOP(m_name, r_offset, read));
        read = m_file->preadv(buffer.iovec(), buffer.iovcnt(), r_offset);
    }

    if (read != (ssize_t)r_count) {
        LOG_ERRNO_RETURN(0, -1, "src file read failed, read: `, expect: `, size: `, offset: `",
                         read, r_count, m_size, r_offset);
    }

    auto write = m_local_file->pwritev(buffer.iovec(), buffer.iovcnt(), r_offset);
    if (write != (ssize_t)r_count) {
        LOG_ERRNO_RETURN(0, -1, "local file write failed, write: `, expect: `, size: `, offset: `",
                         write, r_count, m_size, r_offset);
    }

    return m_local_file->preadv(iov, iovcnt, offset);
}

int PersistentCacheFile::fstat(struct stat *buf) {
    return m_file->fstat(buf);
}

int PersistentCacheFile::vioctl(int request, va_list args) {
    if (request == SET_LOCAL_DIR) {
        auto dir = va_arg(args, std::string);
        if (m_local_file != nullptr) {
            LOG_DEBUG("dir already set, ignore");
            return 0;
        }
        m_local_path = dir + "/.download";

        m_local_file = m_fs->m_file_pool.acquire(m_local_path, [&]() -> PersistentCacheStore * {
            auto f = open_localfile_adaptor(m_local_path.c_str(), O_RDWR | O_CREAT, 0644, 0);
            if (f == nullptr) {
                LOG_ERRNO_RETURN(0, nullptr, "failed to open local file ", VALUE(m_local_path));
            }
            return new PersistentCacheStore(f, m_fs);
        });

        if (m_local_file != nullptr && m_size > 0) {
            m_local_file->ftruncate(m_size);
            ready = true;
        }
        return 0;
    } else if (request == SET_SIZE) {
        if (m_size > 0) {
            LOG_DEBUG("size already set, ignore");
            return 0;
        }
        m_size = va_arg(args, size_t);
        if (m_local_file != nullptr) {
            m_local_file->ftruncate(m_size);
            ready = true;
        }
        return 0;
    } else {
        return m_file->vioctl(request, args);
    }
}

int PersistentCacheFile::fallocate(int mode, off_t offset, off_t len) {
    if (m_local_file == nullptr) {
        LOG_WARN("local path or size not set, ignore");
        return 0;
    }
    if (len == -1) {
        len = m_size - offset;
    }
    range_split_power2 rs(offset, len, m_fs->block_size);
    auto aligned_offset = rs.aligned_begin_offset();
    auto aligned_len = rs.aligned_length();
    LOG_DEBUG("fallocate offset: `, len: `, aligned offset: `, aligned len: `", offset, len,
              aligned_offset, aligned_len);

    return m_local_file->trim(aligned_offset, aligned_len);
}

std::pair<off_t, size_t> PersistentCacheStore::query_refill_range(off_t offset, size_t size) {
    off_t align_left = align_down(offset, m_fs->block_size);
    off_t align_right = align_up(offset + size, m_fs->block_size);
    auto req_offset = align_left;
    auto req_size = static_cast<size_t>(align_right - align_left);

    struct photon::fs::fiemap_t<4096> fie(req_offset, req_size);
    fie.fm_mapped_extents = 0;

    if (req_size > 0) { //  fiemap cannot handle size zero.
        auto ok = m_file->fiemap(&fie);
        if (ok != 0) {
            LOG_ERRNO_RETURN(0, std::make_pair(-1, 0),
                             "media fiemap failed : `, offset : `, size : `", ok, req_offset,
                             req_size);
        }
        if (fie.fm_mapped_extents >= 4096) {
            LOG_ERROR_RETURN(EINVAL, std::make_pair(-1, 0), "read size is too big : `", req_size);
        }
    }

    uint64_t hole_start = req_offset;
    uint64_t hole_end = req_offset + req_size;

    for (auto i = fie.fm_mapped_extents - 1; i < fie.fm_mapped_extents; i--) {
        auto &extent = fie.fm_extents[i];
        if ((extent.fe_flags == FIEMAP_EXTENT_UNKNOWN) ||
            (extent.fe_flags == FIEMAP_EXTENT_UNWRITTEN))
            continue;
        if (extent.fe_logical < hole_end) {
            if (extent.fe_logical_end() >= hole_end) {
                hole_end = extent.fe_logical;
            } else
                break;
        }
    }

    for (uint32_t i = 0; i < fie.fm_mapped_extents; i++) {
        auto &extent = fie.fm_extents[i];
        if ((extent.fe_flags == FIEMAP_EXTENT_UNKNOWN) ||
            (extent.fe_flags == FIEMAP_EXTENT_UNWRITTEN))
            continue;
        if (extent.fe_logical_end() > hole_start) {
            if (extent.fe_logical <= hole_start) {
                hole_start = extent.fe_logical_end();
            } else
                break;
        }
    }

    if (hole_start >= hole_end)
        return std::make_pair(0, 0);
    // miss
    auto left = align_down(hole_start, m_fs->refill_size);
    auto right = align_up(hole_end, m_fs->refill_size);
    return std::make_pair(left, right - left);
}


IFileSystem *new_persistent_cached_fs(photon::fs::IFileSystem *src_fs, size_t blk_size,
                                    size_t refill_size, IOAlloc *io_alloc) {
    if (io_alloc == nullptr) {
        io_alloc = new IOAlloc;
    }
    return new PersistentCacheFs(src_fs, blk_size, refill_size, io_alloc);
}
} 
} // namespace FileSystem
