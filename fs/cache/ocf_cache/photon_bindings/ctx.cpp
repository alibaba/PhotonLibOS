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

#include "ctx.h"

#include <photon/common/alog.h>
#include <photon/fs/localfs.h>
#include <photon/common/io-alloc.h>

extern "C" {
#include "env/ocf_env.h"
}

IOAlloc *g_io_alloc = nullptr;

static size_t iovec_to_buf(iovec *iov, size_t iovcnt, void *buf, size_t size, size_t offset) {
    size_t i, len, done = 0;
    for (i = 0; i < iovcnt; i++) {
        if (offset >= iov[i].iov_len) {
            offset -= iov[i].iov_len;
            continue;
        }
        if (iov[i].iov_base == nullptr) {
            continue;
        }
        if (done >= size) {
            break;
        }
        len = MIN(size - done, iov[i].iov_len - offset);
        memcpy(buf, (uint8_t *)iov[i].iov_base + offset, len);
        buf = (uint8_t *)buf + len;
        done += len;
        offset = 0;
    }
    return done;
}

static size_t buf_to_iovec(const void *buf, size_t size, iovec *iov, size_t iovcnt, size_t offset) {
    size_t i, len, done = 0;
    for (i = 0; i < iovcnt; i++) {
        if (offset >= iov[i].iov_len) {
            offset -= iov[i].iov_len;
            continue;
        }
        if (iov[i].iov_base == nullptr) {
            continue;
        }
        if (done >= size) {
            break;
        }
        len = MIN(size - done, iov[i].iov_len - offset);
        memcpy((uint8_t *)iov[i].iov_base + offset, buf, len);
        buf = (uint8_t *)buf + len;
        done += len;
        offset = 0;
    }
    return done;
}

static size_t iovset(iovec *iov, size_t iovcnt, int byte, size_t size, size_t offset) {
    size_t i, len, done = 0;
    for (i = 0; i < iovcnt; i++) {
        if (offset >= iov[i].iov_len) {
            offset -= iov[i].iov_len;
            continue;
        }
        if (iov[i].iov_base == nullptr) {
            continue;
        }
        if (done >= size) {
            break;
        }
        len = MIN(size - done, iov[i].iov_len - offset);
        memset((uint8_t *)iov[i].iov_base + offset, byte, len);
        done += len;
        offset = 0;
    }
    return done;
}

/* Context config */

static ctx_data_t *ctx_data_alloc(uint32_t pages) {
    size_t buf_size = PAGE_SIZE * pages;
    auto data = new ease_ocf_io_data();
    data->iovs = (iovec *)g_io_alloc->alloc(sizeof(iovec));
    if (data->iovs == nullptr) {
        delete data;
        LOG_ERRNO_RETURN(ENOMEM, nullptr, "OCF: failed to allocate iov");
    }

    void *buf = g_io_alloc->alloc(buf_size);
    if (buf == nullptr) {
        LOG_ERRNO_RETURN(ENOMEM, nullptr, "OCF: failed to allocate memory, size `",
                         PAGE_SIZE * pages);
    }

    data->iovs[0].iov_base = buf;
    data->iovs[0].iov_len = buf_size;
    data->iovcnt = 1;
    data->size = buf_size;

    LOG_DEBUG("OCF: ctx data alloc: buf `, size ", buf, buf_size);
    return data;
}

static void ctx_data_free(ctx_data_t *ctx_data) {
    if (ctx_data == nullptr) {
        return;
    }
    auto data = (ease_ocf_io_data *)ctx_data;
    for (int i = 0; i < data->iovcnt; i++) {
        if (data->iovs[i].iov_base) {
            LOG_DEBUG("OCF: ctx data free: buf `", data->iovs[i].iov_base);
            g_io_alloc->dealloc(data->iovs[i].iov_base);
            data->iovs[i].iov_base = nullptr;
        }
    }
    if (data->iovs) {
        g_io_alloc->dealloc(data->iovs);
        data->iovs = nullptr;
    }
    delete data;
}

static int ctx_data_mlock(ctx_data_t *ctx_data) {
    return 0;
}

static void ctx_data_munlock(ctx_data_t *ctx_data) {
}

static uint32_t ctx_data_read(void *dst, ctx_data_t *src, uint32_t size) {
    auto data = (ease_ocf_io_data *)src;
    LOG_DEBUG("OCF: ctx data read: buf `, seek_offset `", data->iovs[0].iov_base, data->seek);
    uint32_t n_read = iovec_to_buf(data->iovs, data->iovcnt, dst, size, data->seek);
    data->seek += n_read;
    return n_read;
}

static uint32_t ctx_data_write(ctx_data_t *dst, const void *src, uint32_t size) {
    auto data = (ease_ocf_io_data *)dst;
    LOG_DEBUG("OCF: ctx data write: buf `, seek_offset `", data->iovs[0].iov_base, data->seek);
    uint32_t n_written = buf_to_iovec(src, size, data->iovs, data->iovcnt, data->seek);
    data->seek += n_written;
    return n_written;
}

static uint32_t ctx_data_zero(ctx_data_t *dst, uint32_t size) {
    auto data = (ease_ocf_io_data *)dst;
    LOG_DEBUG("OCF: ctx data zero: buf `, seek_offset `", data->iovs[0].iov_base, data->seek);
    uint32_t n_bytes = iovset(data->iovs, data->iovcnt, 0, size, data->seek);
    data->seek += n_bytes;
    return n_bytes;
}

static uint32_t ctx_data_seek(ctx_data_t *dst, ctx_data_seek_t seek, uint32_t offset) {
    auto data = (ease_ocf_io_data *)dst;
    uint32_t off = 0;
    LOG_DEBUG("OCF: ctx data seek: buf `, current seek_offset `, new seek `, new seek_offset `",
              data->iovs[0].iov_base, data->seek, seek, offset);
    switch (seek) {
    case ctx_data_seek_begin:
        off = MIN(offset, data->size);
        data->seek = off;
        break;
    case ctx_data_seek_current:
        off = MIN(offset, data->size - data->seek);
        data->seek += off;
        break;
    }
    return off;
}

inline static uint64_t ctx_data_copy(ctx_data_t *dst, ctx_data_t *src, uint64_t to, uint64_t from,
                                     uint64_t bytes) {
    auto data_dst = (ease_ocf_io_data *)dst;
    auto data_src = (ease_ocf_io_data *)src;
    bytes = MIN(bytes, data_src->size - from);
    bytes = MIN(bytes, data_dst->size - to);
    LOG_DEBUG(
        "OCF: ctx data copy: src buf `, src seek_offset `, dst buf `, dst seek_offset `, copy ` bytes",
        data_src->iovs[0].iov_base, data_src->seek, data_dst->iovs[0].iov_base, data_dst->seek,
        bytes);

    uint32_t need_copy = bytes;
    uint32_t iter_iov = 0, iter_offset = 0, n = 0;

    while (from || bytes) {
        if (data_src->iovs[iter_iov].iov_len == iter_offset) {
            iter_iov++;
            iter_offset = 0;
            continue;
        }
        if (from) {
            n = MIN(from, data_src->iovs[iter_iov].iov_len);
            from -= n;
        } else {
            n = MIN(bytes, data_src->iovs[iter_iov].iov_len);
            buf_to_iovec((uint8_t *)data_src->iovs[iter_iov].iov_base + iter_offset, n,
                         data_dst->iovs, data_dst->iovcnt, to);
            bytes -= n;
            to += n;
        }
        iter_offset += n;
    }
    return need_copy;
}

static void ctx_data_secure_erase(ctx_data_t *ctx_data) {
}

static int ctx_cleaner_init(ocf_cleaner_t c) {
    return 0;
}

static void ctx_cleaner_kick(ocf_cleaner_t c) {
}

static void ctx_cleaner_stop(ocf_cleaner_t c) {
}

static int ctx_logger_print(ocf_logger_t logger, ocf_logger_lvl_t lvl, const char *fmt,
                            va_list args) {
    char buf[512];
    int ret = vsprintf(buf, fmt, args);
    DEFER(LOG_DEBUG(buf));
    if (ret > 0 && ret <= (int)sizeof(buf) && buf[ret - 1] == '\n') {
        buf[ret - 1] = 0;
        return ret - 1;
    } else {
        return ret;
    }
}

static const ocf_ctx_config ctx_cfg = {
    .name = "Context Config",
    .ops =
        ocf_ctx_ops{
            .data =
                ocf_data_ops{
                    .alloc = ctx_data_alloc,
                    .free = ctx_data_free,
                    .mlock = ctx_data_mlock,
                    .munlock = ctx_data_munlock,
                    .read = ctx_data_read,
                    .write = ctx_data_write,
                    .zero = ctx_data_zero,
                    .seek = ctx_data_seek,
                    .copy = ctx_data_copy,
                    .secure_erase = ctx_data_secure_erase,
                },

            .cleaner =
                ocf_cleaner_ops{
                    .init = ctx_cleaner_init,
                    .kick = ctx_cleaner_kick,
                    .stop = ctx_cleaner_stop,
                },

            .logger =
                ocf_logger_ops{
                    .open = nullptr,
                    .close = nullptr,
                    .print = ctx_logger_print,
                    .print_rl = nullptr,
                    .dump_stack = nullptr,
                },
        },
    .logger_priv = nullptr,
};

const ocf_ctx_config *get_context_config() {
    return &ctx_cfg;
}
