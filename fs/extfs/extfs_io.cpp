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
#include <ext2fs/ext2fs.h>
#include <sys/stat.h>
#include <photon/common/alog.h>
#include <photon/common/callback.h>
#include <photon/thread/thread.h>

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

struct unix_private_data {
    int magic;
    int dev;
    int flags;
    int align;
    int access_time;
    ext2_loff_t offset;
    void *bounce;
    struct struct_io_stats io_stats;
};

static struct struct_io_manager extfs_io_manager = {
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

static photon::fs::IFile *backend_file;
// add for debug
static uint64_t total_read_cnt = 0;
static uint64_t total_write_cnt = 0;

static errcode_t extfs_open(const char *name, int flags, io_channel *channel) {
    LOG_INFO(VALUE(name));
    DEFER(LOG_INFO("opened"));
    io_channel io = nullptr;
    struct unix_private_data *data = nullptr;
    errcode_t retval;
    retval = ext2fs_get_mem(sizeof(struct struct_io_channel), &io);
    if (retval)
        return -retval;
    memset(io, 0, sizeof(struct struct_io_channel));
    io->magic = EXT2_ET_MAGIC_IO_CHANNEL;
    retval = ext2fs_get_mem(sizeof(struct unix_private_data), &data);
    if (retval)
        return -retval;
    io->manager = &extfs_io_manager;
    retval = ext2fs_get_mem(strlen(name) + 1, &io->name);
    if (retval)
        return -retval;
    strcpy(io->name, name);
    io->private_data = data;
    io->block_size = 1024;
    io->read_error = 0;
    io->write_error = 0;
    io->refcount = 1;
    io->flags = 0;
    io->reserved[0] = reinterpret_cast<std::uintptr_t>(backend_file);
    LOG_DEBUG(VALUE(backend_file), VALUE((void *)io->reserved[0]));
    memset(data, 0, sizeof(struct unix_private_data));
    data->magic = EXT2_ET_MAGIC_UNIX_IO_CHANNEL;
    data->io_stats.num_fields = 2;
    data->flags = flags;
    data->dev = 0;
    *channel = io;
    return 0;
}

static errcode_t extfs_close(io_channel channel) {
    LOG_INFO("extfs close");
    return ext2fs_free_mem(&channel);
}

static errcode_t extfs_set_blksize(io_channel channel, int blksize) {
    LOG_DEBUG(VALUE(blksize));
    channel->block_size = blksize;
    return 0;
}

static errcode_t extfs_read_blk(io_channel channel, unsigned long block, int count, void *buf) {
    off_t offset = (ext2_loff_t)block * channel->block_size;
    ssize_t size = count < 0 ? -count : (ext2_loff_t)count * channel->block_size;
    auto file = reinterpret_cast<photon::fs::IFile *>(channel->reserved[0]);
    // LOG_DEBUG("read ", VALUE(offset), VALUE(size));
    auto res = file->pread(buf, size, offset);
    if (res == size) {
        total_read_cnt += size;
        return 0;
    }
    LOG_ERROR("failed to pread, got `, expect `", res, size);
    return -1;
}

static errcode_t extfs_read_blk64(io_channel channel, unsigned long long block, int count, void *buf) {
    return extfs_read_blk(channel, block, count, buf);
}

static errcode_t extfs_write_blk(io_channel channel, unsigned long block, int count, const void *buf) {
    off_t offset = (ext2_loff_t)block * channel->block_size;
    ssize_t size = count < 0 ? -count : (ext2_loff_t)count * channel->block_size;
    auto file = reinterpret_cast<photon::fs::IFile *>(channel->reserved[0]);
    // LOG_DEBUG("write ", VALUE(offset), VALUE(size));
    auto res = file->pwrite(buf, size, offset);
    if (res == size) {
        total_write_cnt += size;
        return 0;
    }
    LOG_ERROR("failed to pwrite, got `, expect `", res, size);
    return -1;
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

static errcode_t extfs_zeroout(io_channel channel, unsigned long long block, unsigned long long count) {
    return 0;
}

namespace photon {
namespace fs {

io_manager new_io_manager(photon::fs::IFile *file) {
    backend_file = file;
    return &extfs_io_manager;
}

}
}
