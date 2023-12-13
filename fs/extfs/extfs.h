/*
Copyright 2023 The Photon Authors

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
#include <photon/fs/filesystem.h>

namespace photon {
namespace fs {

photon::fs::IFileSystem *new_extfs(photon::fs::IFile *file, bool buffer = true);

// make extfs on an prezeroed IFile,
// should be truncated to specified size in advance
int make_extfs(photon::fs::IFile *file, char *uuid = nullptr);

}
}
