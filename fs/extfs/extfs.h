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
#include <cstdint>
#include <photon/fs/filesystem.h>
#include <stdint.h>

namespace photon {
namespace fs {


// when buffer is true, defaults to 8MB buffer size and 128KB block size
photon::fs::IFileSystem *new_extfs(photon::fs::IFile *file, bool buffer = true);

photon::fs::IFileSystem *new_extfs_with_buffer(photon::fs::IFile *file,
                                               uint32_t buffer_size = 8 << 20,
                                               uint32_t block_size = 128 << 10);

// make extfs on an prezeroed IFile,
// should be truncated to specified size in advance
int make_extfs(photon::fs::IFile *file, char *uuid = nullptr);

// Offline repair for images made by make_extfs. Every fix is idempotent, and
// the whole run refuses to touch an image that needs journal recovery, has a
// non-empty orphan list, is marked with errors, or has the metadata_csum
// feature. It addresses these legacy defects:
//   1. illegal i_dtime, e.g. left as 0 or 1 by old builds, which makes e2fsck
//      report ZERO_DTIME / LOW_DTIME (see photon #1169 and #1240);
//   2. all-zero s_uuid, caused by an invalid uuid string being silently
//      ignored (see photon #1581);
//   3. free block/inode counters that drifted away from the bitmaps, which a
//      superblock write lost to an incoherent write-back buffer leaves behind;
//   4. an unfinished lazy inode table init, which makes the kernel zero the
//      inode tables in the background on every rw mount (opt-in, see below).
struct ExtfsFsckOptions {
    bool fix_dtime = true;
    bool fix_uuid = true;           // only when s_uuid is all zero, unless force_uuid
    bool force_uuid = false;
    bool sync_journal_uuid = true;  // keep the internal journal's superblock in sync
    // recount the free blocks and inodes from the bitmaps, and correct the
    // superblock and group descriptors; reads 2 blocks per group
    bool fix_free_counts = true;
    // Finish the inode table lazy init the kernel would otherwise run on every
    // rw mount: verify the never used tail of every table reads back as zero,
    // zeroing it if not (which inflates a sparse image), then mark every group
    // INODE_ZEROED. Off by default; only a caller that knows its images carry
    // this defect should turn it on. Costs one pass over at most ~1.5% of the
    // image.
    bool finish_lazy_init = false;
    bool dry_run = false;           // report only, never write
    const char *uuid = nullptr;     // nullptr means the built-in default uuid
};

// returns 0 on success, or -1 with errno set on error. A summary of what was
// found and fixed, plus some cheap I/O accounting, is written to the log.
int fsck_extfs(photon::fs::IFile *file,
               const ExtfsFsckOptions &opt = ExtfsFsckOptions());

#ifdef PHOTON_ENABLE_RESIZE
int resize_extfs(photon::fs::IFile *file, uint64_t new_size, int flags = 0);
#endif

}
}
