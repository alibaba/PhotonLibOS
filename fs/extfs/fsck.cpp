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
#include "default_uuid.h"
#include <string.h>
#include <arpa/inet.h>
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/common/uuid4.h>

// jbd2 journal superblock, all fields big-endian. Only the offsets we need are
// spelled out here; see linux/jbd2.h for the full layout.
static const uint32_t JBD2_MAGIC = 0xc03b3998u;
static const uint32_t JBD2_CSUM_FEATURES = 0x00000018u;  // CSUM_V2 | CSUM_V3
enum {
    JSB_MAGIC            = 0x00,
    JSB_START            = 0x1c,
    JSB_FEATURE_INCOMPAT = 0x2c,
    JSB_UUID             = 0x30,
};

// how many inode table blocks to read at a time
static const int kItableChunkBlocks = 64;

// a dtime that can never be mistaken for an orphan list link, nor for
// "this deleted inode has no dtime at all"
static const uint32_t kSaneDtime = 0xFFFFFFFFu;

namespace photon {
namespace fs {

extern io_manager new_io_manager(photon::fs::IFile *file);

namespace {
// An internal tally, kept only to log a summary of the run. It is not part of
// the public API: callers read the log, not a struct.
struct FsckStats {
    uint64_t inodes_scanned = 0;
    uint64_t itable_bytes_read = 0;
    uint64_t bitmap_bytes_read = 0;
    uint64_t dtime_fixed = 0;       // deleted inodes given a sane dtime
    uint64_t dtime_cleared = 0;     // in-use inodes whose stale dtime was cleared
    uint32_t groups_total = 0;
    uint32_t groups_marked_zeroed = 0;
    uint32_t groups_itable_zeroed = 0;
    uint32_t groups_hint_lost = 0;  // bg_itable_unused no longer usable for pruning
    uint32_t groups_free_counts_fixed = 0;  // groups whose free counters were corrected
    bool free_blocks_fixed = false;         // the superblock total was corrected
    bool free_inodes_fixed = false;
    bool uuid_changed = false;
    bool journal_uuid_synced = false;
};
}

static bool is_zero(const void *buf, size_t size) {
    auto p = (const uint64_t *)buf;
    for (size_t i = 0; i < size / sizeof(*p); ++i)
        if (p[i]) return false;
    return true;
}

static uint32_t be32_at(const char *buf, int offset) {
    uint32_t v;
    memcpy(&v, buf + offset, sizeof(v));
    return ntohl(v);
}

// Refuse anything we cannot fix up safely. Every check here is a plain
// superblock field, so this costs no I/O.
static int fsck_preflight(ext2_filsys fs) {
    auto sb = fs->super;
    if (ext2fs_has_feature_metadata_csum(sb))
        LOG_ERROR_RETURN(EOPNOTSUPP, -1, "metadata_csum is enabled: changing the uuid would require rewriting all metadata checksums");
    if (ext2fs_has_feature_journal_needs_recovery(sb))
        LOG_ERROR_RETURN(EUCLEAN, -1, "the journal needs recovery: replay it first, or the replay would overwrite our fixes");
    // the on-disk orphan list is chained through i_dtime, so a non-empty list
    // means the dtime of those inodes must not be touched
    if (sb->s_last_orphan || (sb->s_state & EXT3_ORPHAN_FS))
        LOG_ERROR_RETURN(EBUSY, -1, "the orphan inode list is not empty (s_last_orphan=`), run e2fsck -fp first", sb->s_last_orphan);
    if (sb->s_state & EXT2_ERROR_FS)
        LOG_ERROR_RETURN(EUCLEAN, -1, "the fs is marked with errors, run e2fsck first");
    if (!(sb->s_state & EXT2_VALID_FS))
        LOG_ERROR_RETURN(EUCLEAN, -1, "the fs was not unmounted cleanly, run e2fsck first");
    if (ext2fs_has_feature_journal(sb) && !sb->s_journal_inum)
        LOG_ERROR_RETURN(EOPNOTSUPP, -1, "external journal is not supported");
    return 0;
}

struct ItableScanPlan {
    uint64_t itable_total = 0;   // size of all the inode tables
    uint64_t prefix_bytes = 0;   // what the dtime scan has to read
    uint32_t hint_lost = 0;      // groups whose bg_itable_unused is no longer usable
    uint32_t uninit = 0;         // groups whose inode table was never written
    uint32_t zeroed = 0;         // groups already marked as INODE_ZEROED
};

// The inode scan prunes with bg_itable_unused and skips INODE_UNINIT groups
// (see ext2fs_open_inode_scan), so mirror that here to predict its cost.
static ItableScanPlan plan_itable_scan(ext2_filsys fs) {
    ItableScanPlan p;
    const __u32 ipg = EXT2_INODES_PER_GROUP(fs->super);
    const int isize = EXT2_INODE_SIZE(fs->super);
    const bool csum = ext2fs_has_group_desc_csum(fs);
    for (dgrp_t g = 0; g < fs->group_desc_count; ++g) {
        p.itable_total += (uint64_t)ipg * isize;
        if (csum && ext2fs_bg_flags_test(fs, g, EXT2_BG_INODE_ZEROED)) p.zeroed++;
        if (csum && ext2fs_bg_flags_test(fs, g, EXT2_BG_INODE_UNINIT)) {
            p.uninit++;
            continue;
        }
        __u32 unused = csum ? ext2fs_bg_itable_unused(fs, g) : 0;
        if (unused > ipg) unused = ipg;
        // claiming no unused inode at the tail while free ones exist means the
        // hint has been reset, most likely by the kernel's lazy init
        if (unused == 0 && ext2fs_bg_free_inodes_count(fs, g) > 0) p.hint_lost++;
        uint64_t bytes = (uint64_t)(ipg - unused) * isize;
        uint64_t bs = fs->blocksize;
        p.prefix_bytes += (bytes + bs - 1) / bs * bs;
    }
    return p;
}

static void report_plan(ext2_filsys fs, const ItableScanPlan &p) {
    LOG_INFO("inode table: groups=` (inode_uninit=`, already_zeroed=`), total=`MB, dtime scan reads `MB",
             fs->group_desc_count, p.uninit, p.zeroed, p.itable_total >> 20, p.prefix_bytes >> 20);
    if (p.hint_lost) {
        LOG_WARN("bg_itable_unused is unusable on `/` groups: the dtime scan degrades to `MB, `% of the inode table",
                 p.hint_lost, fs->group_desc_count, p.prefix_bytes >> 20,
                 p.itable_total ? p.prefix_bytes * 100 / p.itable_total : 0);
        LOG_WARN("this image was very likely mounted rw before, and the kernel reset the hint when it finished the lazy init");
    }
    if (fs->group_desc_count && p.zeroed == fs->group_desc_count) {
        LOG_WARN("every group is already marked INODE_ZEROED, there is no lazy init left to finish");
    }
}

// An illegal i_dtime, as e2fsck sees it: a deleted inode (links_count == 0)
// with dtime 0 (PR_1_ZERO_DTIME) or a dtime low enough to look like an orphan
// list link (PR_1_LOW_DTIME), or an in-use inode carrying any dtime. A never
// used slot (all zero) is left pristine, to avoid dirtying sparse itable blocks.
static bool fix_dtime(struct ext2_super_block *sb, struct ext2_inode_large *inode,
                      FsckStats *rpt) {
    if (inode->i_links_count) {
        if (!inode->i_dtime) return false;
        inode->i_dtime = 0;
        rpt->dtime_cleared++;
        return true;
    }
    if (!inode->i_dtime) {
        if (!inode->i_mode) return false;
    } else if (inode->i_dtime >= sb->s_inodes_count) {
        return false;
    }
    inode->i_dtime = kSaneDtime;
    rpt->dtime_fixed++;
    return true;
}

static int fsck_dtime(ext2_filsys fs, const ExtfsFsckOptions &opt, FsckStats *rpt) {
    ext2_inode_scan scan = nullptr;
    errcode_t err = ext2fs_open_inode_scan(fs, kItableChunkBlocks, &scan);
    if (err) LOG_ERROR_RETURN(EIO, -1, "ext2fs_open_inode_scan failed, err=`", err);
    DEFER(ext2fs_close_inode_scan(scan));
    // groups whose inode table was never allocated are of no interest
    ext2fs_inode_scan_flags(scan, EXT2_SF_SKIP_MISSING_ITABLE, 0);

    const int isize = EXT2_INODE_SIZE(fs->super);
    char *buf = nullptr;
    err = ext2fs_get_mem(isize, &buf);
    if (err) LOG_ERROR_RETURN(ENOMEM, -1, "no memory for an inode");
    DEFER(ext2fs_free_mem(&buf));
    auto inode = (struct ext2_inode_large *)buf;

    while (true) {
        ext2_ino_t ino = 0;
        err = ext2fs_get_next_inode_full(scan, &ino, (struct ext2_inode *)inode, isize);
        if (err == EXT2_ET_BAD_BLOCK_IN_INODE_TABLE) continue;
        if (err) LOG_ERROR_RETURN(EIO, -1, "ext2fs_get_next_inode_full failed, err=`", err);
        if (!ino) break;
        rpt->inodes_scanned++;
        // the reserved inodes are accounted in the bitmap but left all zero,
        // and e2fsck does not check their dtime either
        if (ino < EXT2_FIRST_INODE(fs->super)) continue;
        if (!fix_dtime(fs->super, inode, rpt)) continue;
        LOG_DEBUG("fix i_dtime of inode `, links=`, mode=`", ino,
                  inode->i_links_count, HEX(inode->i_mode));
        if (opt.dry_run) continue;
        err = ext2fs_write_inode_full(fs, ino, (struct ext2_inode *)inode, isize);
        if (err)
            LOG_ERROR_RETURN(EIO, -1, "ext2fs_write_inode_full failed, ino=`, err=`", ino, err);
    }
    return 0;
}

// The never used tail of a group's inode table, which is exactly the region the
// kernel's lazy init would have zeroed. Returns the number of blocks.
static uint32_t itable_tail(ext2_filsys fs, dgrp_t g, blk64_t *first_block) {
    const __u32 ipg = EXT2_INODES_PER_GROUP(fs->super);
    const __u32 ipb = fs->blocksize / EXT2_INODE_SIZE(fs->super);
    __u32 unused = ext2fs_bg_itable_unused(fs, g);
    if (ext2fs_bg_flags_test(fs, g, EXT2_BG_INODE_UNINIT)) unused = ipg;
    if (unused > ipg) unused = ipg;
    __u32 used_blocks = (ipg - unused + ipb - 1) / ipb;
    if (used_blocks >= fs->inode_blocks_per_group) return 0;
    *first_block = ext2fs_inode_table_loc(fs, g) + used_blocks;
    return fs->inode_blocks_per_group - used_blocks;
}

static int verify_itable_tail(ext2_filsys fs, dgrp_t g, const ExtfsFsckOptions &opt,
                              char *buf, FsckStats *rpt) {
    blk64_t blk = 0;
    uint32_t left = itable_tail(fs, g, &blk);
    bool dirty = false;
    while (left) {
        int cnt = left < (uint32_t)kItableChunkBlocks ? (int)left : kItableChunkBlocks;
        size_t size = (size_t)cnt * fs->blocksize;
        errcode_t err = io_channel_read_blk64(fs->io, blk, cnt, buf);
        if (err)
            LOG_ERROR_RETURN(EIO, -1, "failed to read inode table of group `, err=`", g, err);
        rpt->itable_bytes_read += size;
        if (!is_zero(buf, size)) {
            if (!opt.dry_run) {
                memset(buf, 0, size);
                err = io_channel_write_blk64(fs->io, blk, cnt, buf);
                if (err)
                    LOG_ERROR_RETURN(EIO, -1, "failed to zero the inode table of group `, err=`", g, err);
            }
            dirty = true;
        }
        blk += cnt;
        left -= cnt;
    }
    if (dirty) {
        rpt->groups_itable_zeroed++;
        LOG_WARN("the inode table of group ` was not zeroed, `", g,
                 opt.dry_run ? "it would have been zeroed" : "zeroed it");
    }
    return 0;
}

// Finish the kernel's mount-time lazy init: verify (zeroing if needed) the
// never used tail of every inode table, then mark it INODE_ZEROED. make_extfs
// images are prezeroed, so this usually writes only the group descriptors.
static int fsck_lazy_init(ext2_filsys fs, const ExtfsFsckOptions &opt, FsckStats *rpt) {
    if (!ext2fs_has_group_desc_csum(fs)) {
        LOG_INFO("no group descriptor checksum feature, the kernel never lazy inits the inode table here");
        return 0;
    }
    char *buf = nullptr;
    errcode_t err = ext2fs_get_array(kItableChunkBlocks, fs->blocksize, &buf);
    if (err) LOG_ERROR_RETURN(ENOMEM, -1, "no memory for an inode table chunk");
    DEFER(ext2fs_free_mem(&buf));

    for (dgrp_t g = 0; g < fs->group_desc_count; ++g) {
        // INODE_ZEROED promises the unused tail reads back as zero, so verify
        // (and zero) it before setting the flag
        if (verify_itable_tail(fs, g, opt, buf, rpt) < 0) return -1;
        if (ext2fs_bg_flags_test(fs, g, EXT2_BG_INODE_ZEROED)) continue;
        if (!opt.dry_run) ext2fs_bg_flags_set(fs, g, EXT2_BG_INODE_ZEROED);
        rpt->groups_marked_zeroed++;
    }
    return 0;
}

// Nothing validates an internal journal's uuid, but jbd2 derives its checksum
// seed from it, so keep it in sync in case the journal ever gains the csum
// feature.
static int sync_journal_uuid(ext2_filsys fs, const ExtfsFsckOptions &opt,
                             FsckStats *rpt) {
    if (!ext2fs_has_feature_journal(fs->super) || !fs->super->s_journal_inum) return 0;

    struct ext2_inode inode;
    errcode_t err = ext2fs_read_inode(fs, fs->super->s_journal_inum, &inode);
    if (err) LOG_ERROR_RETURN(EIO, -1, "failed to read the journal inode, err=`", err);
    blk64_t blk = 0;
    err = ext2fs_bmap2(fs, fs->super->s_journal_inum, &inode, nullptr, 0, 0, nullptr, &blk);
    if (err || !blk)
        LOG_ERROR_RETURN(EIO, -1, "failed to map the journal superblock, err=`", err);

    char *buf = nullptr;
    err = ext2fs_get_mem(fs->blocksize, &buf);
    if (err) LOG_ERROR_RETURN(ENOMEM, -1, "no memory for the journal superblock");
    DEFER(ext2fs_free_mem(&buf));
    err = io_channel_read_blk64(fs->io, blk, 1, buf);
    if (err) LOG_ERROR_RETURN(EIO, -1, "failed to read the journal superblock, err=`", err);

    if (be32_at(buf, JSB_MAGIC) != JBD2_MAGIC)
        LOG_ERROR_RETURN(EUCLEAN, -1, "bad journal superblock magic");
    if (be32_at(buf, JSB_START) != 0)
        LOG_ERROR_RETURN(EUCLEAN, -1, "the journal is not empty (s_start=`)",
                         be32_at(buf, JSB_START));
    if (be32_at(buf, JSB_FEATURE_INCOMPAT) & JBD2_CSUM_FEATURES)
        LOG_ERROR_RETURN(EOPNOTSUPP, -1, "the journal has a checksum feature, its superblock checksum would have to be recomputed");
    if (memcmp(buf + JSB_UUID, fs->super->s_uuid, sizeof(fs->super->s_uuid)) == 0) return 0;

    LOG_INFO("updating the uuid of the internal journal superblock");
    if (opt.dry_run) return 0;
    memcpy(buf + JSB_UUID, fs->super->s_uuid, sizeof(fs->super->s_uuid));
    err = io_channel_write_blk64(fs->io, blk, 1, buf);
    if (err) LOG_ERROR_RETURN(EIO, -1, "failed to write the journal superblock, err=`", err);
    rpt->journal_uuid_synced = true;
    return 0;
}

static int fsck_uuid(ext2_filsys fs, const ExtfsFsckOptions &opt, FsckStats *rpt) {
    uuid4_string_t str;
    snprintf(str, sizeof(str), "%s", opt.uuid ? opt.uuid : DEFAULT_UUID);
    uuid4_t uuid;
    if (uuid4_parse(str, uuid) != 0)
        LOG_ERROR_RETURN(EINVAL, -1, "invalid uuid `", str);

    if (memcmp(fs->super->s_uuid, uuid, sizeof(uuid)) == 0) return 0;
    if (!uuid4_is_null((char *)fs->super->s_uuid) && !opt.force_uuid) {
        LOG_INFO("keeping the existing uuid, pass force_uuid to overwrite it");
        return 0;
    }
    uuid4_string_t old;
    uuid4_unparse_upper((char *)fs->super->s_uuid, old);
    LOG_INFO("setting the uuid to ` (was `)", str, old);
    if (opt.dry_run) return 0;
    memcpy(fs->super->s_uuid, uuid, sizeof(uuid));
    // the dir_index hash seed is an independent field, and the htrees on disk
    // were built with the old one: it must be left alone
    ext2fs_init_csum_seed(fs);
    rpt->uuid_changed = true;
    if (opt.sync_journal_uuid) return sync_journal_uuid(fs, opt, rpt);
    return 0;
}

// Recount free blocks and inodes from the bitmaps (e2fsck pass5 style) and
// correct the superblock and group descriptor totals, which a lost superblock
// write leaves at their stale mkfs values ("Free blocks count wrong"). Reads 2
// bitmap blocks per group and never dirties them.
static int fsck_free_counts(ext2_filsys fs, const ExtfsFsckOptions &opt,
                            FsckStats *rpt) {
    errcode_t err = ext2fs_read_bitmaps(fs);
    if (err) LOG_ERROR_RETURN(EIO, -1, "failed to read the bitmaps, err=`", err);
    rpt->bitmap_bytes_read += (uint64_t)fs->group_desc_count * 2 * fs->blocksize;

    const __u32 ipg = EXT2_INODES_PER_GROUP(fs->super);
    const ext2_ino_t inodes_count = fs->super->s_inodes_count;
    uint64_t total_free_blocks = 0, total_free_inodes = 0;

    for (dgrp_t g = 0; g < fs->group_desc_count; ++g) {
        __u32 free_blocks = 0, free_inodes = 0;
        blk64_t last = ext2fs_group_last_block2(fs, g);
        for (blk64_t b = ext2fs_group_first_block2(fs, g); b <= last; ++b)
            if (!ext2fs_fast_test_block_bitmap2(fs->block_map, b)) free_blocks++;
        for (__u32 i = 0; i < ipg; ++i) {
            ext2_ino_t ino = (ext2_ino_t)g * ipg + i + 1;
            if (ino > inodes_count) break;
            if (!ext2fs_fast_test_inode_bitmap2(fs->inode_map, ino)) free_inodes++;
        }
        total_free_blocks += free_blocks;
        total_free_inodes += free_inodes;

        if (ext2fs_bg_free_blocks_count(fs, g) == free_blocks &&
            ext2fs_bg_free_inodes_count(fs, g) == free_inodes)
            continue;
        LOG_WARN("group ` free counters wrong: blocks ` (counted `), inodes ` (counted `)",
                 g, ext2fs_bg_free_blocks_count(fs, g), free_blocks,
                 ext2fs_bg_free_inodes_count(fs, g), free_inodes);
        rpt->groups_free_counts_fixed++;
        if (opt.dry_run) continue;
        ext2fs_bg_free_blocks_count_set(fs, g, free_blocks);
        ext2fs_bg_free_inodes_count_set(fs, g, free_inodes);
    }

    if (ext2fs_free_blocks_count(fs->super) != total_free_blocks) {
        LOG_INFO("superblock free blocks wrong: ` (counted `)",
                 ext2fs_free_blocks_count(fs->super), total_free_blocks);
        rpt->free_blocks_fixed = true;
        if (!opt.dry_run) ext2fs_free_blocks_count_set(fs->super, total_free_blocks);
    }
    if (fs->super->s_free_inodes_count != total_free_inodes) {
        LOG_INFO("superblock free inodes wrong: ` (counted `)",
                 fs->super->s_free_inodes_count, total_free_inodes);
        rpt->free_inodes_fixed = true;
        if (!opt.dry_run) fs->super->s_free_inodes_count = (__u32)total_free_inodes;
    }
    // the counters live in the superblock and the group descriptors, neither of
    // which is written unless the fs is marked dirty
    if (!opt.dry_run &&
        (rpt->free_blocks_fixed || rpt->free_inodes_fixed || rpt->groups_free_counts_fixed))
        ext2fs_mark_super_dirty(fs);
    return 0;
}

int fsck_extfs(photon::fs::IFile *file, const ExtfsFsckOptions &opt) {
    FsckStats stats;
    auto rpt = &stats;

    auto manager = new_io_manager(file);
    ext2_filsys fs = nullptr;
    int flags = EXT2_FLAG_RW | EXT2_FLAG_64BITS;
    errcode_t err = ext2fs_open2("virtual-dev", nullptr, flags, 0, 0, manager, &fs);
    if (err) {
        // an image with a bogus uuid may well have stale group descriptor
        // checksums; we are going to recompute every one of them anyway
        LOG_WARN("ext2fs_open2 failed (err=`), retrying while ignoring checksum errors", err);
        err = ext2fs_open2("virtual-dev", nullptr, flags | EXT2_FLAG_IGNORE_CSUM_ERRORS,
                           0, 0, manager, &fs);
    }
    if (err) LOG_ERROR_RETURN(EIO, -1, "failed to open the image, err=`", err);
    DEFER(ext2fs_close_free(&fs));

    fs->default_bitmap_type = EXT2FS_BMAP64_RBTREE;
    // extfs_zeroout() is a no-op stub, any zeroing through it would be lost
    fs->io->manager->zeroout = nullptr;
    // keep s_wtime stable so that fixing up an image stays reproducible, and so
    // that e2fsck keeps skipping its low dtime heuristic
    fs->now = 1;

    if (fsck_preflight(fs) < 0) return -1;
    rpt->groups_total = fs->group_desc_count;

    auto plan = plan_itable_scan(fs);
    rpt->groups_hint_lost = plan.hint_lost;
    report_plan(fs, plan);

    if (opt.fix_dtime && fsck_dtime(fs, opt, rpt) < 0) return -1;
    rpt->itable_bytes_read += plan.prefix_bytes;
    if (opt.fix_uuid && fsck_uuid(fs, opt, rpt) < 0) return -1;
    if (opt.finish_lazy_init && fsck_lazy_init(fs, opt, rpt) < 0) return -1;
    if (opt.fix_free_counts && fsck_free_counts(fs, opt, rpt) < 0) return -1;

    // the group descriptor checksum covers the uuid, the flags and the counters,
    // so every group has to be recomputed once any of them changed
    if (!opt.dry_run && ext2fs_has_group_desc_csum(fs)) {
        for (dgrp_t g = 0; g < fs->group_desc_count; ++g)
            ext2fs_group_desc_csum_set(fs, g);
        ext2fs_mark_super_dirty(fs);
    }

    LOG_INFO("extfs fsck done`: dtime_fixed=`, dtime_cleared=`, uuid_changed=`, journal_uuid_synced=`",
             opt.dry_run ? " (dry run)" : "", rpt->dtime_fixed, rpt->dtime_cleared,
             rpt->uuid_changed, rpt->journal_uuid_synced);
    LOG_INFO("groups_marked_zeroed=`/`, groups_itable_zeroed=`, groups_free_counts_fixed=`",
             rpt->groups_marked_zeroed, rpt->groups_total, rpt->groups_itable_zeroed,
             rpt->groups_free_counts_fixed);
    LOG_INFO("free_blocks_fixed=`, free_inodes_fixed=`, inodes_scanned=`, itable_read=`MB, bitmap_read=`KB",
             rpt->free_blocks_fixed, rpt->free_inodes_fixed, rpt->inodes_scanned,
             rpt->itable_bytes_read >> 20, rpt->bitmap_bytes_read >> 10);
    return 0;
}

}
}
