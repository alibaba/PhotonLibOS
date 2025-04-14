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

#include <unistd.h>

#include <photon/common/object.h>
#include <photon/common/estring.h>
#include <photon/common/callback.h>
#include <photon/fs/filesystem.h>

class OcfNamespace : public Object {
public:
    explicit OcfNamespace(size_t blk_size) : m_blk_size(blk_size) {
    }

    /**
     * @brief Validate parameters, and load some metadata into memory
     */
    virtual int init() = 0;

    /** NsInfo indicates a file's starting offset within its filesystem's address space, and its
     * size */
    struct NsInfo {
        off_t blk_idx;
        size_t file_size;
    };

    /**
     * @brief Locate a source file in namespace
     * @param[in] file_path
     * @param[in] src_file
     * @param[out] info
     * @retval 0 for success
     */
    virtual int locate_file(const estring &file_path, photon::fs::IFile *src_file,
                            NsInfo &info) = 0;

    size_t block_size() const {
        return m_blk_size;
    }

protected:
    size_t m_blk_size;
};

OcfNamespace *new_ocf_namespace_on_fs(size_t blk_size, photon::fs::IFileSystem *fs);

OcfNamespace *new_ocf_namespace_on_rocksdb(size_t blk_size);