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
#include <photon/common/iovector.h>

#include "ctx.h"
#include "queue.h"
#include "volume.h"

class ease_ocf_provider {
public:
    ease_ocf_provider(ease_ocf_volume_params *params, size_t prefetch_unit)
        : m_volume_params(params), m_prefetch_unit(prefetch_unit) {
    }

    int start(bool reload_media);

    int stop();

    /**
     * @brief Caller should guarantee that offset + count does not exceed the EOF.
     * @return On success, count is returned. On error, other value is returned.
     */
    ssize_t ocf_pread(void *buf, size_t count, off_t offset, size_t blk_addr, OcfSrcFileCtx *ctx,
                      bool prefetch = false);

    size_t prefetch_unit() const {
        return m_prefetch_unit;
    }

    static const size_t SectorSize;

private:
    static constexpr const char *CACHE_NAME = "Ease Cache";
    static constexpr const char *CORE_NAME = "Ease Core";
    static constexpr const char *CACHE_UUID = "cache";
    static constexpr const char *CORE_UUID = "core";

    /* Main control context */
    ocf_ctx_t m_ctx = nullptr;

    /* Cache and Core objects */
    ocf_cache_t m_cache = nullptr;
    ocf_core_t m_core = nullptr;

    /* Configurations */
    ease_ocf_config m_cfg = {};

    /* Queue */
    ease_ocf_queue *m_queue = nullptr;

    /* Volume parameters */
    ease_ocf_volume_params *m_volume_params; // owned by external class

    size_t m_prefetch_unit;

    /*
     *       |                       |                       |                       |
     *       |                       |                       |                       |
     *       |< lower_padding >|     |<      mid_count      >|     |< upper_padding >|
     *   ----|-----------------------|-----[sectors ...]-----|-----------------------|----
     *       |                 |                                   |                 |
     *  lower_bound            |                                   |            upper_bound
     *                       offset                         offset + count
     */
    struct alignment {
        off_t lower_bound;
        size_t lower_padding;
        size_t mid_count;
        size_t upper_padding;
        off_t upper_bound;
    };

    static void prepare_aligned_iov(size_t count, off_t offset, alignment &a, IOVector &iov,
                                    const void *buf, void *padding_buf);

    static void copy_aligned_iov(size_t count, alignment &a, IOVector &iov, void *buf);
};