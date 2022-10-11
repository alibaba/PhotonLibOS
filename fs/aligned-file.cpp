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

#include "aligned-file.h"

#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <photon/common/iovector.h>
#include "filesystem.h"
#include "forwardfs.h"
#include "range-split.h"
#include "range-split-vi.h"
#include <photon/common/utility.h>
#include <photon/common/alog.h>
#include <photon/common/io-alloc.h>

namespace photon {
namespace fs
{
    static constexpr size_t MAX_TMP_IOVEC_SIZE=32;

    class AlignedFileAdaptor : public ForwardFile_Ownership
    {
    public:
        uint32_t m_alignment;
        bool m_align_memory;
        IOAlloc m_allocator;

        AlignedFileAdaptor(IFile *underlay_file, uint32_t alignment, bool align_memory,
                           bool ownership, IOAlloc *allocator)
            : ForwardFile_Ownership(underlay_file, ownership)
        {
            // if no allocator specified, initialized by align_memory
            if (allocator == nullptr) {
                // if align_memory, bind to aligned alloc, else use default
                if (align_memory) {
                    m_allocator = AlignedAlloc(alignment);
                }
            } else {
                m_allocator = *allocator;
            }
            m_alignment = alignment;
            m_align_memory = align_memory;
        }
        void* mem_alloc(uint64_t size)
        {
            void* ptr = nullptr;
            m_allocator.allocate(IOAlloc::RangeSize{(int)size, (int)size}, &ptr);
            return ptr;
        }
        virtual ssize_t pread(void *buf, size_t count, off_t offset) override
        {
            if (count == 0) return 0;
            range_split_power2 rs(offset, count, m_alignment);
            if (rs.is_aligned() && (!m_align_memory || rs.is_aligned_ptr(buf)))
                return m_file->pread(buf, count, offset);

            auto ptr = mem_alloc(rs.aligned_length());
            if (!ptr)
                LOG_ERROR_RETURN(0, -1, "Failed to allocate memory");
            DEFER(free(ptr));
            ssize_t ret = m_file->pread(ptr, rs.aligned_length(), rs.aligned_begin_offset());
            if (ret < (ssize_t)(rs.begin_remainder)) {
                LOG_ERRNO_RETURN(0, -1, "failed to aligned [`]->pread(buf=`, count=`, offset=`), ret: ` ( < ` )",
                                 m_file, ptr, rs.aligned_length(), rs.aligned_begin_offset(), ret, rs.begin_remainder + count);
            }
            auto actual_read = ret - rs.begin_remainder;
            if (actual_read > count) actual_read = count;
            memcpy(buf, (char*)ptr + rs.begin_remainder, actual_read);
            return actual_read;
        }
        virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override
        {
            if (count == 0) return 0;
            range_split_power2 rs(offset, count, m_alignment);
            if (rs.is_aligned() && (!m_align_memory || rs.is_aligned_ptr(buf)))
                return m_file->pwrite(buf, count, offset);

            struct stat stat;
            m_file->fstat(&stat);
            ssize_t filesize = stat.st_size;
            auto suppose_filesize = (offset + (ssize_t)count) > filesize ? offset + count: filesize;
            auto ptr = mem_alloc(rs.aligned_length());
            if (!ptr)
                LOG_ERROR_RETURN(0, -1, "Failed to allocate memory");
            DEFER(free(ptr));

            if (rs.begin_remainder > 0)
            {
                ssize_t off = rs.aligned_begin_offset();
                ssize_t ret = m_file->pread(ptr, m_alignment, off);
                if ((ret < 0) || ((off + ret < filesize) && (ret < (ssize_t)m_alignment)))
                    LOG_ERRNO_RETURN(0, -1, "failed to aligned [`]->pread(ptr=`, count=`, offset=`) and patch the first alignment block", m_file, ptr, m_alignment, off);
                if (ret < (ssize_t)m_alignment) {
                    memset((char*)ptr + ret, 0, m_alignment - ret);
                }
            }

            if (!rs.small_note && rs.end_remainder > 0)
            {
                ssize_t off = rs.aligned_end_offset() - m_alignment; // this should never be negative
                if (filesize - off > (ssize_t)(rs.end_remainder)) {
                    // that means there is still parts of data after the rear end of pwrite block
                    auto ptr_ = (char*)ptr + off - rs.aligned_begin_offset();
                    ssize_t ret = m_file->pread(ptr_, m_alignment, off);
                    if ((ret < 0) ||
                        ((ret + off < filesize) && (ret < (ssize_t)m_alignment))) // cannot fetch all data of file, and cannot fillup aligned block
                        LOG_ERRNO_RETURN(0, -1, "failed to aligned [`]->pread(ptr=`, count=`, offset=`) and patch the last alignment block", m_file, ptr_, m_alignment, off);
                }
            }

            memcpy((char*)ptr + rs.begin_remainder, buf, count);
            auto actual_write = m_file->pwrite(ptr, rs.aligned_length(), rs.aligned_begin_offset());
            auto current_tail = rs.aligned_begin_offset() + actual_write;
            if ((ssize_t)current_tail < offset)
                return -1; // failed to write
            auto count_write = current_tail - offset >= count ? count : current_tail - offset;
            if (suppose_filesize < current_tail) { // that means have written more than needs
                m_file->ftruncate(suppose_filesize);
            }
            return count_write;
        }
        virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
            IOVector iovec(iov, iovcnt);
            return this->preadv_mutable(iovec.iovec(), iovec.iovcnt(), offset);
        }
        virtual ssize_t preadv_mutable(struct iovec *iov, int iovcnt, off_t offset) override {
            return this->preadv2_mutable(iov, iovcnt, offset, 0);
        }
        virtual ssize_t preadv2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override {
            IOVector iovec(iov, iovcnt);
            return this->preadv2_mutable(iovec.iovec(), iovec.iovcnt(), offset, flags);
        }

        bool iov_align_check(const iovector_view& view) {
            for (auto p : view) {
                if (((uint64_t)p.iov_base & (m_alignment - 1)) || (p.iov_len & (m_alignment - 1)))
                    return false;
            }
            return true;
        }
        virtual ssize_t preadv2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags) override {
            iovector_view buf(iov, iovcnt);
            auto count = buf.sum();
            if (count == 0) return 0;
            range_split_power2 rs(offset, count, m_alignment);
            if (rs.is_aligned() && (!m_align_memory || iov_align_check(buf)))
                return m_file->preadv2_mutable(iov, iovcnt, offset, flags);

            IOVector rbuf(m_allocator);
            auto ret = rbuf.push_back(rs.aligned_length());
            if (ret < rs.aligned_length())
                LOG_ERROR_RETURN(0, -1, "Failed to allocate");
            ret = m_file->preadv2(rbuf.iovec(), rbuf.iovcnt(), rs.aligned_begin_offset(), flags);
            if (ret < rs.begin_remainder) {
                LOG_ERRNO_RETURN(0, -1, "failed to aligned [`]->preadv2(iov=`, iovcnt=`, offset=`, flags=`), ret: ` ( < ` )",
                                 m_file, rbuf.iovec(), rbuf.iovcnt(), rs.aligned_begin_offset(), flags, ret, rs.begin_remainder + count);
            }
            auto actual_read = ret - rs.begin_remainder;
            if (actual_read > count) actual_read = count;
            if (rs.end_remainder)
                rbuf.extract_back(m_alignment - rs.end_remainder);
            rbuf.extract_front(rs.begin_remainder);
            rbuf.memcpy_to(&buf, actual_read);
            return actual_read;
        }
        virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
            IOVector iovec(iov, iovcnt);
            return this->pwritev_mutable(iovec.iovec(), iovec.iovcnt(), offset);
        }
        virtual ssize_t pwritev_mutable(struct iovec *iov, int iovcnt, off_t offset) override {
            return this->pwritev2_mutable(iov, iovcnt, offset, 0);
        }
        virtual ssize_t pwritev2(const struct iovec *iov, int iovcnt, off_t offset, int flags) override {
            IOVector iovec(iov, iovcnt);
            return this->pwritev2_mutable(iovec.iovec(), iovec.iovcnt(), offset, flags);
        }
        virtual ssize_t pwritev2_mutable(struct iovec *iov, int iovcnt, off_t offset, int flags) override {
            struct iovec tmpiovec[MAX_TMP_IOVEC_SIZE];
            iovector_view view(tmpiovec, MAX_TMP_IOVEC_SIZE);
            iovector_view buf(iov, iovcnt);
            auto count = buf.sum();
            if (count == 0) return 0;
            range_split_power2 rs(offset, count, m_alignment);
            if (rs.is_aligned() && (!m_align_memory || iov_align_check(buf)))
                return m_file->pwritev2_mutable(iov, iovcnt, offset, flags);

            struct stat stat;
            m_file->fstat(&stat);
            ssize_t filesize = stat.st_size;
            auto suppose_filesize = (offset + (ssize_t)count) > filesize ? offset + count: filesize;
            IOVector wbuf(m_allocator);
            auto ret = wbuf.push_back(rs.aligned_length());
            if (ret < rs.aligned_length())
                LOG_ERROR_RETURN(0, -1, "Failed to allocate memory");

            if (rs.begin_remainder > 0)
            {
                view.iovcnt = MAX_TMP_IOVEC_SIZE;
                ssize_t ret = wbuf.slice(m_alignment, 0, &view);
                assert(ret == m_alignment);
                ssize_t off = rs.aligned_begin_offset();
                ret = m_file->preadv2(view.iov, view.iovcnt, off, flags);
                if ((ret < 0) || ((off + ret < filesize) && (ret < (ssize_t)m_alignment)))
                    LOG_ERRNO_RETURN(0, -1, "failed to aligned [`]->preadv2(iov=`, iovcnt=`, offset=`, flags=`) and patch the first alignment block",
                                     m_file, view.iov, view.iovcnt, off, flags);
                if (ret < (ssize_t)m_alignment) {
                    view.extract_front(ret);
                    for (auto &p : view) {
                        memset(p.iov_base, 0, p.iov_len);
                    }
                }
            }
            if (!rs.small_note && rs.end_remainder > 0)
            {
                ssize_t off = rs.aligned_end_offset() - m_alignment; // this should never be negative
                if (filesize - off > (ssize_t)(rs.end_remainder)) {
                    // that means there is still parts of data after the rear end of pwrite block
                    view.iovcnt = MAX_TMP_IOVEC_SIZE;
                    ssize_t ret = wbuf.slice(m_alignment, wbuf.sum() - m_alignment, &view);
                    assert(ret == m_alignment);
                    ret = m_file->preadv2(view.iov, view.iovcnt, off, flags);
                    if (ret < 0)
                        LOG_ERRNO_RETURN(0, -1, "failed to aligned [`]->preadv2(ptr=`, count=`, offset=`, flags=`)",
                                         m_file, view.iov, view.iovcnt, off, flags);
                    if ((ret + off < filesize) && (ret < (ssize_t)m_alignment)) // cannot fetch all data of file, and cannot fillup aligned block
                        LOG_ERRNO_RETURN(0, -1, "aligned [`]->preadv2(ptr=`, count=`, offset=`, flags=`) partial read",
                                         m_file, view.iov, view.iovcnt, off, flags);
                }
            }
            view.iovcnt = MAX_TMP_IOVEC_SIZE;
            ret = wbuf.slice(count, rs.begin_remainder, &view);
            assert(ret == count);
            ret = view.memcpy_from(&buf, count);
            assert(ret == count);
            auto actual_write = m_file->pwritev2_mutable(wbuf.iovec(), wbuf.iovcnt(), rs.aligned_begin_offset(), flags);
            auto current_tail = rs.aligned_begin_offset() + actual_write;
            if ((ssize_t)current_tail < offset)
                LOG_ERRNO_RETURN(0, -1, "failed to pwritev2");
            if (suppose_filesize < current_tail) { // that means have written more than needs
                m_file->ftruncate(suppose_filesize);
                current_tail = suppose_filesize;
            } else { // means pwrite will not extend file

            }
            auto count_write = current_tail - offset;
            if (count_write > count)
                count_write = count;
            return count_write;

        }
    };

    class AlignedFSAdaptor: public ForwardFS_Ownership
    {


    public:
        uint32_t m_alignment;
        bool m_align_memory;
        IOAlloc *m_allocator;
        AlignedFSAdaptor(IFileSystem* underlay_fs, uint32_t alignment, bool align_memory, bool ownership, IOAlloc* allocator):
            ForwardFS_Ownership(underlay_fs, ownership),
            m_alignment(alignment),
            m_align_memory(align_memory),
            m_allocator(allocator)
        {
        }
        virtual IFile* open(const char* path, int flags, mode_t mode) override {
            auto file = m_fs->open(path, flags, mode);
            if (file == nullptr)
                return nullptr;
            return new AlignedFileAdaptor(
                file,
                m_alignment,
                m_align_memory,
                true,
                m_allocator
            );
        }
        virtual IFile* open(const char* path, int flags) override {
            auto file = m_fs->open(path, flags);
            if (file == nullptr)
                return nullptr;
            return new AlignedFileAdaptor(
                file,
                m_alignment,
                m_align_memory,
                true,
                m_allocator
            );
        }
    };

    IFile* new_aligned_file_adaptor(IFile* file, uint32_t alignment, bool align_memory, bool ownership, IOAlloc* alloc)
    {
        if (file == nullptr)
            LOG_ERROR_RETURN(0, nullptr, "cannot open file");

        if (!is_power_of_2(alignment))
            LOG_ERROR_RETURN(EINVAL, nullptr, "alignment must be 2^n");

        return new AlignedFileAdaptor(file, alignment, align_memory, ownership, alloc);
    }

    IFileSystem* new_aligned_fs_adaptor(IFileSystem* fs, uint32_t alignment, bool align_memory, bool ownership, IOAlloc* alloc)
    {
        if (!is_power_of_2(alignment))
            LOG_ERROR_RETURN(EINVAL, nullptr, "alignment must be 2^n");

        return new AlignedFSAdaptor(fs, alignment, align_memory, ownership, alloc);
    }
}
}
