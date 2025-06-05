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

#include "volume.h"

#include <string>

#include <photon/common/alog.h>
#include <photon/common/alog-stdstring.h>
#include <photon/common/alog-audit.h>
#include <photon/fs/localfs.h>
#include <photon/common/io-alloc.h>
#include <photon/common/iovector.h>
#include <photon/thread/thread11.h>

#include "provider.h"
#include "ctx.h"

extern IOAlloc *g_io_alloc;

struct ease_ocf_volume_io {
    ease_ocf_io_data *data;
    off_t offset;
};

// Cache volume and core volume share the same struct, uuid string may be cache or core.
// Cache volume has extra parameters used in `volume_open`.
struct ease_ocf_volume {
    ocf_volume_uuid uuid;
    ease_ocf_volume_params *cache_params;
};

static int prefetch_read(OcfSrcFileCtx *src_file_ctx, size_t count, off_t offset, off_t blk_addr) {
    off_t new_offset = offset + count;
    size_t new_count = src_file_ctx->provider->prefetch_unit();
    if (new_count == 0 || (size_t)new_offset >= src_file_ctx->ns_info.file_size) {
        return 0;
    } else if (new_offset + new_count > src_file_ctx->ns_info.file_size) {
        new_count =
            ROUND_UP(src_file_ctx->ns_info.file_size, ease_ocf_provider::SectorSize) - new_offset;
    }

    LOG_DEBUG("OCF: Prefetch pread, count `, offset `", new_count, new_offset);
    void *buf = g_io_alloc->alloc(new_count);
    DEFER(g_io_alloc->dealloc(buf));
    auto ret =
        src_file_ctx->provider->ocf_pread(buf, new_count, new_offset, blk_addr, src_file_ctx);
    if (ret <= 0) {
        LOG_ERRNO_RETURN(0, -1, "OCF: prefetch read failed")
    }
    return 0;
}

static int volume_open(ocf_volume_t volume, void *volume_params) {
    auto uuid = ocf_volume_get_uuid(volume);
    auto vol = (ease_ocf_volume *)ocf_volume_get_priv(volume);
    vol->uuid = *uuid;

    if (strncmp((const char *)vol->uuid.data, "core", vol->uuid.size) == 0) {
        // For now the volume_open of core will always receive a nullptr volume_params, so blk_size
        // can only be known from io_data. Maybe OCF will change it in the future.
        assert(volume_params == nullptr);
    }
    if (volume_params != nullptr) {
        vol->cache_params = (ease_ocf_volume_params *)volume_params;
    }
    return 0;
}

// Do noting here. Let the upper caller to release resources
static void volume_close(ocf_volume_t volume) {
}

static void volume_submit_io(struct ocf_io *io) {
    auto vol = (ease_ocf_volume *)ocf_volume_get_priv(ocf_io_get_volume(io));
    auto vol_io = (ease_ocf_volume_io *)ocf_io_get_priv(io);

    IOVector local_iov(vol_io->data->iovs, vol_io->data->iovcnt);
    if (vol_io->offset > 0) {
        local_iov.extract_front(vol_io->offset);
    }
    if ((size_t)io->bytes < local_iov.sum()) {
        local_iov.truncate(io->bytes);
    }

    ssize_t ret;
    if (strncmp((const char *)vol->uuid.data, "cache", vol->uuid.size) == 0) {
        if (vol->cache_params->enable_logging) {
            LOG_DEBUG("OCF cache `, buf `, count: `, offset: `",
                      io->dir == OCF_READ ? "read" : "write", local_iov.iovec()[0].iov_base,
                      local_iov.sum(), io->addr);
        }
        auto media_file = vol->cache_params->media_file;
        if (io->dir == OCF_WRITE) {
            ret = media_file->pwritev(local_iov.iovec(), local_iov.iovcnt(), io->addr);
        } else {
            ret = media_file->preadv(local_iov.iovec(), local_iov.iovcnt(), io->addr);
        }

    } else {
        auto src_file = vol_io->data->ctx;
        if (vol_io->offset != 0 || src_file == nullptr) {
            LOG_ERROR("OCF: core read with non-zero offset or null src_file, must be a bug");
            vol_io->data->err_no = EINVAL;
            io->end(io, -1);
        }
        off_t offset = io->addr - vol_io->data->blk_addr;
        LOG_DEBUG("OCF core `, buf `, count: `, offset: `", io->dir == OCF_READ ? "read" : "write",
                  local_iov.iovec()[0].iov_base, local_iov.sum(), offset);
        if (io->dir == OCF_WRITE) {
            ret = -1;
            errno = ENOSYS;
        } else {
            if (vol_io->data->prefetch) {
                photon::thread_create11(prefetch_read, vol_io->data->ctx, local_iov.sum(), offset,
                                        vol_io->data->blk_addr);
            }
            SCOPE_AUDIT("download", AU_FILEOP(src_file->path, offset, ret));
            ret = src_file->src_file->preadv(local_iov.iovec(), local_iov.iovcnt(), offset);
        }
    }

    if (ret < 0) {
        vol_io->data->err_no = errno;
        io->end(io, -1);
    } else if (ret == 0) {
        LOG_ERROR("OCF: EOF should have been validated, must be a bug !");
        vol_io->data->err_no = EINVAL;
        io->end(io, -1);
    } else {
        io->end(io, 0);
    }
}

/*
 * We don't need to implement submit_flush(). Just complete io with success.
 */
static void volume_submit_flush(struct ocf_io *io) {
    io->end(io, 0);
}

/*
 * We don't need to implement submit_discard(). Just complete io with success.
 */
static void volume_submit_discard(struct ocf_io *io) {
    io->end(io, 0);
}

static unsigned int volume_get_max_io_size(ocf_volume_t volume) {
    return MiB;
}

static uint64_t volume_get_length(ocf_volume_t volume) {
    auto vol = (ease_ocf_volume *)ocf_volume_get_priv(volume);
    auto uuid = ocf_volume_get_uuid(volume);
    if (strncmp((const char *)uuid->data, "cache", uuid->size) == 0) {
        return vol->cache_params->media_size;
    } else if (strncmp((const char *)uuid->data, "core", uuid->size) == 0) {
        return UINT64_MAX;
    }
    return 0;
}

static int volume_io_set_data(ocf_io *io, ctx_data_t *data, uint32_t offset) {
    auto vol_io = (ease_ocf_volume_io *)ocf_io_get_priv(io);
    vol_io->data = (ease_ocf_io_data *)data;
    vol_io->offset = offset;
    return 0;
}

static ctx_data_t *volume_io_get_data(ocf_io *io) {
    auto vol_io = (ease_ocf_volume_io *)ocf_io_get_priv(io);
    return vol_io->data;
}

/*
 * OCF uses a volume interface for accessing BOTH backend storage and cache storage
 */
const struct ocf_volume_properties volume_properties = {
    .name = "OCF Volume",
    .io_priv_size = sizeof(ease_ocf_volume_io),
    .volume_priv_size = sizeof(struct ease_ocf_volume),
    .caps =
        {
            .atomic_writes = 0,
        },
    .io_ops =
        {
            .set_data = volume_io_set_data,
            .get_data = volume_io_get_data,
        },
    .deinit = nullptr,
    .ops =
        {
            .submit_io = volume_submit_io,
            .submit_flush = volume_submit_flush,
            .submit_metadata = nullptr,
            .submit_discard = volume_submit_discard,
            .submit_write_zeroes = nullptr,
            .open = volume_open,
            .close = volume_close,
            .get_length = volume_get_length,
            .get_max_io_size = volume_get_max_io_size,
        },
};

int volume_init(ocf_ctx_t ocf_ctx) {
    return ocf_ctx_register_volume_type(ocf_ctx, PHOTON_OCF_VOLUME_TYPE, &volume_properties);
}

void volume_cleanup(ocf_ctx_t ocf_ctx) {
    ocf_ctx_unregister_volume_type(ocf_ctx, PHOTON_OCF_VOLUME_TYPE);
}
