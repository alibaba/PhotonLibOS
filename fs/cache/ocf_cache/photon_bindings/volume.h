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

#include <photon/fs/filesystem.h>

extern "C" {
#include <ocf/ocf.h>
}

#define PHOTON_OCF_VOLUME_TYPE 1

struct ease_ocf_volume_params {
    size_t blk_size;
    size_t media_size;
    photon::fs::IFile *media_file;
    bool enable_logging;
};

int volume_init(ocf_ctx_t ocf_ctx);

void volume_cleanup(ocf_ctx_t ocf_ctx);
