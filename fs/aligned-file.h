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

struct AlignedAlloc;
struct IOAlloc;

namespace photon {
namespace fs
{
    class IFile;
    class IFileSystem;


    // create an adaptor to freely access a file that requires aligned access
    // alignment must be 2^n
    // only pread() and pwrite() are supported
    IFile* new_aligned_file_adaptor(IFile* file, uint32_t alignment,
                                    bool align_memory, bool ownership = false,
                                    IOAlloc* allocator = nullptr);

    IFileSystem* new_aligned_fs_adaptor(IFileSystem* fs, uint32_t alignment,
                                        bool align_memory, bool ownership,
                                        IOAlloc* allocator = nullptr);

}
}
