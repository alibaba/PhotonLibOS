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

#include "xfile.h"
#include <assert.h>
#include <vector>
#include <new>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "virtual-file.h"
#include "range-split.h"
#include "range-split-vi.h"
#include <photon/common/utility.h>
#include <photon/common/alog.h>

using namespace std;

namespace photon {
namespace fs
{
    class XFile : public VirtualFile
    {
    public:
        vector<IFile*> m_files;
        uint64_t m_size = 0;
        bool m_ownership;
        int init(IFile** files, uint64_t n, bool ownership)
        {
            m_files.assign(files, files + n);
            m_ownership = ownership;
            assert(files);
            assert(n > 0);
            return 0;
        }
        virtual int close() override
        {
            if (m_ownership)
                for (auto x: m_files)
                    x->close();
            m_files.clear();
            return 0;
        }
        virtual ssize_t pio(const ALogStringL& piof_name, FuncPIO piof,
                            void *buf, size_t count, off_t offset) = 0;
        virtual ssize_t pread(void *buf, size_t count, off_t offset) override
        {
            return pio("pread", _and_pread(), buf, count, offset);
        }
        virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override
        {
            return pio("pwrite", _and_pwrite(), (void*)buf, count, offset);
        }
        virtual int fsync() override
        {
            for (auto x: m_files)
                x->fsync();
            return 0;
        }
        virtual int fdatasync() override
        {
            for (auto x: m_files)
                x->fdatasync();
            return 0;
        }
        virtual int fstat(struct stat *buf) override
        {
            int ret = m_files.front()->fstat(buf);
            if (ret < 0)
                return ret;

            buf->st_size = m_size;
            return ret;
        }
        UNIMPLEMENTED_POINTER(IFileSystem* filesystem() override);
        UNIMPLEMENTED(int fchmod(mode_t mode) override);
        UNIMPLEMENTED(int fchown(uid_t owner, gid_t group) override);
        UNIMPLEMENTED(int ftruncate(off_t length) override);
    };

    template<typename RangeSplit>
    class FixedSizeLinearFile : public XFile
    {
    public:
        uint64_t m_unit_size;
        int init(uint64_t unit_size, IFile** files, uint64_t n, bool ownership)
        {
            m_unit_size = unit_size;
            m_size = n * unit_size;
            return XFile::init(files, n, ownership);
        }
        virtual ssize_t pio(const ALogStringL& piof_name, FuncPIO piof,
                            void *buf, size_t count, off_t offset) override
        {
            if (offset < 0 || (uint64_t)offset >= m_size) {
                LOG_ERRNO_RETURN(EIO, -1, "offset ` overflow", offset);
            } else if ((uint64_t)offset + count > m_size) {
                count = m_size - offset;
            }
            RangeSplit rs(offset, count, m_unit_size);
            for (auto& x: rs.all_parts())
            {
                ssize_t ret = (m_files[x.i]->*piof)(buf, x.length, x.offset);
                if (ret < (ssize_t)x.length)
                    LOG_ERRNO_RETURN(0, -1, "failed to [`]->`()", m_files[x.i], piof_name);
                (char*&)buf += x.length;
            }
            return count;
        }
    };

    class VariableSizeLinearFile : public XFile
    {
    public:
        vector<uint64_t> m_key_points;
        int init(IFile** files, uint64_t n, bool ownership)
        {
            XFile::init(files, n, ownership);
            m_key_points.reserve(m_files.size() + 2);
            m_key_points.push_back(0);
            for (auto f: m_files)
            {
                struct stat stat;
                int ret = f->fstat(&stat);
                if (ret < 0)
                    LOG_ERRNO_RETURN(0, -1, "failed to [`]->fstat()", f);
                m_key_points.push_back(m_key_points.back() + stat.st_size);
                m_size += stat.st_size;
            }
            m_key_points.push_back(UINT64_MAX);
            return 0;
        }
        virtual ssize_t pio(const ALogStringL& piof_name, FuncPIO piof,
                            void *buf, size_t count, off_t offset) override
        {
            if (offset < 0 || (uint64_t)offset >= m_size) {
                LOG_ERRNO_RETURN(EIO, -1, "offset ` overflow", offset);
            } else if ((uint64_t)offset + count > m_size) {
                count = m_size - offset;
            }
            range_split_vi rs(offset, count, &m_key_points[0], m_key_points.size());
            for (auto& x: rs.all_parts())
            {
                ssize_t ret = (m_files[x.i]->*piof)(buf, x.length, x.offset);
                if (ret < (ssize_t)x.length)
                    LOG_ERRNO_RETURN(0, -1, "failed to [`]->`()", m_files[x.i], piof_name);
                (char*&)buf += x.length;
            }
            return count;
        }
    };

    class StripeFile : public XFile
    {
    public:
        uint64_t m_stripe_size;
        int init(uint64_t stripe_size, IFile** files, uint64_t n, bool ownership)
        {
            m_stripe_size = stripe_size;
            XFile::init(files, n, ownership);
            for (auto i: xrange(n))
            {
                struct stat stat;
                auto& f = m_files[i];
                int ret = f->fstat(&stat);
                if (ret < 0)
                    LOG_ERRNO_RETURN(0, -1, "failed to [`]->fstat()", f);

                if (stat.st_size == 0)
                    LOG_ERRNO_RETURN(0, -1, "size of `-th file [`] must be >0 ", i, f);

                if (stat.st_size % m_stripe_size != 0)
                    LOG_ERRNO_RETURN(0, -1, "size of `-th file [`] must be multiple of stripe size ", i, f, m_stripe_size);

                if (!m_size) {
                    m_size = stat.st_size;
                } else if (m_size != (uint64_t)stat.st_size) {
                    LOG_ERRNO_RETURN(0, -1, "size of files must be equal!");
                }
            }
            m_size *= n;
            return 0;
        }
        virtual ssize_t pio(const ALogStringL& piof_name, FuncPIO piof,
                            void *buf, size_t count, off_t offset) override
        {   // TODO: batch pread & pwrite with iovectorized I/O (preadv / pwritev)
            if (offset < 0 || (uint64_t)offset >= m_size) {
                LOG_ERRNO_RETURN(EIO, -1, "offset ` overflow", offset);
            } else if ((uint64_t)offset + count > m_size) {
                count = m_size - offset;
            }
            range_split_power2 rs(offset, count, m_stripe_size);
            for (auto& x: rs.all_parts())
            {
                auto i = x.i % m_files.size();
                auto pos = rs.multiply(x.i / m_files.size(), x.offset);
                ssize_t ret = (m_files[i]->*piof)(buf, x.length, pos);
                if (ret < (ssize_t)x.length)
                    LOG_ERRNO_RETURN(0, -1, "failed to [`]->`()", m_files[x.i], piof_name);
                (char*&)buf += x.length;
            }
            return count;
        }
    };

    template<typename F, typename...Ts>
    static inline IFile* new_file(Ts...xs)
    {
        auto file = new(nothrow) F();
        if (!file)
            return nullptr;

        if (file->init(xs...) >= 0)
            return file;

        delete file;
        return nullptr;
    }

    IFile* new_fixed_size_linear_file(uint64_t unit_size, IFile** files, uint64_t n, bool ownership)
    {
        if (n == 0 || !files)
            LOG_ERROR_RETURN(EINVAL, nullptr, "no underlay file(s)");

        if (unit_size == 0)
            LOG_ERROR_RETURN(EINVAL, nullptr, "unit size must be >0!");

        return is_power_of_2(unit_size) ?
            new_file<FixedSizeLinearFile<range_split_power2>>(unit_size, files, n, ownership) :
            new_file<FixedSizeLinearFile<range_split>>(unit_size, files, n, ownership);
    }

    IFile* new_linear_file(IFile** files, uint64_t n, bool ownership)
    {
        if (n == 0 || !files)
            LOG_ERROR_RETURN(EINVAL, nullptr, "no underlay file(s)");

        return new_file<VariableSizeLinearFile>(files, n ,ownership);
    }

    IFile* new_stripe_file(uint64_t stripe_size, IFile** files, uint64_t n, bool ownership)
    {
        if (n == 0 || !files)
            LOG_ERROR_RETURN(EINVAL, nullptr, "no underlay file(s)");

        if (!is_power_of_2(stripe_size))
            LOG_ERROR_RETURN(EINVAL, nullptr, "stripe size must be power of 2!");

        return new_file<StripeFile>(stripe_size, files, n, ownership);
    }
}
}
