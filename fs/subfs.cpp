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

#include "subfs.h"
#include <sys/stat.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
//#include <linux/limits.h>
#include "filesystem.h"
#include "forwardfs.h"
#include "path.h"
#include <photon/common/iovector.h>
#include <photon/common/alog.h>

namespace photon {
namespace fs
{
    // Sub tree of a file system
    class SubFileSystem : public IFileSystem
    {
    public:
        IFileSystem* underlayfs = nullptr;
        char base_path[PATH_MAX];
        uint base_path_len = 0;
        bool ownership = false;

        int init(IFileSystem* _underlayfs, const char* _base_path, bool _ownership)
        {
            ownership = _ownership;
            underlayfs = _underlayfs;
            if (!_base_path || !_base_path[0])         // use default relative path
            {
                base_path[0] = '\0';
                base_path_len = 0;
                return 0;
            }

            struct stat st;
            int ret = underlayfs->stat(_base_path, &st);
            if (ret < 0 || !S_ISDIR(st.st_mode))
                LOG_ERROR_RETURN(EINVAL, -1, "the base path '`' of file system [`] must be an directory!",
                                 _base_path, _underlayfs);

            base_path_len = (uint)strlen(_base_path);
            assert(base_path_len > 0);
            if (base_path_len > LEN(base_path) - 2)
                LOG_ERROR_RETURN(EINVAL, -1, "the base path '`' is too long!", _base_path);

            memcpy(base_path, _base_path, base_path_len);
            if (base_path[base_path_len - 1] != '/')
                base_path[base_path_len++] = '/';

            return 0;
        }
        virtual ~SubFileSystem() override
        {
            if (ownership)
                delete underlayfs;
        }

        class PathCat
        {
        public:
            char buf[PATH_MAX];
            PathCat(const SubFileSystem* subfs, const char*& path)
            {
                if (subfs->base_path_len == 0) return;

                size_t len = strlen(path);
                size_t len2 = len + subfs->base_path_len;
                if (len2 >= sizeof(buf) - 2)
                {
                    path = nullptr;
                    LOG_ERROR_RETURN(0, , "path '`' too long!", path);
                }

                if (!path_level_valid(path))
                {
                    path = nullptr;
                    LOG_ERROR_RETURN(0, , "path '`' tries to escape from base dir by too many double dots '..'", path);
                }

                memcpy(buf, subfs->base_path, subfs->base_path_len);
                memcpy(buf + subfs->base_path_len, path, len);
                buf[len2] = '\0';
                path = buf;
            }
        };

        virtual IFile* open(const char *path, int flags) override
        {
            PathCat __(this, path);
            return underlayfs->open(path, flags);
        }
        virtual IFile* open(const char *path, int flags, mode_t mode) override
        {
            PathCat __(this, path);
            return underlayfs->open(path, flags, mode);
        }
        virtual IFile* creat(const char *path, mode_t mode) override
        {
            PathCat __(this, path);
            return underlayfs->creat(path, mode);
        }
        virtual int mkdir(const char *path, mode_t mode) override
        {
            PathCat __(this, path);
            return underlayfs->mkdir(path, mode);
        }
        virtual int rmdir(const char *path) override
        {
            PathCat __(this, path);
            return underlayfs->rmdir(path);
        }
        virtual int symlink(const char *oldname, const char *newname) override
        {
            PathCat __(this, newname);
            return underlayfs->symlink(oldname, newname);
        }
        virtual ssize_t readlink(const char *path, char *buf, size_t bufsiz) override
        {
            PathCat __(this, path);
            return underlayfs->readlink(path, buf, bufsiz);
        }
        virtual int link(const char *oldname, const char *newname) override
        {
            PathCat __(this, oldname);
            PathCat ___(this, newname);
            return underlayfs->link(oldname, newname);
        }
        virtual int rename(const char *oldname, const char *newname) override
        {
            PathCat __(this, oldname);
            PathCat ___(this, newname);
            return underlayfs->rename(oldname, newname);
        }
        virtual int unlink(const char *path) override
        {
            PathCat __(this, path);
            return underlayfs->unlink(path);
        }
        virtual int chmod(const char *path, mode_t mode) override
        {
            PathCat __(this, path);
            return underlayfs->chmod(path, mode);
        }
        virtual int chown(const char *path, uid_t owner, gid_t group) override
        {
            PathCat __(this, path);
            return underlayfs->chown(path, owner, group);
        }
        virtual int lchown(const char *path, uid_t owner, gid_t group) override
        {
            PathCat __(this, path);
            return underlayfs->lchown(path, owner, group);
        }
        virtual DIR* opendir(const char *path) override
        {
            PathCat __(this, path);
            return underlayfs->opendir(path);
        }
        virtual int stat(const char *path, struct stat *buf) override
        {
            PathCat __(this, path);
            return underlayfs->stat(path, buf);
        }
        virtual int lstat(const char *path, struct stat *buf) override
        {
            PathCat __(this, path);
            return underlayfs->lstat(path, buf);
        }
        virtual int access(const char *path, int mode) override
        {
            PathCat __(this, path);
            return underlayfs->access(path, mode);
        }
        virtual int truncate(const char *path, off_t length) override
        {
            PathCat __(this, path);
            return underlayfs->truncate(path, length);
        }
        virtual int syncfs() override
        {
            return underlayfs->syncfs();
        }
        virtual int statfs(const char *path, struct statfs *buf) override
        {
            PathCat __(this, path);
            return underlayfs->statfs(path, buf);
        }
        virtual int statvfs(const char *path, struct statvfs *buf) override
        {
            PathCat __(this, path);
            return underlayfs->statvfs(path, buf);
        }
        virtual int utime(const char *path, const struct utimbuf *file_times) override
        {
            PathCat __(this, path);
            return underlayfs->utime(path, file_times);
        }
        virtual int utimes(const char *path, const struct timeval times[2]) override
        {
            PathCat __(this, path);
            return underlayfs->utimes(path, times);
        }
        virtual int lutimes(const char *path, const struct timeval times[2]) override
        {
            PathCat __(this, path);
            return underlayfs->lutimes(path, times);
        }
        virtual int mknod(const char *path, mode_t mode, dev_t dev) override
        {
            PathCat __(this, path);
            return underlayfs->mknod(path, mode, dev);
        }
        /*
        virtual ssize_t getxattr(const char *path, const char *name, void *value, size_t size)
        {
            PathCat __(this, path);
            return underlayfs->getxattr(path, name, value, size);
        }
        virtual ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size)
        {
            PathCat __(this, path);
            return underlayfs->lgetxattr(path, name, value, size);
        }
        virtual ssize_t listxattr(const char *path, char *list, size_t size)
        {
            PathCat __(this, path);
            return underlayfs->listxattr(path, list, size);
        }
        virtual ssize_t llistxattr(const char *path, char *list, size_t size)
        {
            PathCat __(this, path);
            return underlayfs->llistxattr(path, list, size);
        }
        virtual int setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
        {
            PathCat __(this, path);
            return underlayfs->setxattr(path, name, value, size, flags);
        }
        virtual int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags)
        {
            PathCat __(this, path);
            return underlayfs->lsetxattr(path, name, value, size, flags);
        }
        virtual int removexattr(const char *path, const char *name)
        {
            PathCat __(this, path);
            return underlayfs->removexattr(path, name);
        }
        virtual int lremovexattr(const char *path, const char *name)
        {
            PathCat __(this, path);
            return underlayfs->lremovexattr(path, name);
        }
        */
    };
    class SubFile : public ForwardFile_Ownership
    {
    public:
        off_t m_offset;
        size_t m_length;
        bool m_ownership;
        SubFile(IFile* file, off_t offset, size_t length, bool ownership) :
            ForwardFile_Ownership(file, ownership), m_offset(offset), m_length(length)
        {
        }
        virtual ssize_t pread(void *buf, size_t count, off_t offset) override
        {
            if (offset >= (off_t)m_length)
                return 0;
            if (offset + count > m_length)
                count = m_length - offset;
            return m_file->pread(buf, count, offset + m_offset);
        }
        virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override
        {
            if (offset >= (off_t)m_length)
                return 0;
            if (offset + count > m_length)
                count = m_length - offset;
            return m_file->pwrite(buf, count, offset + m_offset);
        }
        virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            if (offset >= (off_t)m_length)
                return 0;
            IOVector iovs(iov, iovcnt);
            iovs.shrink_to(m_length - offset);
            return m_file->preadv(iovs.iovec(), iovs.iovcnt(), offset + m_offset);
        }
        virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override
        {
            if (offset >= (off_t)m_length)
                return 0;
            IOVector iovs(iov, iovcnt);
            iovs.shrink_to(m_length - offset);
            return m_file->pwritev(iovs.iovec(), iovs.iovcnt(), offset);
        }
    };

    IFileSystem* new_subfs(IFileSystem* underlayfs, const char* base_path, bool ownership)
    {
        auto subfs = new SubFileSystem;
        int ret = subfs->init(underlayfs, base_path, ownership);
        if (ret < 0)
        {
            delete subfs;
            return nullptr;
        }
        return subfs;
    }

    IFile* new_subfile(IFile* underlay_file, off_t offset, size_t length, bool ownership)
    {
        return new SubFile(underlay_file, offset, length, ownership);
    }
}
}
