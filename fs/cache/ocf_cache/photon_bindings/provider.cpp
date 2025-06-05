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

/*
 * Note: Some OCF APIs are not being used under current circumstances, for example:
 *      ocf_mngt_cache_lock
 *      ocf_mngt_cache_unlock
 *      ocf_mngt_cache_flush
 *      ocf_mngt_core_flush
 *      ocf_mngt_cache_remove_core
 * They might be added in the future, for advanced usages.
 */

#include "provider.h"

#include <photon/common/alog.h>


extern IOAlloc *g_io_alloc;

/* Callbacks - Start */

struct simple_context {
    int error = 0;
    photon::semaphore sem;
};

static void simple_complete(ocf_cache_t cache, void *priv, int error) {
    auto context = (simple_context *)priv;
    context->error = error;
    context->sem.signal(1);
}

struct add_core_context : public simple_context {
    ocf_core_t core = nullptr;
};

static void add_core_complete(ocf_cache_t cache, ocf_core_t core, void *priv, int error) {
    auto context = (add_core_context *)priv;
    context->core = core;
    context->error = error;
    context->sem.signal(1);
}

static void read_complete(ocf_io *io, int error) {
    auto data = (ease_ocf_io_data *)ocf_io_get_data(io);
    if (error != 0) {
        auto error_ptr = (int *)io->priv2;
        *error_ptr = error;
    }
    ocf_io_put(io);
    data->sem.signal(1);
}

/* Callbacks - End */

const size_t ease_ocf_provider::SectorSize = 512;

int ease_ocf_provider::start(bool reload_media) {
    /* Create context */
    auto ctx_cfg = get_context_config();
    int ret = ocf_ctx_create(&m_ctx, ctx_cfg);
    if (ret != 0) {
        return ret;
    }

    /* Register volume */
    ret = volume_init(m_ctx);
    if (ret != 0) {
        ocf_ctx_put(m_ctx);
        return ret;
    }

    /* Configurations */
    ocf_mngt_cache_config_set_default(&m_cfg.cache);
    ocf_mngt_cache_device_config_set_default(&m_cfg.device);
    ocf_mngt_core_config_set_default(&m_cfg.core);

    m_cfg.cache.cache_mode = ocf_cache_mode_wt;
    m_cfg.device.discard_on_start = false;
    m_cfg.device.perform_test = false;
    m_cfg.device.volume_params = m_volume_params;
    m_cfg.device.cache_line_size = (ocf_cache_line_size_t)m_volume_params->blk_size;
    m_cfg.cache.cache_line_size = (ocf_cache_line_size_t)m_volume_params->blk_size;

    strncpy(m_cfg.cache.name, CACHE_NAME, sizeof(m_cfg.cache.name));
    strncpy(m_cfg.core.name, CORE_NAME, sizeof(m_cfg.core.name));

    ocf_uuid_set_str(&m_cfg.device.uuid, (char *)CACHE_UUID);
    ocf_uuid_set_str(&m_cfg.core.uuid, (char *)CORE_UUID);

    m_cfg.device.volume_type = PHOTON_OCF_VOLUME_TYPE;
    m_cfg.core.volume_type = PHOTON_OCF_VOLUME_TYPE;

    /* Start cache */
    ret = ocf_mngt_cache_start(m_ctx, &m_cache, &m_cfg.cache, nullptr);
    if (ret != 0) {
        LOG_ERROR("OCF: failed to start cache");
        return ret;
    }

    /* Allocate and assign cache private data */
    m_queue = new ease_ocf_queue{};
    ocf_cache_set_priv(m_cache, m_queue);

    /* Create management queue */
    simple_context simple_ctx;
    ret = ocf_queue_create(m_cache, &m_queue->mngt_queue, get_queue_ops());
    if (ret != 0) {
        LOG_ERROR("OCF: failed to create management queue");
        return ret;
    }
    ocf_mngt_cache_set_mngt_queue(m_cache, m_queue->mngt_queue);

    /* Create IO submission queue */
    ret = ocf_queue_create(m_cache, &m_queue->io_queue, get_queue_ops());
    if (ret != 0) {
        LOG_ERROR("OCF: failed to create io queue");
        return ret;
    }

    init_queues(m_queue->mngt_queue, m_queue->io_queue);

    if (reload_media) {
        /* Reload cache instance */
        ocf_mngt_cache_load(m_cache, &m_cfg.device, simple_complete, &simple_ctx);
    } else {
        /* Attach cache device to a new cache instance */
        ocf_mngt_cache_attach(m_cache, &m_cfg.device, simple_complete, &simple_ctx);
    }

    simple_ctx.sem.wait(1);
    if (simple_ctx.error != 0) {
        LOG_ERROR("OCF: failed to reload/attach cache");
        return simple_ctx.error;
    }

    if (reload_media) {
        /* Core will be auto opened when reloading cache */
        ret = ocf_core_get_by_name(m_cache, CORE_NAME, strlen(CORE_NAME), &m_core);
        if (ret != 0) {
            LOG_ERROR_RETURN(0, ret, "OCF: get core by name failed");
        }

    } else {
        /* Add new core to cache */
        add_core_context add_core_ctx;
        ocf_mngt_cache_add_core(m_cache, &m_cfg.core, add_core_complete, &add_core_ctx);

        add_core_ctx.sem.wait(1);
        if (add_core_ctx.error != 0) {
            LOG_ERROR("OCF: failed to add core to cache");
            return add_core_ctx.error;
        } else {
            m_core = add_core_ctx.core;
        }
    }
    m_volume_params->enable_logging = true;
    LOG_INFO("OCF: OCF cache is ready, blk_size `, prefetch_unit `", m_volume_params->blk_size,
             m_prefetch_unit);
    return 0;
}

int ease_ocf_provider::stop() {
    LOG_INFO("OCF: OCF is going to stop ...");
    m_volume_params->enable_logging = false;
    if (m_cache != nullptr) {
        simple_context simple_ctx;
        ocf_mngt_cache_stop(m_cache, simple_complete, &simple_ctx);

        simple_ctx.sem.wait(1);
        if (simple_ctx.error != 0) {
            LOG_ERROR_RETURN(0, -1, "OCF: failed to stop management cache, error = `",
                             simple_ctx.error);
        } else {
            LOG_DEBUG("OCF: succeeded to stop management cache");
        }
    }

    if (m_queue != nullptr) {
        ocf_queue_put(m_queue->mngt_queue);
        LOG_DEBUG("OCF: done put management queue");
    }

    delete m_queue;

    if (m_ctx != nullptr) {
        volume_cleanup(m_ctx);
        ocf_ctx_put(m_ctx);
        LOG_DEBUG("OCF: succeeded to destroy volume and context");
    }
    LOG_INFO("OCF: OCF cache is fully stopped ...");
    return 0;
}

void ease_ocf_provider::prepare_aligned_iov(size_t count, off_t offset, alignment &a, IOVector &iov,
                                            const void *buf, void *padding_buf) {
    // bound
    a.lower_bound = ROUND_DOWN(offset, SectorSize);
    a.upper_bound = ROUND_UP(offset + count, SectorSize);

    // padding
    if (offset > a.lower_bound) {
        a.lower_padding = offset - a.lower_bound;
    }
    if (offset + count < (size_t)a.upper_bound) {
        a.upper_padding = a.upper_bound - offset - count;
    }

    // middle
    if (a.lower_padding != 0 && a.upper_padding != 0 &&
        a.upper_bound - a.lower_bound != (off_t)SectorSize) {
        a.mid_count = ROUND_DOWN(a.upper_bound - a.lower_bound, SectorSize) - 2 * SectorSize;
    } else {
        a.mid_count = ROUND_DOWN(count, SectorSize);
    }

    if (a.upper_bound - a.lower_bound == (off_t)SectorSize) {
        // within one sector
        iov.push_back(padding_buf, SectorSize);
    } else {
        // across multiple sectors
        if (a.lower_padding != 0) {
            // assign lower to iov
            iov.push_back(padding_buf, SectorSize);
        }
        if (a.mid_count != 0) {
            // assign middle sectors to iov
            auto cur = (uint8_t *)buf;
            if (a.lower_padding != 0) {
                cur += SectorSize - a.lower_padding;
            }
            iov.push_back(cur, a.mid_count);
        }
        if (a.upper_padding != 0) {
            // assign upper to iov
            iov.push_back((uint8_t *)padding_buf + SectorSize, SectorSize);
        }
    }
    LOG_DEBUG("bound `-`, padding `-`, mid `, iov cnt `, size `", a.lower_bound, a.upper_bound,
              a.lower_padding, a.upper_padding, a.mid_count, iov.iovcnt(), iov.sum());
}

void ease_ocf_provider::copy_aligned_iov(size_t count, alignment &a, IOVector &iov, void *buf) {
    if (a.upper_bound - a.lower_bound == (off_t)SectorSize) {
        // within one sector
        if (a.lower_padding != 0) {
            iov.extract_front(a.lower_padding);
        }
        iov.memcpy_to(buf, count);
    } else {
        // across multiple sectors
        auto cur = (uint8_t *)buf;
        if (a.lower_padding != 0) {
            // copy lower padding to buf
            iov.extract_front(a.lower_padding);
            iov.extract_front(SectorSize - a.lower_padding, buf);
            cur += SectorSize - a.lower_padding;
        }
        if (a.upper_padding != 0) {
            // skip middle, and copy upper padding to buf
            if (a.mid_count > 0) {
                cur += a.mid_count;
                iov.extract_front(a.mid_count);
            }
            iov.extract_front(SectorSize - a.upper_padding, cur);
        }
    }
}

ssize_t ease_ocf_provider::ocf_pread(void *buf, size_t count, off_t offset, size_t blk_addr,
                                     OcfSrcFileCtx *ctx, bool prefetch) {
    LOG_DEBUG("New IO: pread buf `, count `, offset `", buf, count, offset);
    int error = 0;
    IOVector iov;
    alignment align{};
    void *padding_buf = nullptr;

    /*
     * The padding_buf is used to store bilateral paddings.
     * A not-null padding_buf indicates alignment is required.
     */
    if (offset % SectorSize != 0 || count % SectorSize != 0) {
        padding_buf = g_io_alloc->alloc(SectorSize * 2);
        if (padding_buf == nullptr) {
            LOG_ERRNO_RETURN(ENOMEM, -1, "OCF: failed to allocate padding buf");
        }
        prepare_aligned_iov(count, offset, align, iov, buf, padding_buf);
    } else {
        iov.push_back(buf, count);
        align.lower_bound = offset;
    }

    DEFER({
        if (padding_buf != nullptr)
            g_io_alloc->dealloc(padding_buf);
    });

    /* Create data */
    ease_ocf_io_data data(iov.iovec(), iov.iovcnt(), iov.sum(), blk_addr, ctx, prefetch);

    /* Create io */
    ocf_io *io = ocf_core_new_io(m_core, m_queue->io_queue, data.blk_addr + align.lower_bound,
                                 (uint32_t)iov.sum(), OCF_READ, 0, 0);
    if (io == nullptr) {
        LOG_ERRNO_RETURN(ENOMEM, -1, "OCF: failed to create new IO, count `, offset `, blk_addr `",
                         iov.sum(), align.lower_bound, blk_addr);
    }

    /* Set data by volume's interface, offset should be 0 */
    ocf_io_set_data(io, &data, 0);
    /* Setup completion function */
    ocf_io_set_cmpl(io, nullptr, &error, read_complete);
    /* Submit io */
    ocf_core_submit_io(io);

    /* Wait IO finished and check error */
    data.sem.wait(1);
    if (error != 0) {
        errno = data.err_no;
        LOG_ERRNO_RETURN(0, -1, "OCF: IO error");
    }

    if (padding_buf != nullptr) {
        copy_aligned_iov(count, align, iov, buf);
    }

    LOG_DEBUG("Finish IO: pread buf `, count `, offset `", buf, count, offset);
    return count;
}
