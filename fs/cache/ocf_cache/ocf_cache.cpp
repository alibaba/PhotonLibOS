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

#include <sys/stat.h>

#include <photon/common/estring.h>
#include <photon/common/io-alloc.h>
#include <photon/common/iovector.h>
#include <photon/common/string-keyed.h>
#include <photon/common/expirecontainer.h>
#include <photon/fs/aligned-file.h>
#include <photon/fs/forwardfs.h>
#include <photon/fs/virtual-file.h>
#include <photon/common/alog-stdstring.h>
#include <photon/fs/cache/cache.h>
#include "ocf_namespace.h"
#include "photon_bindings/provider.h"


extern IOAlloc *g_io_alloc;


namespace photon {
namespace fs {

class OcfTruncateFile : public ForwardFile_Ownership {
public:
    OcfTruncateFile(IFile *file, size_t file_size)
        : ForwardFile_Ownership(file, true), m_file_size(file_size) {
    }

    ssize_t pread(void *buf, size_t count, off_t offset) override {
        if (offset > (off_t)m_file_size || count == 0) {
            /* Read EOF, or read nothing */
            return 0;
        } else if (offset < 0) {
            errno = ENOSYS;
            LOG_ERRNO_RETURN(0, -1, "Invalid parameters, offset `", offset);
        } else if (count + offset > m_file_size) {
            count = m_file_size - offset;
        }
        return m_file->pread(buf, count, offset);
    }

    ssize_t preadv(const iovec *iov, int iovcnt, off_t offset) override {
        IOVector iv(iov, iovcnt);
        auto result = truncate_iov(iv, offset);
        if (result <= 0) {
            return result;
        }
        return m_file->preadv(iv.iovec(), iv.iovcnt(), offset);
    }

private:
    int truncate_iov(IOVector &iv, off_t offset) const {
        size_t count = iv.sum();
        if (offset > (off_t)m_file_size || count == 0) {
            /* Read EOF, or read nothing */
            return 0;
        } else if (offset < 0) {
            errno = ENOSYS;
            LOG_ERRNO_RETURN(0, -1, "Invalid parameters, offset `", offset);
        } else if (count + offset > m_file_size) {
            count = m_file_size - offset;
            iv.truncate(count);
        }
        return 1;
    }

    size_t m_file_size;
};

class OcfCachedFs;

class OcfCachedFile : public VirtualFile {
public:
    OcfCachedFile(OcfCachedFs *fs, OcfSrcFileCtx *ctx);

    ~OcfCachedFile();

    ssize_t pread(void *buf, size_t count, off_t offset) override;

    int fadvise(off_t offset, off_t len, int advice) override;

    inline estring get_pathname() {
        return m_path_name;
    };

    inline void set_pathname(const estring &pathname) {
        m_path_name = pathname;
    }

    int fstat(struct stat *buf) override {
        return m_ctx->src_file->fstat(buf);
    }

    int vioctl(int request, va_list args) override {
        return m_ctx->src_file->vioctl(request, args);
    }

    UNIMPLEMENTED_POINTER(IFileSystem *filesystem() override);
    UNIMPLEMENTED(off_t lseek(off_t offset, int whence) override);
    UNIMPLEMENTED(int fsync() override);
    UNIMPLEMENTED(int fdatasync() override);
    UNIMPLEMENTED(int fchmod(mode_t mode) override);
    UNIMPLEMENTED(int fchown(uid_t owner, gid_t group) override);
    UNIMPLEMENTED(int ftruncate(off_t length) override);
    UNIMPLEMENTED(int close() override);

private:
    OcfCachedFs *m_fs;    // owned by external class
    OcfSrcFileCtx *m_ctx; // owned by external class
    estring m_path_name;
};

class OcfCachedFs : public IFileSystem {
public:
    OcfCachedFs(IFileSystem *src_fs, size_t prefetch_unit, OcfNamespace *ocf_ns,
                IFile *media_file, bool reload_media, IOAlloc *io_alloc);

    ~OcfCachedFs();

    int init();

    ssize_t ocf_pread(void *buf, size_t count, off_t offset, OcfSrcFileCtx *ctx);

    inline void pooled_release(OcfCachedFile *file) {
        m_src_file_pool.release(file->get_pathname());
    }

    inline IOAlloc *get_io_alloc() const {
        return m_io_alloc;
    }

    IFile *open(const char *pathname, int flags, mode_t mode) override;

    IFile *open(const char *pathname, int flags) override {
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

private:
    IFileSystem *m_src_fs; // owned by external class
    size_t m_prefetch_unit;
    OcfNamespace *m_ocf_ns;          // owned by self
    IFile *m_media_file; // owned by external class
    bool m_reload_media;
    IOAlloc *m_io_alloc; // owned by external class

    ObjectCache<std::string, OcfSrcFileCtx *> m_src_file_pool;

    ease_ocf_volume_params *m_volume_params = nullptr; // owned by self
    ease_ocf_provider *m_provider = nullptr;           // owned by self
};

OcfCachedFile::OcfCachedFile(OcfCachedFs *fs, OcfSrcFileCtx *ctx) : m_fs(fs), m_ctx(ctx) {
}

OcfCachedFile::~OcfCachedFile() {
    m_fs->pooled_release(this);
}

ssize_t OcfCachedFile::pread(void *buf, size_t count, off_t offset) {
    auto ret = m_fs->ocf_pread(buf, count, offset, m_ctx);
    if (ret != (ssize_t)count) {
        LOG_ERRNO_RETURN(0, ret, "OcfCachedFile pread failed");
    }
    return count;
}

int OcfCachedFile::fadvise(off_t offset, off_t len, int advice) {
    if (advice == POSIX_FADV_WILLNEED) {
        void *buf = m_fs->get_io_alloc()->alloc(len);
        DEFER(m_fs->get_io_alloc()->dealloc(buf));
        auto ret = pread(buf, len, offset);
        if (ret < 0) {
            LOG_ERROR_RETURN(0, -1, "prefetch read failed");
        }
        return 0;
    }
    LOG_ERRNO_RETURN(ENOSYS, -1, "advice ` is not implemented", advice);
}

OcfCachedFs::OcfCachedFs(IFileSystem *src_fs, size_t prefetch_unit,
                         OcfNamespace *ocf_ns, IFile *media_file, bool reload_media,
                         IOAlloc *io_alloc)
    : m_src_fs(src_fs), m_prefetch_unit(prefetch_unit), m_ocf_ns(ocf_ns), m_media_file(media_file),
      m_reload_media(reload_media), m_io_alloc(io_alloc), m_src_file_pool(1 * 1000 * 1000) {
}

OcfCachedFs::~OcfCachedFs() {
    m_provider->stop();
    delete m_provider;
    delete m_volume_params;
    delete m_ocf_ns;
}

int OcfCachedFs::init() {
    if (m_prefetch_unit % ease_ocf_provider::SectorSize != 0) {
        LOG_ERROR_RETURN(0, -1, "OCF: invalid prefetch unit");
    }
    g_io_alloc = m_io_alloc;

    struct stat buf {};
    if (m_media_file->fstat(&buf) != 0) {
        LOG_ERROR_RETURN(0, -1, "OCF: failed to get media file size");
    }
    size_t media_size = buf.st_size;
    m_volume_params =
        new ease_ocf_volume_params{m_ocf_ns->block_size(), media_size, m_media_file, false};
    m_provider = new ease_ocf_provider(m_volume_params, m_prefetch_unit);

    return m_provider->start(m_reload_media);
}

ssize_t OcfCachedFs::ocf_pread(void *buf, size_t count, off_t offset, OcfSrcFileCtx *ctx) {
    size_t blk_addr = ctx->ns_info.blk_idx * m_volume_params->blk_size;
    bool prefetch = (m_prefetch_unit != 0);
    return m_provider->ocf_pread(buf, count, offset, blk_addr, ctx, prefetch);
}

IFile *OcfCachedFs::open(const char *pathname, int flags, mode_t mode) {
    estring path_str(pathname);
    if (path_str.ends_with("/")) {
        LOG_ERROR_RETURN(0, nullptr, "OCF: cannot open a directory `", path_str.c_str());
    }

    auto ctor = [&]() -> OcfSrcFileCtx * {
        // Open src file first
        auto src_file = m_src_fs->open(path_str.c_str(), flags, mode);
        if (src_file == nullptr) {
            LOG_ERRNO_RETURN(0, nullptr, "OCF: failed to open src file of `", path_str);
        }

        OcfNamespace::NsInfo info{};
        if (m_ocf_ns->locate_file(path_str, src_file, info) != 0) {
            LOG_ERROR_RETURN(0, nullptr, "OCF: failed to locate src_file in namespace, path `",
                             path_str);
        }
        return new OcfSrcFileCtx(src_file, info, m_provider, path_str);
    };

    auto src_file_ctx = m_src_file_pool.acquire(path_str, ctor);
    if (src_file_ctx == nullptr) {
        LOG_ERROR_RETURN(0, nullptr, "OCF: failed to open ` from pool", path_str);
    }

    auto cached_file = new OcfCachedFile(this, src_file_ctx);
    cached_file->set_pathname(path_str);

    return new OcfTruncateFile(cached_file, src_file_ctx->ns_info.file_size);
}


IFileSystem *new_ocf_cached_fs(IFileSystem *src_fs, IFileSystem *namespace_fs, size_t blk_size,
                               size_t prefetch_unit, IFile *media_file, bool reload_media,
                               IOAlloc *io_alloc) {
    auto ocf_ns = new_ocf_namespace_on_fs(blk_size, namespace_fs);
    if (ocf_ns->init() != 0) {
        delete ocf_ns;
        LOG_ERROR_RETURN(0, nullptr, "OCF: init namespace failed");
    }

    auto fs =
        new OcfCachedFs(src_fs, prefetch_unit, ocf_ns, media_file, reload_media, io_alloc);
    if (fs->init() != 0) {
        delete fs;
        LOG_ERROR_RETURN(0, nullptr, "OCF: init cache fs failed");
    }
    return fs;
}

} 
} /* namespace photon::fs */
