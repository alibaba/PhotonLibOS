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
#include "extfs.h"
#include "extfs_io.h"
#include <ext2fs/ext2fs.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <photon/common/alog.h>
#include <photon/common/callback.h>
#include <photon/common/utility.h>
#include <photon/fs/filesystem.h>

static errcode_t extfs_open(const char *name, int flags, io_channel *channel);
static errcode_t extfs_close(io_channel channel);
static errcode_t extfs_set_blksize(io_channel channel, int blksize);
static errcode_t extfs_read_blk(io_channel channel, unsigned long block, int count, void *buf);
static errcode_t extfs_read_blk64(io_channel channel, unsigned long long block, int count, void *buf);
static errcode_t extfs_write_blk(io_channel channel, unsigned long block, int count, const void *buf);
static errcode_t extfs_write_blk64(io_channel channel, unsigned long long block, int count, const void *buf);
static errcode_t extfs_flush(io_channel channel);
static errcode_t extfs_discard(io_channel channel, unsigned long long block, unsigned long long count);
static errcode_t extfs_cache_readahead(io_channel channel, unsigned long long block, unsigned long long count);
static errcode_t extfs_zeroout(io_channel channel, unsigned long long block, unsigned long long count);

namespace {

constexpr char kDevPrefix[] = "extfs:";
constexpr size_t kDevNameSize = 32;    // "extfs:" + 16 hex digits + '\0'

// The device name encodes IFile*, so each open is unique and extfs_open() can
// recover the backend without any state shared across calls.
void format_dev_name(photon::fs::IFile *file, char *buf, size_t size) {
    snprintf(buf, size, "%s%llx", kDevPrefix, (unsigned long long)(uintptr_t)file);
}

photon::fs::IFile *file_from_dev_name(const char *name) {
    if (!name) return nullptr;
    constexpr size_t plen = sizeof(kDevPrefix) - 1;
    if (strncmp(name, kDevPrefix, plen) != 0) return nullptr;
    char *end = nullptr;
    auto addr = strtoull(name + plen, &end, 16);
    if (addr == 0 || !end || *end != '\0') return nullptr;
    return reinterpret_cast<photon::fs::IFile *>(addr);
}

}  // namespace

struct unix_private_data {
    int magic;
    int dev;
    int flags;
    int align;
    int access_time;
    ext2_loff_t offset;
    void *bounce;
    struct struct_io_stats io_stats;

    // per-channel state, bound at open and released at close
    photon::fs::IFile *file;
    char *zero_buf;                 // lazily allocated, private to this channel
    size_t zero_stride;             // in blocks
};

// Read-only after init: the manager holds no per-open state; the context is
// passed through the device name.
static struct_io_manager extfs_io_manager = {
    .magic              = EXT2_ET_MAGIC_IO_MANAGER,
    .name               = "extfs I/O Manager",
    .open               = extfs_open,
    .close              = extfs_close,
    .set_blksize        = extfs_set_blksize,
    .read_blk           = extfs_read_blk,
    .write_blk          = extfs_write_blk,
    .flush              = extfs_flush,
    .write_byte         = nullptr,
    .set_option         = nullptr,
    .get_stats          = nullptr,
    .read_blk64         = extfs_read_blk64,
    .write_blk64        = extfs_write_blk64,
    .discard            = extfs_discard,
    .cache_readahead    = extfs_cache_readahead,
    .zeroout            = extfs_zeroout,
    .reserved           = {},
};

static errcode_t extfs_open(const char *name, int flags, io_channel *channel) {
    auto file = file_from_dev_name(name);
    if (!file) {
        LOG_ERROR("invalid extfs device name `, expected `<hex IFile*>", name, kDevPrefix);
        return EXT2_ET_BAD_DEVICE_NAME;
    }

    io_channel io = nullptr;
    errcode_t ret = ext2fs_get_mem(sizeof(*io), &io);
    if (ret) return ret;
    memset(io, 0, sizeof(*io));

    bool ok = false;
    DEFER({
        if (!ok) {
            ext2fs_free_mem(&io->private_data);
            ext2fs_free_mem(&io->name);
            ext2fs_free_mem(&io);
        }
    });

    struct unix_private_data *data = nullptr;
    ret = ext2fs_get_mem(sizeof(*data), &data);
    if (ret) return ret;
    memset(data, 0, sizeof(*data));
    data->magic = EXT2_ET_MAGIC_UNIX_IO_CHANNEL;
    data->io_stats.num_fields = 2;
    data->flags = flags;
    data->file = file;
    io->private_data = data;

    ret = ext2fs_get_mem(strlen(name) + 1, &io->name);
    if (ret) return ret;
    strcpy(io->name, name);

    io->magic = EXT2_ET_MAGIC_IO_CHANNEL;
    io->manager = &extfs_io_manager;
    io->block_size = 1024;          // overridden by extfs_set_blksize()
    io->refcount = 1;

    ok = true;
    *channel = io;
    LOG_DEBUG("extfs opened, `", name);
    return 0;
}

static errcode_t extfs_close(io_channel channel) {
    if (--channel->refcount > 0)
        return 0;
    auto data = reinterpret_cast<unix_private_data *>(channel->private_data);
    if (data) {
        free(data->zero_buf);
        ext2fs_free_mem(&channel->private_data);
    }
    if (channel->name)
        ext2fs_free_mem(&channel->name);
    LOG_INFO("extfs close");
    return ext2fs_free_mem(&channel);
}

static errcode_t extfs_set_blksize(io_channel channel, int blksize) {
    LOG_DEBUG(VALUE(blksize));
    channel->block_size = blksize;
    return 0;
}

static errcode_t extfs_read_blk(io_channel channel, unsigned long block, int count, void *buf) {
    auto data = reinterpret_cast<unix_private_data *>(channel->private_data);
    off_t offset = (ext2_loff_t)block * channel->block_size;
    ssize_t size = count < 0 ? -count : (ext2_loff_t)count * channel->block_size;
    auto res = data->file->pread(buf, size, offset);
    if (res == size)
        return 0;
    LOG_ERROR("failed to pread, got `, expect `", res, size);
    return errno ? errno : EIO;
}

static errcode_t extfs_read_blk64(io_channel channel, unsigned long long block, int count, void *buf) {
    return extfs_read_blk(channel, block, count, buf);
}

static errcode_t extfs_write_blk(io_channel channel, unsigned long block, int count, const void *buf) {
    auto data = reinterpret_cast<unix_private_data *>(channel->private_data);
    off_t offset = (ext2_loff_t)block * channel->block_size;
    ssize_t size = count < 0 ? -count : (ext2_loff_t)count * channel->block_size;
    auto res = data->file->pwrite(buf, size, offset);
    if (res == size)
        return 0;
    LOG_ERROR("failed to pwrite, got `, expect `", res, size);
    return errno ? errno : EIO;
}

static errcode_t extfs_write_blk64(io_channel channel, unsigned long long block, int count, const void *buf) {
    return extfs_write_blk(channel, block, count, buf);
}

static errcode_t extfs_flush(io_channel channel) {
    return 0;
}

static errcode_t extfs_discard(io_channel channel, unsigned long long block, unsigned long long count) {
    return 0;
}

static errcode_t extfs_cache_readahead(io_channel channel, unsigned long long block, unsigned long long count) {
    return 0;
}

// Real zero-fill. Once ext2fs_zero_blocks2() gets 0 back it returns immediately
// without allocating libext2fs' process-wide static buffer, making the cleanup
// in ext2fs_free() a no-op; this is what keeps concurrent images free of
// cross-image use-after-free. The buffer is private to this channel.
static errcode_t extfs_zeroout(io_channel channel, unsigned long long block, unsigned long long count) {
    if (count == 0)
        return 0;
    auto data = reinterpret_cast<unix_private_data *>(channel->private_data);
    size_t bs = channel->block_size;
    if (bs == 0)
        return EINVAL;

    const size_t kMaxStrideBytes = 1UL << 20;
    size_t stride = kMaxStrideBytes / bs ? kMaxStrideBytes / bs : 1;
    if (count < stride)
        stride = count;
    if (data->zero_stride < stride) {
        free(data->zero_buf);
        data->zero_buf = (char *)calloc(1, stride * bs);
        if (!data->zero_buf) {
            data->zero_stride = 0;
            LOG_ERROR_RETURN(ENOMEM, ENOMEM, "failed to allocate zero buffer, ` bytes", stride * bs);
        }
        data->zero_stride = stride;
    }

    off_t off = (off_t)block * bs;
    while (count) {
        size_t n = count < data->zero_stride ? count : data->zero_stride;
        size_t left = n * bs;
        while (left) {
            auto written = data->file->pwrite(data->zero_buf, left, off);
            if (written <= 0) {
                LOG_ERROR("failed to zero blocks, got `, expect `", written, left);
                return errno ? errno : EIO;
            }
            left -= written;
            off += written;
        }
        count -= n;
    }
    return 0;
}

namespace photon {
namespace fs {

errcode_t ext2fs_open2(IFile *file, int flags, unsigned int superblock,
                       unsigned int block_size, ext2_filsys *fs) {
    char name[kDevNameSize];
    format_dev_name(file, name, sizeof(name));
    *fs = nullptr;
    return ::ext2fs_open2(name, nullptr, flags, superblock, block_size,
                          &extfs_io_manager, fs);
}

errcode_t ext2fs_open(IFile *file, int flags, unsigned int superblock,
                      unsigned int block_size, ext2_filsys *fs) {
    char name[kDevNameSize];
    format_dev_name(file, name, sizeof(name));
    *fs = nullptr;
    return ::ext2fs_open(name, flags, superblock, block_size,
                         &extfs_io_manager, fs);
}

errcode_t ext2fs_initialize(IFile *file, int flags,
                            struct ext2_super_block *param, ext2_filsys *fs) {
    char name[kDevNameSize];
    format_dev_name(file, name, sizeof(name));
    *fs = nullptr;
    return ::ext2fs_initialize(name, flags, param, &extfs_io_manager, fs);
}

}
}
