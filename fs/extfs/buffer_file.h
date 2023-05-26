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
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <map>
#include <algorithm>
#include <photon/fs/filesystem.h>
#include <photon/fs/forwardfs.h>
#include <photon/common/alog.h>

#define DEFAULT_BLOCK_SIZE 4096

photon::fs::IFile *new_buffer_file(photon::fs::IFile *file);

class BufferFile : public photon::fs::ForwardFile {
public:
    BufferFile(photon::fs::IFile *file, uint32_t buffer_size = 8 << 20, uint32_t _block_size = 128 << 10)
    : photon::fs::ForwardFile(file), block_size(_block_size) {
        if (buffer_size % block_size != 0 \
        || block_size % EXT_BLOCK_SIZE != 0 \
        || lowbit(block_size) != block_size) {
            LOG_ERROR("buffer_size must be a multiple of block_size, " \
                "block_size must be a multiple of EXT_IO_BLOCK_SIZE, " \
                "block_size must be a power of 2");
            return;
        }
        block_size_bit = __builtin_ffs(block_size) - 1;
        block_size_mod = block_size - 1;
        block_counts = DIV(buffer_size);
        buffer = (char*) malloc(buffer_size);
        head = tail = nullptr;
    }
    ~BufferFile() {
        close();
    }
    virtual int close() override {
        for (auto &it : mp) {
            int ret = flush_node(it.second);
            if (ret) {
                return -1;
            }
            delete it.second;
        }
        free(buffer);
        LOG_INFO("buffer file flush and close, cache hit: `, cache miss: `, cache hit rate: `%", cache_hit, cache_miss, cache_hit * 100.0 / (cache_hit + cache_miss));
        LOG_INFO("mem_read: `, mem_write: `, disk_read: `, disk_write: `", mem_read, mem_write, disk_read, disk_write);
        return 0;
    }
    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        if (count != EXT_BLOCK_SIZE || (offset & EXT_BLOCK_SIZE_MOD)) {
            disk_read += count;
            return m_file->pread(buf, count, offset);
        }
        ListNode *x = get_buffer_block(DIV(offset));
        if (!x) {
            LOG_ERROR_RETURN(0, -1, "BufferFile: failed to pread from file to buffer");
        }
        off_t buffer_offset = pos(x->buffer_block_id, MOD(offset));
        mem_read += count;
        return std::copy_n(buffer + buffer_offset, count, (char*) buf) - (char*) buf;
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        if (count != EXT_BLOCK_SIZE || (offset & EXT_BLOCK_SIZE_MOD)) {
            disk_write += count;
            return m_file->pwrite(buf, count, offset);
        }
        ListNode *x = get_buffer_block(DIV(offset));
        if (!x) {
            LOG_ERROR_RETURN(0, -1, "BufferFile: failed to pwrite from file to buffer");
        }
        x->is_dif = true;
        off_t buffer_offset = pos(x->buffer_block_id, MOD(offset));
        mem_write += count;
        return std::copy_n((char*) buf, count, buffer + buffer_offset) - (buffer + buffer_offset);
    }
    virtual int fdatasync() override {
        auto write = disk_write;
        for (auto &it : mp) {
            int ret = flush_node(it.second);
            if (ret) {
                return -1;
            }
        }
        write = disk_write - write;
        LOG_DEBUG("flush data: `", write);
        return 0;
    }

private:
    size_t cache_hit = 0;
    size_t cache_miss = 0;
    uint64_t disk_read = 0;
    uint64_t disk_write = 0;
    uint64_t mem_read = 0;
    uint64_t mem_write = 0;
    static const uint32_t EXT_BLOCK_SIZE = DEFAULT_BLOCK_SIZE;  // ext I/O block size

    uint32_t block_size;  // cache block size
    size_t block_counts;
    char *buffer;

    // LRU index data struct
    struct ListNode {
        bool is_dif;
        ListNode* prv;
        ListNode* nxt;
        off_t block_id;
        off_t buffer_block_id;
        ListNode(): is_dif(false), prv(nullptr), nxt(nullptr), block_id(0), buffer_block_id(0) {};
    };
    std::map<off_t, ListNode*> mp;
    ListNode *head, *tail;

    ListNode *get_buffer_block(off_t block_id) {
        ListNode *x;
        if (mp.count(block_id)) {
            x = mp[block_id];
            pop_node(x);
            cache_hit++;
        } else {
            cache_miss++;
            // cache miss
            assert(mp.size() <= block_counts);
            if (mp.size() != block_counts) {
                x = new ListNode();
                x->block_id = block_id;
                x->buffer_block_id = mp.size();
            } else {
                x = tail;
                int ret = flush_node(tail);
                if (ret) {
                    return nullptr;
                }
                pop_node(tail);
                mp.erase(x->block_id);
                x->block_id = block_id;
            }
            disk_read += block_size;
            auto ret = m_file->pread(buffer + pos(x->buffer_block_id, 0), block_size, pos(x->block_id, 0));
            if (ret != block_size) {
                return nullptr;
            }
            mp[block_id] = x;
        }
        push_node(x);
        return x;
    }

    inline off_t pos(off_t block_id, off_t offset) {
        return MULT(block_id) + offset;
    }
    void pop_node(ListNode *x) {
        if (x == head) head = head->nxt;
        if (x == tail) tail = tail->prv;
        ListNode *prv = x->prv;
        ListNode *nxt = x->nxt;
        if (prv) prv->nxt = nxt;
        if (nxt) nxt->prv = prv;
        x->prv = nullptr;
        x->nxt = nullptr;
    }
    void push_node(ListNode *x) {
        if (head) head->prv = x;
        x->nxt = head;
        head = x;
        if (!tail) tail = x;
    }
    int flush_node(ListNode *x) {
        if (!(x->is_dif)) return 0;

        disk_write += block_size;
        ssize_t ret = m_file->pwrite(buffer + pos(x->buffer_block_id, 0), block_size, pos(x->block_id, 0));
        if (ret != block_size) {
            LOG_ERRNO_RETURN(0, -1, "BufferFile: failed to flush node");
        }
        x->is_dif = false;
        return 0;
    }

    // bit calc
    static const uint32_t EXT_BLOCK_SIZE_MOD = EXT_BLOCK_SIZE - 1;
    uint16_t block_size_bit;  // __builtin_ffs(block_size)
    uint32_t block_size_mod;  // block_size-1
    int64_t lowbit(int64_t x) {
        return x & (-x);
    }
    inline off_t DIV(off_t x) {
        return x >> block_size_bit;
    }
    inline off_t MOD(off_t x) {
        return x & block_size_mod;
    }
    inline off_t MULT(off_t x) {
        return x << block_size_bit;
    }
};

photon::fs::IFile *new_buffer_file(photon::fs::IFile *file) {
    photon::fs::IFile *buffer_file = new BufferFile(file);
    return buffer_file;
}
