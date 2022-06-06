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

namespace photon {
namespace fs
{
    struct ThrottleLimits
    {
        uint32_t struct_size = sizeof(ThrottleLimits);

        // the time window (in seconds) of I/O events to analyse, minimally 1
        uint32_t time_window = 1;

        struct UpperLimits
        {
            uint32_t concurent_ops = 0, IOPS = 0, throughput = 0, block_size = 0;
        };

        // limits for read, write, and either read or write
        UpperLimits R, W, RW;
    };

    class IFileSystem;
    class IFile;

    extern "C" IFile *new_throttled_file(IFile *file,
                                         const ThrottleLimits &limits,
                                         bool ownership = false);

    extern "C" IFileSystem *new_throttled_fs(IFileSystem *fs,
                                             const ThrottleLimits &limits,
                                             bool ownership = false);
}
}
