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
#include "../extfs.h"
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <photon/photon.h>
#include <photon/fs/localfs.h>
#include <photon/common/alog.h>
#include <photon/common/utility.h>
#include <photon/common/uuid4.h>
#include <gtest/gtest.h>

#define IMAGE_SIZE (256UL << 20)
static const char *IMAGE_PATH = "/tmp/extfs_fsck_test.img";
static const char *DEFAULT_UUID_STR = "bdf7bb2e-c231-43ce-87c2-299792458ef5";

namespace photon {
namespace fs {
extern io_manager new_io_manager(photon::fs::IFile *file);
}
}

// Open the image with libext2fs directly, so that a test can both fabricate the
// defects of an old image and inspect what fsck_extfs did about them. Only one
// of these may be alive at a time, the io manager keeps a single backend file.
static ext2_filsys open_raw(photon::fs::IFile *image) {
    auto manager = photon::fs::new_io_manager(image);
    ext2_filsys fs = nullptr;
    auto err = ext2fs_open2("virtual-dev", nullptr,
                            EXT2_FLAG_RW | EXT2_FLAG_64BITS | EXT2_FLAG_IGNORE_CSUM_ERRORS,
                            0, 0, manager, &fs);
    if (err) {
        LOG_ERROR("ext2fs_open2 failed, err=`", err);
        return nullptr;
    }
    fs->io->manager->zeroout = nullptr;
    fs->now = 1;
    return fs;
}

static uint32_t get_dtime(photon::fs::IFile *image, ext2_ino_t ino) {
    auto fs = open_raw(image);
    if (!fs) return 0;
    DEFER(ext2fs_close_free(&fs));
    struct ext2_inode inode;
    if (ext2fs_read_inode(fs, ino, &inode)) return 0;
    return inode.i_dtime;
}

static void set_dtime(photon::fs::IFile *image, ext2_ino_t ino, uint32_t dtime) {
    auto fs = open_raw(image);
    ASSERT_NE(nullptr, fs);
    DEFER(ext2fs_close_free(&fs));
    struct ext2_inode inode;
    ASSERT_EQ(0, ext2fs_read_inode(fs, ino, &inode));
    inode.i_dtime = dtime;
    ASSERT_EQ(0, ext2fs_write_inode(fs, ino, &inode));
}

// Bring an image back to the shape the old mkfs left behind: an all-zero uuid,
// plus the gdt_csum feature with no inode table declared as zeroed, which is
// what makes the kernel run its lazy inode table init on every rw mount.
static void make_legacy(photon::fs::IFile *image) {
    auto fs = open_raw(image);
    ASSERT_NE(nullptr, fs);
    DEFER(ext2fs_close_free(&fs));

    memset(fs->super->s_uuid, 0, sizeof(fs->super->s_uuid));
    ext2fs_set_feature_gdt_csum(fs->super);
    const __u32 ipg = EXT2_INODES_PER_GROUP(fs->super);
    for (dgrp_t g = 0; g < fs->group_desc_count; ++g) {
        // group 0 holds the reserved inodes, the root dir and lost+found
        ext2fs_bg_itable_unused_set(fs, g, g == 0 ? ipg - 16 : ipg);
        ext2fs_bg_flags_clear(fs, g, EXT2_BG_INODE_ZEROED);
        if (g) ext2fs_bg_flags_set(fs, g, EXT2_BG_INODE_UNINIT);
        ext2fs_group_desc_csum_set(fs, g);
    }
    ext2fs_mark_super_dirty(fs);
}

class ExtfsFsckTest : public ::testing::Test {
protected:
    photon::fs::IFile *image = nullptr;
    ext2_ino_t keeper = 0;              // a file that survives, with a stale dtime
    std::vector<ext2_ino_t> deleted;    // deleted inodes with an illegal dtime
    uint32_t inodes_count = 0;
    uint64_t good_free_blocks = 0;      // the free counters before they are skewed
    uint32_t good_free_inodes = 0;

    void SetUp() override {
        image = photon::fs::open_localfile_adaptor(IMAGE_PATH, O_RDWR | O_CREAT | O_TRUNC,
                                                  0644, 0);
        ASSERT_NE(nullptr, image);
        ASSERT_EQ(0, image->ftruncate(IMAGE_SIZE));
        ASSERT_EQ(0, photon::fs::make_extfs(image));
        ASSERT_NO_FATAL_FAILURE(populate());
        ASSERT_NO_FATAL_FAILURE(make_legacy(image));
        ASSERT_NO_FATAL_FAILURE(break_dtimes());
        ASSERT_NO_FATAL_FAILURE(break_free_counts());
    }

    void TearDown() override {
        delete image;
        image = nullptr;
        ::unlink(IMAGE_PATH);
    }

    // Create every file up front, so that unlinking does not let a later creat
    // reuse an inode we still want to look at, then delete some of them to get
    // inodes that carry a dtime.
    void populate() {
        auto extfs = photon::fs::new_extfs(image, false);
        ASSERT_NE(nullptr, extfs);
        DEFER(delete extfs);

        auto file = extfs->creat("/keeper", 0644);
        ASSERT_NE(nullptr, file);
        ASSERT_EQ(6, file->pwrite("keeper", 6, 0));
        delete file;
        struct stat st;
        ASSERT_EQ(0, extfs->stat("/keeper", &st));
        keeper = st.st_ino;

        for (int i = 0; i < 3; ++i) {
            char path[64];
            snprintf(path, sizeof(path), "/deleted%d", i);
            auto f = extfs->creat(path, 0644);
            ASSERT_NE(nullptr, f);
            ASSERT_EQ(4, f->pwrite("data", 4, 0));
            delete f;
            ASSERT_EQ(0, extfs->stat(path, &st));
            deleted.push_back(st.st_ino);
        }
        for (int i = 0; i < 3; ++i) {
            char path[64];
            snprintf(path, sizeof(path), "/deleted%d", i);
            ASSERT_EQ(0, extfs->unlink(path));
        }
    }

    // the illegal states an old build could leave behind, plus the one e2fsck
    // calls "inode is in use, but has dtime set"
    void break_dtimes() {
        auto fs = open_raw(image);
        ASSERT_NE(nullptr, fs);
        inodes_count = fs->super->s_inodes_count;
        ext2fs_close_free(&fs);

        ASSERT_NO_FATAL_FAILURE(set_dtime(image, deleted[0], 0));
        ASSERT_NO_FATAL_FAILURE(set_dtime(image, deleted[1], 1));
        ASSERT_NO_FATAL_FAILURE(set_dtime(image, deleted[2], inodes_count - 1));
        ASSERT_NO_FATAL_FAILURE(set_dtime(image, keeper, 123456));
    }

    // What a superblock write lost to an incoherent write-back buffer leaves
    // behind: the totals in the superblock stay at their pre-session values,
    // while the group descriptors and the bitmaps are up to date.
    void break_free_counts() {
        auto fs = open_raw(image);
        ASSERT_NE(nullptr, fs);
        DEFER(ext2fs_close_free(&fs));
        good_free_blocks = ext2fs_free_blocks_count(fs->super);
        good_free_inodes = fs->super->s_free_inodes_count;
        ext2fs_free_blocks_count_set(fs->super, good_free_blocks + 511);
        fs->super->s_free_inodes_count = good_free_inodes + 9;
        ext2fs_mark_super_dirty(fs);
    }
};

TEST_F(ExtfsFsckTest, fix_everything) {
    ASSERT_EQ(3u, deleted.size());
    EXPECT_NE(deleted[0], deleted[1]);
    EXPECT_NE(deleted[1], deleted[2]);

    photon::fs::ExtfsFsckOptions opt;
    opt.finish_lazy_init = true;   // exercise the opt-in inode table pass
    ASSERT_EQ(0, photon::fs::fsck_extfs(image, opt));

    for (auto ino : deleted)
        EXPECT_EQ(0xFFFFFFFFu, get_dtime(image, ino));
    EXPECT_EQ(0u, get_dtime(image, keeper));

    auto fs = open_raw(image);
    ASSERT_NE(nullptr, fs);
    DEFER(ext2fs_close_free(&fs));
    uuid4_t expected;
    char str[37];
    snprintf(str, sizeof(str), "%s", DEFAULT_UUID_STR);
    ASSERT_EQ(0, uuid4_parse(str, expected));
    EXPECT_EQ(0, memcmp(fs->super->s_uuid, expected, sizeof(expected)));
    // the htrees on disk were hashed with the old seed, it must be left alone
    for (size_t i = 0; i < 4; ++i) EXPECT_EQ(0u, fs->super->s_hash_seed[i]);
    for (dgrp_t g = 0; g < fs->group_desc_count; ++g) {
        EXPECT_NE(0, ext2fs_bg_flags_test(fs, g, EXT2_BG_INODE_ZEROED));
        EXPECT_NE(0, ext2fs_group_desc_csum_verify(fs, g));
    }
    // the free counters are back to what the bitmaps say
    EXPECT_EQ(good_free_blocks, ext2fs_free_blocks_count(fs->super));
    EXPECT_EQ(good_free_inodes, fs->super->s_free_inodes_count);
}

TEST_F(ExtfsFsckTest, idempotent) {
    photon::fs::ExtfsFsckOptions opt;
    opt.finish_lazy_init = true;
    ASSERT_EQ(0, photon::fs::fsck_extfs(image, opt));

    uint64_t free_blocks;
    uint32_t free_inodes;
    uint8_t uuid[16];
    {
        auto fs = open_raw(image);
        ASSERT_NE(nullptr, fs);
        DEFER(ext2fs_close_free(&fs));
        free_blocks = ext2fs_free_blocks_count(fs->super);
        free_inodes = fs->super->s_free_inodes_count;
        memcpy(uuid, fs->super->s_uuid, sizeof(uuid));
    }

    // a second run must find nothing left to change
    ASSERT_EQ(0, photon::fs::fsck_extfs(image, opt));
    for (auto ino : deleted) EXPECT_EQ(0xFFFFFFFFu, get_dtime(image, ino));
    EXPECT_EQ(0u, get_dtime(image, keeper));
    auto fs = open_raw(image);
    ASSERT_NE(nullptr, fs);
    DEFER(ext2fs_close_free(&fs));
    EXPECT_EQ(free_blocks, ext2fs_free_blocks_count(fs->super));
    EXPECT_EQ(free_inodes, fs->super->s_free_inodes_count);
    EXPECT_EQ(0, memcmp(uuid, fs->super->s_uuid, sizeof(uuid)));
    for (dgrp_t g = 0; g < fs->group_desc_count; ++g)
        EXPECT_NE(0, ext2fs_bg_flags_test(fs, g, EXT2_BG_INODE_ZEROED));
}

TEST_F(ExtfsFsckTest, dry_run_changes_nothing) {
    photon::fs::ExtfsFsckOptions opt;
    opt.finish_lazy_init = true;
    opt.dry_run = true;
    ASSERT_EQ(0, photon::fs::fsck_extfs(image, opt));

    // every defect is still there after a dry run
    EXPECT_EQ(0u, get_dtime(image, deleted[0]));
    EXPECT_EQ(1u, get_dtime(image, deleted[1]));
    {
        auto fs = open_raw(image);
        ASSERT_NE(nullptr, fs);
        DEFER(ext2fs_close_free(&fs));
        EXPECT_TRUE(uuid4_is_null((char *)fs->super->s_uuid));
        EXPECT_EQ(good_free_blocks + 511, ext2fs_free_blocks_count(fs->super));
        EXPECT_EQ(good_free_inodes + 9, fs->super->s_free_inodes_count);
    }

    // and a real run then fixes them
    photon::fs::ExtfsFsckOptions wet_opt;
    wet_opt.finish_lazy_init = true;
    ASSERT_EQ(0, photon::fs::fsck_extfs(image, wet_opt));
    for (auto ino : deleted) EXPECT_EQ(0xFFFFFFFFu, get_dtime(image, ino));
    auto fs = open_raw(image);
    ASSERT_NE(nullptr, fs);
    DEFER(ext2fs_close_free(&fs));
    EXPECT_EQ(good_free_blocks, ext2fs_free_blocks_count(fs->super));
    EXPECT_EQ(good_free_inodes, fs->super->s_free_inodes_count);
}

TEST_F(ExtfsFsckTest, refuse_when_orphan_list_is_not_empty) {
    {
        auto fs = open_raw(image);
        ASSERT_NE(nullptr, fs);
        DEFER(ext2fs_close_free(&fs));
        fs->super->s_last_orphan = keeper;
        ext2fs_mark_super_dirty(fs);
    }
    ASSERT_EQ(-1, photon::fs::fsck_extfs(image));
    EXPECT_EQ(EBUSY, errno);
    // an orphan link lives in i_dtime, so nothing may have been touched
    EXPECT_EQ(0u, get_dtime(image, deleted[0]));
}

TEST_F(ExtfsFsckTest, keep_an_existing_uuid_unless_forced) {
    uuid4_t mine;
    char str[37] = "12345678-1234-4321-8765-123456789abc";
    ASSERT_EQ(0, uuid4_parse(str, mine));
    {
        auto fs = open_raw(image);
        ASSERT_NE(nullptr, fs);
        DEFER(ext2fs_close_free(&fs));
        memcpy(fs->super->s_uuid, mine, sizeof(mine));
        ext2fs_mark_super_dirty(fs);
    }

    // a plain run keeps the existing non-zero uuid
    ASSERT_EQ(0, photon::fs::fsck_extfs(image, photon::fs::ExtfsFsckOptions()));
    {
        auto fs = open_raw(image);
        ASSERT_NE(nullptr, fs);
        DEFER(ext2fs_close_free(&fs));
        EXPECT_EQ(0, memcmp(fs->super->s_uuid, mine, sizeof(mine)));
    }

    // force_uuid overwrites it with the default
    photon::fs::ExtfsFsckOptions opt;
    opt.force_uuid = true;
    ASSERT_EQ(0, photon::fs::fsck_extfs(image, opt));
    auto fs = open_raw(image);
    ASSERT_NE(nullptr, fs);
    DEFER(ext2fs_close_free(&fs));
    uuid4_t def;
    char str2[37];
    snprintf(str2, sizeof(str2), "%s", DEFAULT_UUID_STR);
    ASSERT_EQ(0, uuid4_parse(str2, def));
    EXPECT_EQ(0, memcmp(fs->super->s_uuid, def, sizeof(def)));
}

TEST_F(ExtfsFsckTest, image_is_still_usable) {
    ASSERT_EQ(0, photon::fs::fsck_extfs(image));

    auto extfs = photon::fs::new_extfs(image, false);
    ASSERT_NE(nullptr, extfs);
    DEFER(delete extfs);
    auto file = extfs->open("/keeper", O_RDONLY);
    ASSERT_NE(nullptr, file);
    char buf[16] = {};
    EXPECT_EQ(6, file->pread(buf, 6, 0));
    EXPECT_STREQ("keeper", buf);
    delete file;

    auto again = extfs->creat("/after_fsck", 0644);
    ASSERT_NE(nullptr, again);
    EXPECT_EQ(5, again->pwrite("hello", 5, 0));
    delete again;
}

int main(int argc, char **argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_DEFAULT);
    DEFER(photon::fini());
    set_log_output_level(1);

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
