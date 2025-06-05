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

#include "ocf_namespace.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <photon/common/enumerable.h>
#include <photon/fs/localfs.h>
#include <photon/fs/path.h>
#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/thread/thread.h>
#include <photon/common/checksum/crc32c.h>

extern "C" {
#include "ocf/ocf.h"
}

#define DIV_ROUND_UP(n, d) (((n) + (d)-1) / (d))

class OcfNamespaceOnFs : public OcfNamespace {
public:
    OcfNamespaceOnFs(size_t blk_size, photon::fs::IFileSystem *fs)
        : OcfNamespace(blk_size), m_fs(fs) {
    }

    int init() override {
        switch (m_blk_size) {
        case ocf_cache_line_size_4:
        case ocf_cache_line_size_8:
        case ocf_cache_line_size_16:
        case ocf_cache_line_size_32:
        case ocf_cache_line_size_64:
            break;
        default:
            LOG_ERROR_RETURN(0, -1, "OCF: invalid cache line size");
        }

        off_t max_blk_idx = 0;
        off_t last_num_blocks = 0;

        for (auto file_path : enumerable(photon::fs::Walker(m_fs, ""))) {
            NsInfo info;
            if (get_ns_info(file_path, info) != 0) {
                return -1;
            }

            if (max_blk_idx < info.blk_idx) {
                max_blk_idx = info.blk_idx;
                last_num_blocks = DIV_ROUND_UP(info.file_size, m_blk_size);
            }
        }

        m_total_blocks = max_blk_idx + last_num_blocks;
        LOG_DEBUG("OCF: set total_blocks to `", m_total_blocks);
        return 0;
    }

    int locate_file(const estring &file_path, photon::fs::IFile *src_file, NsInfo &info) override {
        if (m_fs->access(file_path.c_str(), F_OK) == 0) {
            if (get_ns_info(file_path, info) != 0) {
                LOG_ERROR_RETURN(0, -1, "OCF: get ns info failed, path `", file_path);
            }
        } else {
            struct stat st_buf {};
            if (src_file->fstat(&st_buf) != 0) {
                LOG_ERROR_RETURN(0, -1, "OCF: failed to get size of `", file_path);
            }
            if (append_ns(file_path, st_buf.st_size, info) != 0) {
                LOG_ERROR_RETURN(0, -1, "OCF: append ns failed, path `", file_path);
            }
        }
        return 0;
    }

private:
    struct NsFileFormat {
        uint32_t magic;
        uint32_t checksum;
        NsInfo info;
    };

    int get_ns_info(estring_view file_path, NsInfo &info) {
        auto file = m_fs->open(file_path.data(), O_RDONLY, 0644);
        if (file == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "OCF: failed to open ns file");
        }
        DEFER(delete file);

        NsFileFormat format;
        ssize_t ret = file->read(&format, sizeof(format));
        if (ret != (ssize_t)sizeof(format)) {
            LOG_ERRNO_RETURN(0, -1, "OCF: failed to read ns file");
        }

        if (format.magic != NS_FILE_MAGIC) {
            LOG_ERROR_RETURN(0, -1, "OCF: ns file magic error");
        }
        if (format.checksum != crc32c(&format.info, sizeof(format.info))) {
            LOG_ERROR_RETURN(0, -1, "OCF: ns file checksum error");
        }
        info = format.info;
        LOG_DEBUG("OCF: load ns_info from `, blk_idx `, file_size `", file_path, info.blk_idx,
                  info.file_size);
        return 0;
    }

    int append_ns(estring_view file_path, size_t file_size, NsInfo &info) {
        // Create dir if necessary
        auto pos = file_path.find_last_of("/");
        if (0 < pos && pos < file_path.length()) {
            auto base_dir = file_path.substr(0, pos + 1);
            if (m_fs->access(base_dir.data(), F_OK) != 0 &&
                photon::fs::mkdir_recursive(base_dir, m_fs) != 0) {
                LOG_ERRNO_RETURN(0, -1, "OCF: failed to mkdir for namespace at `", base_dir);
            }
        }

        // Lock in case of concurrent append
        photon::scoped_lock lock(m_mutex);

        // Persist ns_info into ns_fs
        info.blk_idx = (off_t)m_total_blocks;
        info.file_size = file_size;

        if (write_ns_info(file_path, info) != 0) {
            LOG_ERROR_RETURN(0, -1, "OCF: failed to write namespace file");
        }

        // Update total_blocks at last
        size_t num_blocks = DIV_ROUND_UP(file_size, m_blk_size);
        m_total_blocks += num_blocks;

        LOG_DEBUG("OCF: append namespace, file `, blk_idx `, size `", file_path, info.blk_idx,
                  info.file_size);
        return 0;
    }

    int write_ns_info(const estring &file_path, NsInfo &info) {
        NsFileFormat format = {
            .magic = NS_FILE_MAGIC,
            .checksum = crc32c(&info, sizeof(info)),
            .info = info,
        };

        auto tmp_file_path = estring().appends(file_path, ".tmp");
        {
            auto tmp_file = m_fs->open(tmp_file_path.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644);
            if (tmp_file == nullptr) {
                LOG_ERRNO_RETURN(0, -1, "OCF: failed to create tmp file `", tmp_file_path);
            }
            DEFER(delete tmp_file);

            // Write tmp file
            ssize_t n_written = tmp_file->write(&format, sizeof(format));
            if (n_written != (ssize_t)sizeof(format)) {
                delete tmp_file;
                LOG_ERRNO_RETURN(0, -1, "OCF: failed to write tmp file `", tmp_file_path);
            }
        }

        // Atomic rename
        if (m_fs->rename(tmp_file_path.c_str(), file_path.c_str()) != 0) {
            ERRNO prev_eno;
            m_fs->unlink(tmp_file_path.c_str());
            LOG_ERRNO_RETURN(prev_eno.no, -1, "OCF: failed to rename tmp file `", tmp_file_path);
        }
        return 0;
    }

    const uint32_t NS_FILE_MAGIC = UINT32_MAX - 1;
    size_t m_total_blocks = 0;
    photon::fs::IFileSystem *m_fs; // owned by external class
    photon::mutex m_mutex;
};

OcfNamespace *new_ocf_namespace_on_fs(size_t blk_size, photon::fs::IFileSystem *fs) {
    return new OcfNamespaceOnFs(blk_size, fs);
}
