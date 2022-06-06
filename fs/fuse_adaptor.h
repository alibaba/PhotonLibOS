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

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 29
#endif

#include <fuse.h>
#include <photon/fs/filesystem.h>

namespace photon {
namespace fs {

int fuser_go(fs::IFileSystem* fs, int argc, char* argv[]);

int fuser_go_exportfs(fs::IFileSystem* fs, int argc, char* argv[]);

void set_fuse_fs(fs::IFileSystem* fs);

fuse_operations* get_fuse_xmp_oper();

}
}
