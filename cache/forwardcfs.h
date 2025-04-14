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
#include <cache.h>
#include <photon/fs/forwardfs.h>

namespace photon {
namespace fs{
template <class IForwardCachedFile>
class ForwardCachedFileBase : public IForwardCachedFile {
protected:
    using Base = IForwardCachedFile;
    using Base::Base;
    virtual photon::fs::IFile *get_source() override {
        return Base::m_file->get_source();
    }
    virtual int set_source(photon::fs::IFile *src) override {
        return Base::m_file->set_source(src);
    }
    virtual ICacheStore *get_store() override {
        return Base::m_file->get_store();
    }
    virtual int query(off_t offset, size_t count) override {
        return Base::m_file->query(offset, count);
    }
};
using ForwardCachedFile = ForwardCachedFileBase<photon::fs::ForwardFileBase<ICachedFile>>;
using ForwardCachedFile_Ownership =
    ForwardCachedFileBase<photon::fs::ForwardFileBase_Ownership<ICachedFile>>;

template <class IForwardCachedFS>
class ForwardCachedFSBase : public IForwardCachedFS {
protected:
    using Base = IForwardCachedFS;
    using Base::Base;
    virtual photon::fs::IFileSystem *get_source() override {
        return Base::m_fs->get_source();
    }
    virtual int set_source(photon::fs::IFileSystem *src) override {
        return Base::m_fs->set_source(src);
    }
    virtual ICachePool *get_pool() override {
        return Base::m_fs->get_pool();
    }
    virtual int set_pool(ICachePool *pool) override {
        return Base::m_fs->set_pool(pool);
    }
};
using ForwardCachedFS = ForwardCachedFSBase<photon::fs::ForwardFSBase<ICachedFileSystem>>;
using ForwardCachedFS_Ownership =
    ForwardCachedFSBase<photon::fs::ForwardFSBase_Ownership<ICachedFileSystem>>;
}
} // namespace FileSystem
