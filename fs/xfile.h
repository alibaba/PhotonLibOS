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
    class IFile;

    // create a linear file that is composed sequencially by `n` `files` of
    // distinct sizes, optionally obtain their ownership (resursive destruction)
    // the sizes of the files will obtained by their fstat() function
    IFile* new_linear_file(IFile** files, uint64_t n, bool ownership = false);

    // create a linear file that is compsoed sequencially by `n` `files` of
    // the same `unit_size`, optionally obtain their ownership (resursive destruction)
    IFile* new_fixed_size_linear_file(uint64_t unit_size, IFile** files, uint64_t n, bool ownership = false);

    // create a strip file that is composed by stripping `n` `files` of the same size
    // `stripe_size` must be power of 2
    IFile* new_stripe_file(uint64_t stripe_size, IFile** files, uint64_t n, bool ownership = false);
}
}
