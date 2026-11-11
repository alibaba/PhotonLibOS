# extfs

An ext2/3/4 filesystem built on top of a Photon `IFile`, backed by `libext2fs`.
It lets you create, mount and repair an ext image without a loop device or root
privileges. See [extfs.h](extfs.h) for the full API.

## `fsck_extfs` — offline image repair

`fsck_extfs()` is a small, purpose-built checker, **not** a general replacement
for `e2fsck`. It repairs a handful of well understood defects that Photon's own
`make_extfs` used to leave in images, plus one opt-in cleanup. Every fix is
idempotent, so running it twice is a no-op, and it never rewrites metadata it
did not have to touch.

```cpp
photon::fs::ExtfsFsckOptions opt;   // sane defaults
int ret = photon::fs::fsck_extfs(image, opt);
```

`ret` is `0` on success, or `-1` with `errno` set. A summary of what was found
and fixed is written to the log. Pass `opt.dry_run = true` to see what it would
do without writing anything.

### What it refuses to touch

To stay safe, the whole run bails out (returns `-1`) before making any change
if the image:

- needs journal recovery — replay it first, or the replay would undo our fixes;
- has a non-empty orphan inode list — those inodes chain through `i_dtime`,
  which we must not disturb (`errno == EBUSY`);
- is marked with errors or was not unmounted cleanly — run `e2fsck` first;
- has the `metadata_csum` feature — changing the uuid would mean rewriting every
  metadata checksum, which is out of scope.

### Options

| Option | Default | What it fixes |
| --- | --- | --- |
| `fix_dtime` | `true` | Illegal `i_dtime`: a deleted inode left with `0` or a low value (which `e2fsck` reports as `ZERO_DTIME` / `LOW_DTIME`), or an in-use inode carrying a stale `dtime`. Deleted inodes get a sane `0xFFFFFFFF`. See PhotonLibOS [#1169](https://github.com/alibaba/PhotonLibOS/pull/1169) and [#1240](https://github.com/alibaba/PhotonLibOS/pull/1240). |
| `fix_uuid` | `true` | An all-zero `s_uuid`, left behind when an invalid uuid string was silently ignored. Sets the built-in default uuid (or `opt.uuid`). Only touches an all-zero uuid unless `force_uuid` is also set. See PhotonLibOS [#1581](https://github.com/alibaba/PhotonLibOS/pull/1581). |
| `force_uuid` | `false` | Overwrite the uuid even when it is already non-zero. |
| `sync_journal_uuid` | `true` | When the uuid changes, keep the internal journal superblock's uuid in sync, so its checksum seed stays consistent. |
| `uuid` | `nullptr` | The uuid string to write; `nullptr` means the built-in default. |
| `fix_free_counts` | `true` | Free block/inode counters in the superblock and group descriptors that drifted away from the bitmaps — what a superblock write lost to an incoherent write-back buffer leaves behind, reported by `e2fsck` as "Free blocks/inodes count wrong". Recounted from the bitmaps, `e2fsck` pass-5 style. Reads 2 blocks per group. |
| `finish_lazy_init` | `false` | Finish the inode-table lazy init the kernel would otherwise run on every rw mount: verify the never-used tail of every inode table reads back as zero (zeroing it if not, which inflates a sparse image), then mark every group `INODE_ZEROED`. **Opt-in**: only enable it if you know your images carry this defect. Costs one pass over at most ~1.5% of the image. |
| `dry_run` | `false` | Report only, never write. |

### The log summary

At the end of a run, `fsck_extfs` logs a one-line summary of what it fixed (e.g.
`dtime_fixed`, `uuid_changed`, `free_blocks_fixed`, `groups_marked_zeroed`)
along with some cheap I/O accounting.

### A note on htree directories

Some legacy images can carry a directory with the htree index flag set over an
incomplete tree, which `e2fsck` reports as an invalid HTREE directory.
`fsck_extfs` deliberately does not handle this: detecting it safely would mean
re-implementing `e2fsck`'s full htree validation, and clearing the flag on a
sound htree by mistake would hide all of its entries. Repair such an image with
`e2fsck` instead.
