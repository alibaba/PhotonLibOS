/*
Copyright 2025 The Photon Authors

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

#include "lz4.h"
#include <photon/common/checksum/xxhash.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <vector>

#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>

extern "C" {
#include <cpa.h>
#include <cpa_dc.h>
#include <icp_sal_poll.h>
#include <icp_sal_user.h>
#include <qat_mem.h>
}

namespace photon::qat {

// ---------------------------------------------------------------------------
// LZ4 Frame Format
// Referenced from veSAL dc_format.h
//
// Standard LZ4 frame layout:
//   [7-byte Header]  magic + FLG + BD + HC
//   [Data Block]*    4-byte block_size (bit31=uncompressed) + block_data
//                    + 4-byte block_checksum (if FLG.BlockCksum=1)
//   [4-byte EndMark] 0x00000000
//   + 4-byte content_checksum (if FLG.ContentCksum=1)
//
// FLG byte: version=1(bit6), block_indep=1(bit5), block_cksum(bit4),
//           content_size(bit3), content_cksum(bit2), dict_id(bit1-0)
// BD byte: max_block_size=64KB (0x40)
// HC byte: (xxhash32({FLG, BD}) >> 8) & 0xFF
// ---------------------------------------------------------------------------
static constexpr uint32_t kLz4FrameHeaderMagic = 0x184D2204u;

static constexpr uint32_t kLz4FrameEndMark         = 0u;
static constexpr size_t   kLz4FrameEndMarkSize   = sizeof(kLz4FrameEndMark);   // 4

static constexpr size_t   kLz4FrameHeaderSize    = 7;   // magic(4) + FLG(1) + BD(1) + HC(1)
static constexpr size_t   kLz4BlockHeaderSize    = 4;   // 4-byte block size field
static constexpr size_t   kLz4BlockCksumSize     = 4;   // 4-byte block checksum
static constexpr size_t   kContentCksumSize      = 4;   // 4-byte content checksum
static constexpr uint32_t kMaxBlockSize          = 64 * 1024;
static constexpr uint32_t kMaxInputSize          = 0x7E000000u;                // ~2GB, same as LZ4_MAX_INPUT_SIZE
static constexpr uint32_t kLz4BlockUncompressedFlag = 0x80000000u;            // bit31 = uncompressed

// FLG bit positions
static constexpr uint8_t kFlgVersion     = 0x40;   // bit6: version=1 (required)
static constexpr uint8_t kFlgBlockIndep  = 0x20;   // bit5: block_indep=1 (required by QAT)
static constexpr uint8_t kFlgBlockCksum  = 0x10;   // bit4: block checksum
static constexpr uint8_t kFlgContentCksum = 0x04;  // bit2: content checksum
static constexpr uint8_t kFlgDictIdMask  = 0x03;   // bit1-0: dict_id

// BD (Block Maximum Size Descriptor) byte: bits[6:4] select max block size.
// Value 4→64KB, 5→256KB, 6→1MB, 7→4MB; formula: 64KB << 2*(n-4).
static constexpr uint8_t  kBdBlockMask    = 0x70;
static constexpr uint8_t  kBdBlockShift   = 4;
static constexpr uint32_t kBdBlockSizeBase = 64 * 1024;

static uint32_t lz4_block_max_size(uint8_t bd) {
    uint32_t n = (bd & kBdBlockMask) >> kBdBlockShift;
    if (n < 4 || n > 7) return 0;
    return kBdBlockSizeBase << (2 * (n - 4));
}

// Maximum number of blocks processed in parallel
// Bounded DMA memory per batch: ~8 × 192KB ≈ 1.5MB
static constexpr uint32_t kPipelineDepth = 8;

static size_t lz4_frame_header_gen(uint8_t* buf, bool block_cksum, bool content_cksum) {
    uint8_t flg = kFlgVersion | kFlgBlockIndep;
    if (block_cksum) flg |= kFlgBlockCksum;
    if (content_cksum) flg |= kFlgContentCksum;
    uint8_t bd = 4 << kBdBlockShift;  // max_block_size=64KB (n=4)
    uint8_t flg_bd[2] = {flg, bd};
    uint8_t hc = (uint8_t)((xxhash32(flg_bd, 2, 0) >> 8) & 0xFF);

    *(uint32_t*)buf = kLz4FrameHeaderMagic;
    buf[4] = flg;
    buf[5] = bd;
    buf[6] = hc;
    return kLz4FrameHeaderSize;
}

// ---------------------------------------------------------------------------
// Global QAT user-start/stop
// ---------------------------------------------------------------------------
static int g_qat_user_ref_count = 0;
static bool g_qat_user_started = false;
static photon::spinlock g_qat_user_lock;

static bool qat_user_start() {
    SCOPED_LOCK(g_qat_user_lock);
    int prev = g_qat_user_ref_count++;
    if (prev > 0) return true;  // already started

    // Try section names: "SSL", then "SSL0".."SSL1023"
    CpaStatus st = icp_sal_userStart("SSL");
    if (st != CPA_STATUS_SUCCESS) {
        for (int i = 0; i < 1024; i++) {
            char section[32];
            snprintf(section, sizeof(section), "SSL%d", i);
            st = icp_sal_userStart(section);
            if (st == CPA_STATUS_SUCCESS) break;
        }
    }
    if (st != CPA_STATUS_SUCCESS) {
        g_qat_user_ref_count--;
        LOG_ERROR_RETURN(0, false, "Failed to icp_sal_userStart, ", VALUE(st));
    }

    g_qat_user_started = true;
    return true;
}

static void qat_user_stop() {
    SCOPED_LOCK(g_qat_user_lock);
    int prev = g_qat_user_ref_count--;
    if (prev > 1) return;  // still in use
    if (g_qat_user_started) {
        icp_sal_userStop();
        g_qat_user_started = false;
    }
}

// ---------------------------------------------------------------------------
// Find and start an EPOLL-mode DC instance
// ---------------------------------------------------------------------------
static CpaInstanceHandle find_dc_instance(int device_id) {
    Cpa16U num_instances = 0;
    CpaStatus st = cpaDcGetNumInstances(&num_instances);
    if (st != CPA_STATUS_SUCCESS || num_instances == 0)
        LOG_ERROR_RETURN(0, nullptr, "No QAT DC instances found, ` ", VALUE(st), num_instances);

    std::vector<CpaInstanceHandle> instances(num_instances);
    st = cpaDcGetInstances(num_instances, instances.data());
    if (st != CPA_STATUS_SUCCESS)
        LOG_ERROR_RETURN(0, nullptr, "cpaDcGetInstances failed, ", VALUE(st));

    CpaInstanceHandle chosen = nullptr;

    // If device_id specified, find that device
    if (device_id >= 0) {
        for (Cpa16U i = 0; i < num_instances; i++) {
            CpaInstanceInfo2 info;
            st = cpaDcInstanceGetInfo2(instances[i], &info);
            if (st != CPA_STATUS_SUCCESS) continue;
            if ((int)(info.physInstId.busAddress >> 8) == device_id &&
                info.isPolled == 0) {
                chosen = instances[i];
                break;
            }
        }
        if (!chosen) {
            LOG_WARN("No EPOLL-mode QAT instance for ", device_id);
        }
    }

    // Otherwise, find first EPOLL-mode instance
    if (!chosen) {
        for (Cpa16U i = 0; i < num_instances; i++) {
            CpaInstanceInfo2 info;
            st = cpaDcInstanceGetInfo2(instances[i], &info);
            if (st != CPA_STATUS_SUCCESS) continue;
            if (info.isPolled == 0) {
                chosen = instances[i];
                break;
            }
        }
    }

    // Fallback: use first instance (will busy-poll)
    if (!chosen) {
        LOG_WARN("No EPOLL-mode QAT instance found, using first instance "
                 "(will busy-poll)");
        chosen = instances[0];
    }

    return chosen;
}

struct QATRequest {
    int status = 0;  // 0 = pending, 1 = success, -1 = error
    CpaDcRqResults cpa_results;
    uint32_t slot = 0;
    // Called by qat_callback after setting status/results.
    // Used to signal per-slot completion.
    void (*on_complete)(void* ctx, uint32_t slot) = nullptr;
    void* on_complete_ctx = nullptr;
};

static void qat_callback(void* p_callback_tag, CpaStatus status) {
    auto* req = (QATRequest*)p_callback_tag;
    if (!req) return;

    if (status != CPA_STATUS_SUCCESS || req->cpa_results.status != CPA_DC_OK) {
        req->status = -1;
        LOG_WARN("QAT callback error, ` ", VALUE(status), VALUE(req->cpa_results.status));
    } else {
        req->status = 1;
    }

    if (req->on_complete) req->on_complete(req->on_complete_ctx, req->slot);
}

struct FrameHeader {
    const uint8_t* src_ptr;
    uint32_t src_remaining;
    uint32_t block_max_size;
    bool has_block_cksum;
    bool has_content_cksum;
    bool verify_content_cksum;
};

static int parse_frame_header(const uint8_t* src, uint32_t src_len,
                              FrameHeader& hdr) {
    if (src_len < kLz4FrameHeaderSize)
        LOG_ERROR_RETURN(EINVAL, -1, "Input too short for LZ4 frame header");
    uint32_t magic = *(const uint32_t*)src;
    if (magic != kLz4FrameHeaderMagic)
        LOG_ERROR_RETURN(EINVAL, -1, "Invalid LZ4 frame magic number");
    uint8_t flg = src[4];
    hdr.has_block_cksum = (flg & kFlgBlockCksum) != 0;
    hdr.has_content_cksum = (flg & kFlgContentCksum) != 0;
    if (!(flg & kFlgBlockIndep))
        LOG_ERROR_RETURN(EINVAL, -1, "LZ4 block_indep=0 (block-dependent) not supported");
    if (flg & kFlgDictIdMask)
        LOG_ERROR_RETURN(EINVAL, -1, "LZ4 dict_id not supported");
    uint8_t bd = src[5];
    hdr.block_max_size = lz4_block_max_size(bd);
    if (hdr.block_max_size == 0)
        LOG_ERROR_RETURN(EINVAL, -1, "LZ4 invalid BD block size, ", VALUE(bd));
    hdr.src_ptr = src + kLz4FrameHeaderSize;
    hdr.src_remaining = src_len - kLz4FrameHeaderSize;
    hdr.verify_content_cksum = hdr.has_content_cksum && !hdr.has_block_cksum;
    return 0;
}

// DMA memory allocation for QAT operations.
// qzMalloc allocates physically contiguous, pinned (non-swappable) memory
// via __get_free_pages(), with a minimum granularity of one page (4KB).
// Each call involves page allocation, IOMMU mapping, and TLB invalidation,
// making it orders of magnitude slower than malloc(). Small allocations
// (e.g. 64-128 byte metadata) still consume a full 4KB page.
// The USDM driver (qzMalloc with QZ_HUGE_PAGES) can use huge pages (2MB)
// to reduce TLB pressure for large buffers, but the qat_mem driver used
// here does not support huge pages.
template<typename T = void*> inline
T dma_alloc(size_t size) { return (T)qzMalloc(size, 0, QZ_MEM_AFFINITY); }

// Frees DMA memory and nulls the pointer. qzFree reverses the IOMMU mapping
// and releases the physical pages.
template<typename T> inline
void dma_free(T& ptr) { if (ptr) {qzFree(ptr); ptr = nullptr; } }

class QATLz4Codec : public ICodec {
    // Per-slot state: DMA buffers, QAT request/operation structures.
    struct Slot {
        QATRequest req;
        CpaFlatBuffer src_flat, dst_flat;
        CpaBufferList src_buf_list, dst_buf_list;
        CpaDcOpData op_data;
        uint8_t* src_dma = nullptr;
        uint8_t* dst_dma = nullptr;

        Slot() {
            src_buf_list.pBuffers = &src_flat;
            src_buf_list.numBuffers = 1;
            dst_buf_list.pBuffers = &dst_flat;
            dst_buf_list.numBuffers = 1;

            memset(&op_data, 0, sizeof(op_data));
            op_data.compressAndVerify = CPA_TRUE;
            op_data.compressAndVerifyAndRecover = CPA_FALSE;
            op_data.flushFlag = CPA_DC_FLUSH_FINAL;
            op_data.inputSkipData.skipMode = CPA_DC_SKIP_DISABLED;
            op_data.outputSkipData.skipMode = CPA_DC_SKIP_DISABLED;
            op_data.integrityCrcCheck = CPA_FALSE;
            op_data.pCrcData = nullptr;

            req.on_complete = &QATLz4Codec::complete;
        }

        bool init(uint32_t src_dma_size, uint32_t dst_dma_size) {
            src_dma = dma_alloc<uint8_t*>(src_dma_size);
            dst_dma = dma_alloc<uint8_t*>(dst_dma_size);
            if (!src_dma || !dst_dma) return false;

            src_flat.pData = src_dma;
            dst_flat.pData = dst_dma;

            return true;
        }

        void free() {
            dma_free(src_dma);
            dma_free(dst_dma);
        }

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };

    // Pipeline state
    Slot slots_[kPipelineDepth];
    uint8_t* meta_base_ = nullptr;
    uint32_t submitted_ = 0;
    photon::semaphore completed_{0};

    bool init_slots(uint32_t src_dma_size, uint32_t dst_dma_size) {
        if (buf_meta_size_ > 0) {
            meta_base_ = dma_alloc<uint8_t*>(buf_meta_size_ * kPipelineDepth * 2);
            if (!meta_base_) return false;
        }
        for (uint32_t i = 0; i < kPipelineDepth; i++) {
            if (!slots_[i].init(src_dma_size, dst_dma_size)) {
                return false;
            }
            if (meta_base_) {
                slots_[i].src_buf_list.pPrivateMetaData = meta_base_ + i * 2 * buf_meta_size_;
                slots_[i].dst_buf_list.pPrivateMetaData = meta_base_ + (i * 2 + 1) * buf_meta_size_;
            }
        }
        return true;
    }

    void free_slots() {
        drain();
        for (uint32_t i = 0; i < kPipelineDepth; i++)
            slots_[i].free();
        dma_free(meta_base_);
    }

    int submit(bool is_compress,
               const void* src_data, uint32_t src_len,
               uint32_t dst_buf_capacity) {
        uint32_t slot = submitted_;
        auto& s = slots_[slot];
        memcpy(s.src_dma, src_data, src_len);

        uint32_t footer_size = 0;
        auto submit_fn = cpaDcCompressData2;
        if (!is_compress) {
            submit_fn = cpaDcDecompressData2;
            *(uint32_t*)(s.src_dma + src_len) = kLz4FrameEndMark;
            footer_size = kLz4FrameEndMarkSize;
        }
        s.src_flat.dataLenInBytes = src_len + footer_size;
        s.dst_flat.dataLenInBytes = dst_buf_capacity;
        s.req.status = 0;
        memset(&s.req.cpa_results, 0, sizeof(s.req.cpa_results));

        while (true) {
            CpaStatus st = submit_fn(instance_, session_, &s.src_buf_list, &s.dst_buf_list,
                                      &s.op_data, &s.req.cpa_results, &s.req);
            if (st == CPA_STATUS_RETRY) {
                photon::thread_usleep(100);
                continue;
            }
            if (st != CPA_STATUS_SUCCESS)
                LOG_ERROR_RETURN(EIO, -1, "QAT submit failed, ", VALUE(st));
            submitted_++;
            return (int)slot;
        }
    }

    void drain() {
        if (submitted_ > 0) {
            completed_.wait(submitted_);
            submitted_ = 0;
        }
    }

    static void complete(void* ctx, uint32_t slot) {
        auto* self = (QATLz4Codec*)ctx;
        self->completed_.signal(1);
    }

public:
    QATLz4Codec() {
        for (uint32_t i = 0; i < kPipelineDepth; i++) {
            slots_[i].req.slot = i;
            slots_[i].req.on_complete_ctx = this;
        }
    }

    ~QATLz4Codec() override {
        cleanup();
        qat_user_stop();
    }

    int compress(Buffer src, Buffer dst) override {
        if (!session_)
            LOG_ERROR_RETURN(EINVAL, -1, "QAT session not initialized");

        if (src.size > kMaxInputSize)
            LOG_ERROR_RETURN(E2BIG, -1, "Source data too large, max ` ", kMaxInputSize, src.size);

        if (!init_slots(kMaxBlockSize, kMaxBlockSize))
            LOG_ERROR_RETURN(ENOMEM, -1, "Failed to allocate QAT DMA buffers");
        DEFER(free_slots());

        auto* dst_bytes = (uint8_t*)dst.addr;
        uint32_t offset = 0;
        auto* src_bytes = (const uint8_t*)src.addr;
        if ((uint64_t)src.size > UINT32_MAX)
            LOG_ERROR_RETURN(E2BIG, -1, "Compress src data too large");
        uint32_t src_len = (uint32_t)src.size;
        if ((uint64_t)dst.size > UINT32_MAX)
            LOG_ERROR_RETURN(ENOBUFS, -1, "Compress dst buffer too large");
        uint32_t dst_len = (uint32_t)dst.size;

        bool do_block_cksum = block_cksum_;
        bool do_content_cksum = content_cksum_ && !block_cksum_;
        uint32_t per_block_overhead = kLz4BlockHeaderSize;
        if (do_block_cksum) per_block_overhead += kLz4BlockCksumSize;

        if (offset + kLz4FrameHeaderSize > dst_len)
            LOG_ERROR_RETURN(ENOBUFS, -1, "Compress dst buffer too small for frame header");
        memcpy(dst_bytes + offset, header_buf_, kLz4FrameHeaderSize);
        offset += kLz4FrameHeaderSize;

        uint32_t num_blocks = (src_len + kMaxBlockSize - 1) / kMaxBlockSize;

        for (uint32_t wi = 0; wi < num_blocks; wi += kPipelineDepth) {
            uint32_t window = std::min(kPipelineDepth, num_blocks - wi);

            for (uint32_t j = 0; j < window; j++) {
                uint32_t src_off = (wi + j) * kMaxBlockSize;
                uint32_t block_len = std::min(kMaxBlockSize, src_len - src_off);
                if (submit(true, src_bytes + src_off,
                           block_len, kMaxBlockSize) < 0) return -1;
            }

            drain();

            for (uint32_t j = 0; j < window; j++) {
                uint32_t src_off = (wi + j) * kMaxBlockSize;

                uint32_t bsl = slots_[j].src_flat.dataLenInBytes;
                bool incompressible = slots_[j].req.status < 0 ||
                                      slots_[j].req.cpa_results.produced >= bsl;

                if (offset + per_block_overhead + bsl > dst_len)
                    LOG_ERROR_RETURN(ENOBUFS, -1, "Compress dst buffer too small for block data");

                if (incompressible) {
                    uint32_t bsz = bsl | kLz4BlockUncompressedFlag;
                    *(uint32_t*)(dst_bytes + offset) = bsz;
                    offset += kLz4BlockHeaderSize;
                    memcpy(dst_bytes + offset, src_bytes + src_off, bsl);
                    offset += bsl;
                } else {
                    uint32_t comp_size = slots_[j].req.cpa_results.produced;
                    *(uint32_t*)(dst_bytes + offset) = comp_size;
                    offset += kLz4BlockHeaderSize;
                    memcpy(dst_bytes + offset, slots_[j].dst_dma, comp_size);
                    offset += comp_size;
                }

                if (do_block_cksum) {
                    uint32_t cksum = slots_[j].req.cpa_results.checksum;
                    *(uint32_t*)(dst_bytes + offset) = cksum;
                    offset += kLz4BlockCksumSize;
                }
            }
        }

        uint32_t tail_size = kLz4FrameEndMarkSize;
        if (do_content_cksum) tail_size += kContentCksumSize;
        if (offset + tail_size > dst_len)
            LOG_ERROR_RETURN(ENOBUFS, -1, "Compress dst buffer too small for frame footer");
        *(uint32_t*)(dst_bytes + offset) = kLz4FrameEndMark;
        offset += kLz4FrameEndMarkSize;

        if (do_content_cksum) {
            uint32_t content_cksum_val = xxhash32(src_bytes, src_len);
            *(uint32_t*)(dst_bytes + offset) = content_cksum_val;
            offset += kContentCksumSize;
        }

        return (int)offset;
    }

    int decompress(Buffer src, Buffer dst) override {
        if (!session_)
            LOG_ERROR_RETURN(EINVAL, -1, "QAT session not initialized");

        auto* src_bytes = (const uint8_t*)src.addr;
        if ((uint64_t)src.size > UINT32_MAX)
            LOG_ERROR_RETURN(E2BIG, -1, "Decompress src data too large");
        uint32_t src_len = (uint32_t)src.size;
        if ((uint64_t)dst.size > UINT32_MAX)
            LOG_ERROR_RETURN(ENOBUFS, -1, "Decompress dst buffer too large");
        uint32_t dst_len = (uint32_t)dst.size;

        FrameHeader hdr;
        if (parse_frame_header(src_bytes, src_len, hdr) < 0) return -1;

        auto* src_ptr = hdr.src_ptr;
        uint32_t src_remaining = hdr.src_remaining;
        auto* dst_bytes = (uint8_t*)dst.addr;
        uint32_t dst_offset = 0;
        bool has_block_cksum = hdr.has_block_cksum;
        bool verify_content_cksum = hdr.verify_content_cksum;

        uint32_t block_max_size = hdr.block_max_size;
        uint32_t src_dma_size = block_max_size + kLz4BlockHeaderSize + kLz4FrameEndMarkSize;

        if (!init_slots(src_dma_size, block_max_size))
            LOG_ERROR_RETURN(ENOMEM, -1, "Failed to allocate QAT DMA buffers");
        DEFER(free_slots());

        while (src_remaining >= kLz4BlockHeaderSize) {
            struct BlockInfo {
                bool uncompressed;
                uint32_t data_size;
                const uint8_t* data_ptr;
            };
            BlockInfo infos[kPipelineDepth];
            uint32_t info_count = 0;
            bool end_mark = false;

            for (uint32_t i = 0; i < kPipelineDepth && src_remaining >= kLz4BlockHeaderSize; i++) {
                uint32_t block_size_raw = *(const uint32_t*)src_ptr;

                if (block_size_raw == 0) {
                    end_mark = true;
                    src_ptr += kLz4BlockHeaderSize;
                    src_remaining -= kLz4BlockHeaderSize;
                    break;
                }

                bool uncompressed = (block_size_raw & kLz4BlockUncompressedFlag) != 0;
                uint32_t data_size = block_size_raw & ~kLz4BlockUncompressedFlag;
                uint32_t block_total = kLz4BlockHeaderSize + data_size;
                if (has_block_cksum) block_total += kLz4BlockCksumSize;
                if (block_total > src_remaining) {
                    LOG_ERROR_RETURN(EINVAL, -1, "LZ4 block data exceeds remaining input");
                }

                infos[info_count++] = {uncompressed, data_size, src_ptr + kLz4BlockHeaderSize};
                src_ptr += block_total;
                src_remaining -= block_total;
            }

            if (info_count == 0) break;

            for (uint32_t i = 0; i < info_count; i++) {
                auto& info = infos[i];
                if (info.uncompressed) continue;

                uint32_t mini_frame_len = kLz4BlockHeaderSize + info.data_size;
                if (submit(false, info.data_ptr - kLz4BlockHeaderSize,
                           mini_frame_len, block_max_size) < 0) return -1;
            }

            drain();

            for (uint32_t i = 0, slot = 0; i < info_count; i++) {
                auto& info = infos[i];

                if (info.uncompressed) {
                    if (dst_offset + info.data_size > dst_len)
                        LOG_ERROR_RETURN(ENOBUFS, -1, "Decompress dst buffer too small");
                    memcpy(dst_bytes + dst_offset, info.data_ptr, info.data_size);
                    dst_offset += info.data_size;
                } else {
                    if (slots_[slot].req.status < 0)
                        LOG_ERROR_RETURN(EIO, -1, "QAT decompress operation failed");

                    uint32_t decomp_size = slots_[slot].req.cpa_results.produced;

                    if (has_block_cksum) {
                        uint32_t expected_cksum = *(const uint32_t*)(info.data_ptr + info.data_size);
                        uint32_t actual_cksum = slots_[slot].req.cpa_results.checksum;
                        if (actual_cksum != expected_cksum)
                            LOG_ERROR_RETURN(EIO, -1, "LZ4 block checksum mismatch, expected ` got ", VALUE(expected_cksum), VALUE(actual_cksum));
                    }

                    if (dst_offset + decomp_size > dst_len)
                        LOG_ERROR_RETURN(ENOBUFS, -1, "Decompress dst buffer too small");
                    memcpy(dst_bytes + dst_offset, slots_[slot].dst_dma, decomp_size);
                    dst_offset += decomp_size;
                    slot++;
                }
            }

            if (end_mark) break;
        }

        if (verify_content_cksum) {
            if (src_remaining < kContentCksumSize)
                LOG_ERROR_RETURN(EINVAL, -1, "Content checksum missing from input");
            uint32_t expected_content_cksum = *(const uint32_t*)src_ptr;
            uint32_t actual_content_cksum = xxhash32(dst_bytes, dst_offset);
            if (actual_content_cksum != expected_content_cksum)
                LOG_ERROR_RETURN(EIO, -1, "LZ4 content checksum mismatch, expected ` got ", VALUE(expected_content_cksum), VALUE(actual_content_cksum));
        }

        return (int)dst_offset;
    }

    template<typename R>
    void threads_create_join_capped(uint64_t n, TempDelegate<R> start,
                                    uint64_t max_concurrent = 32) {
        uint64_t launched = 0;
        while (launched < n) {
            uint64_t batch = std::min(n - launched, max_concurrent);
            threads_create_join(batch, start);
            launched += batch;
        }
    }

    int batch_process(Buffer* src, Buffer* dst, size_t n,
                      int (ICodec::*fn)(Buffer, Buffer)) {
        if (!session_)
            LOG_ERROR_RETURN(EINVAL, -1, "QAT session not initialized");

        int error = 0;
        size_t next = 0;
        threads_create_join_capped(n, [&]() {
            size_t i = next++;
            if (i >= n) return;
            int ret = (this->*fn)(src[i], dst[i]);
            if (ret < 0) { error = -1; }
            else { dst[i].size = ret; }
        });
        return error;
    }

    int compress(Buffer* src, Buffer* dst, size_t n) override {
        return batch_process(src, dst, n, &ICodec::compress);
    }

    int decompress(Buffer* src, Buffer* dst, size_t n) override {
        return batch_process(src, dst, n, &ICodec::decompress);
    }

    int init(int device_id, int comp_level, bool block_cksum, bool content_cksum) {
        bool ok = false;

        // Step 1: Global QAT user-space start (ref-counted)
        if (!qat_user_start())
            LOG_ERROR_RETURN(0, -1, "QAT user start failed");
        DEFER(if (!ok) qat_user_stop());

        // Step 2: Find a DC instance (prefer EPOLL mode)
        instance_ = find_dc_instance(device_id);
        if (!instance_)
            LOG_ERROR_RETURN(0, -1, "No QAT DC instance available");
        block_cksum_ = block_cksum;
        content_cksum_ = content_cksum;
        // When block_cksum is on, content_cksum is considered off
        bool effective_content_cksum = content_cksum && !block_cksum;
        lz4_frame_header_gen(header_buf_, block_cksum, effective_content_cksum);

        // Step 3: Set address translation for DMA
        CpaStatus st = cpaDcSetAddressTranslation(instance_, qzVirtToPhys);
        if (st != CPA_STATUS_SUCCESS)
            LOG_ERROR_RETURN(0, -1, "cpaDcSetAddressTranslation failed, ", VALUE(st));

        // Step 4: Start the DC instance
        st = cpaDcStartInstance(instance_, 0, nullptr);
        if (st != CPA_STATUS_SUCCESS)
            LOG_ERROR_RETURN(0, -1, "cpaDcStartInstance failed, ", VALUE(st));
        // Step 5: Get epoll file descriptor for event-driven waiting
        int fd = -1;
        st = icp_sal_DcGetFileDescriptor(instance_, &fd);
        if (st == CPA_STATUS_SUCCESS && fd >= 0) {
            qat_fd_ = fd;
        } else {
            LOG_WARN("Failed to get QAT EPOLL fd, ", VALUE(st), " (will use polling fallback)");
            qat_fd_ = -1;
        }

        // Start background QAT event polling thread
        start_poller();

        // Step 6: Get buffer list metadata size
        st = cpaDcBufferListGetMetaSize(instance_, 1, &buf_meta_size_);
        if (st != CPA_STATUS_SUCCESS)
            LOG_ERROR_RETURN(0, -1, "cpaDcBufferListGetMetaSize failed, ", VALUE(st));

        // Step 7: Setup LZ4 session
        CpaDcSessionSetupData sess_data;
        memset(&sess_data, 0, sizeof(sess_data));
        sess_data.sessState = CPA_DC_STATELESS;
        sess_data.compLevel = (CpaDcCompLvl)comp_level;
        sess_data.autoSelectBestHuffmanTree = CPA_DC_ASB_DISABLED;
        sess_data.sessDirection = CPA_DC_DIR_COMBINED;
        sess_data.compType = CPA_DC_LZ4;
        sess_data.checksum = CPA_DC_XXHASH32;
        sess_data.huffType = CPA_DC_HT_STATIC;
        sess_data.lz4BlockMaxSize = CPA_DC_LZ4_MAX_BLOCK_SIZE_64K;
        sess_data.lz4BlockIndependence = CPA_TRUE;
        sess_data.lz4BlockChecksum = block_cksum_ ? CPA_TRUE : CPA_FALSE;
        sess_data.accumulateXXHash = CPA_FALSE;

        Cpa32U session_size = 0;
        Cpa32U ctx_size = 0;
        st = cpaDcGetSessionSize(instance_, &sess_data, &session_size,
                                  &ctx_size);
        if (st != CPA_STATUS_SUCCESS)
            LOG_ERROR_RETURN(0, -1, "cpaDcGetSessionSize failed, ", VALUE(st));

        session_ = dma_alloc<CpaDcSessionHandle>(session_size);
        if (!session_)
            LOG_ERROR_RETURN(0, -1, "Failed to allocate QAT session memory");

        st = cpaDcInitSession(instance_, session_, &sess_data,
                               nullptr, qat_callback);
        if (st != CPA_STATUS_SUCCESS)
            LOG_ERROR_RETURN(0, -1, "cpaDcInitSession failed, ", VALUE(st));

        ok = true;
        return 0;
    }

    CpaInstanceHandle instance_ = nullptr;
    CpaDcSessionHandle session_ = nullptr;
    int qat_fd_ = -1;
    uint32_t buf_meta_size_ = 0;
    bool block_cksum_ = true;
    bool content_cksum_ = false;
    uint8_t header_buf_[kLz4FrameHeaderSize];
    photon::thread* poller_thread_ = nullptr;
    std::atomic<bool> poller_stopped_{false};

    void start_poller() {
        poller_stopped_.store(false, std::memory_order_relaxed);
        poller_thread_ = photon::thread_create11(&QATLz4Codec::poller_loop, this);
        photon::thread_enable_join(poller_thread_);
    }

    void stop_poller() {
        if (!poller_thread_) return;
        poller_stopped_.store(true, std::memory_order_release);
        photon::thread_interrupt(poller_thread_, EOK);
        photon::thread_join((photon::join_handle*)poller_thread_);
        poller_thread_ = nullptr;
    }

    void poller_loop() {
        while (!poller_stopped_.load(std::memory_order_acquire)) {
            if (qat_fd_ >= 0) {
                int ret = photon::wait_for_fd_readable(qat_fd_);
                if (ret < 0 && errno != EINTR) {
                    LOG_WARN("QAT poller: wait_for_fd_readable failed, ", ERRNO());
                }
            } else {
                photon::thread_usleep(100);
            }
            icp_sal_DcPollInstance(instance_, 0);
        }
    }

    void cleanup() {
        stop_poller();
        if (session_ && instance_) {
            cpaDcRemoveSession(instance_, session_);
        }
        if (session_) {
            dma_free(session_);
        }
        if (qat_fd_ >= 0 && instance_) {
            icp_sal_DcPutFileDescriptor(instance_, qat_fd_);
            qat_fd_ = -1;
        }
        if (instance_) {
            cpaDcStopInstance(instance_);
            instance_ = nullptr;
        }
    }
};

ICodec* new_lz4_codec(int device_id, int comp_level, bool block_cksum, bool content_cksum) {
    return NewObj<QATLz4Codec>()->init(device_id, comp_level, block_cksum, content_cksum);
}

}  // namespace photon::qat
