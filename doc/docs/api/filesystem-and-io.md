---
sidebar_position: 5
toc_max_heading_level: 4
---

# Filesystem and IO

Photon has POSIX-like encapsulations for file and filesystem. You can choose to use the encapsulations or not.

#### Namespace

`photon::fs::`

### 1. Use the encapsulations

#### localfs

`<photon/fs/localfs.h>`

```cpp
auto fs = photon::fs::new_localfs_adaptor(".", photon::fs::ioengine_psync);
if (!fs) {
    LOG_ERRNO_RETURN(0, -1, "failed to create fs");
}
DEFER(delete fs);

auto file = fs->open("test-file", O_WRONLY | O_CREAT | O_TRUNC, 0644);
if (!file) {
    LOG_ERRNO_RETURN(0, -1, "failed to open file");
}
DEFER(delete file);

ssize_t n_written = file->write(buf, 4096);
```

#### fusefs

To be added...

#### cachefs

To be added...

### 2. Use the raw API

#### aio wrapper

`<photon/io/aio-wrapper.h>`

Support libaio and posixaio.

```cpp
// `fd` must be opened with O_DIRECT, and the buffers must be aligned
ssize_t libaio_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t libaio_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
ssize_t libaio_pwrite(int fd, const void *buf, size_t count, off_t offset);
ssize_t libaio_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);
static int libaio_fsync(int fd) { return 0; }

ssize_t posixaio_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t posixaio_pwrite(int fd, const void *buf, size_t count, off_t offset);
int posixaio_fsync(int fd);
int posixaio_fdatasync(int fd);
```

#### io_uring wrapper

`<photon/io/iouring-wrapper.h>`

```cpp
ssize_t iouring_pread(int fd, void* buf, size_t count, off_t offset, uint64_t timeout);
ssize_t iouring_pwrite(int fd, const void* buf, size_t count, off_t offset, uint64_t timeout);
ssize_t iouring_preadv(int fd, const iovec* iov, int iovcnt, off_t offset, uint64_t timeout);
ssize_t iouring_pwritev(int fd, const iovec* iov, int iovcnt, off_t offset, uint64_t timeout);
ssize_t iouring_send(int fd, const void* buf, size_t len, int flags, uint64_t timeout);
ssize_t iouring_recv(int fd, void* buf, size_t len, int flags, uint64_t timeout);
ssize_t iouring_sendmsg(int fd, const msghdr* msg, int flags, uint64_t timeout);
ssize_t iouring_recvmsg(int fd, msghdr* msg, int flags, uint64_t timeout);
int iouring_connect(int fd, const sockaddr* addr, socklen_t addrlen, uint64_t timeout);
int iouring_accept(int fd, sockaddr* addr, socklen_t* addrlen, uint64_t timeout);
int iouring_fsync(int fd);
int iouring_fdatasync(int fd);
int iouring_open(const char* path, int flags, mode_t mode);
int iouring_mkdir(const char* path, mode_t mode);
int iouring_close(int fd);
```

:::note
The IO engine must be set appropriately in [Env initialization](./env#init).
:::

## Core Interfaces

Defined in `<photon/fs/filesystem.h>`.

### IFile (extends IStream)

Positioned I/O interface:

- `pread` / `preadv` / `pwrite` / `pwritev` — positioned read/write
- `preadv2` / `pwritev2` — with flags (e.g. `RWF_NOWAIT`)
- `lseek`, `fsync`, `fdatasync`, `fstat`, `ftruncate`, `fallocate`
- `fiemap` — extent map query (used by the cache layer)
- `trim`, `zero_range` — discard/zero operations
- `append` / `appendv` — append with position output
- `ioctl` / `vioctl` — extensible operations

Member function pointers (`FuncPIO`, `FuncPIOCV`, etc.) are used for dispatch.

### IFileSystem

POSIX-like filesystem:

- `open`, `creat`, `mkdir`, `rmdir`, `symlink`, `readlink`, `link`, `rename`, `unlink`
- `chmod`, `chown`, `stat`, `lstat`, `access`, `truncate`
- `statfs`, `statvfs`, `opendir` + `DIR` interface
- `FileList`: range-based iteration over directory entries

### IFileXAttr / IFileSystemXAttr

Extended attributes: `getxattr`, `setxattr`, `listxattr`, `removexattr`.

## Implementations

### Local filesystem

`<photon/fs/localfs.h>`. Multiple I/O engine backends:

| Engine | Constant | Description |
|--------|----------|-------------|
| psync | `ioengine_psync` (0) | Synchronous POSIX I/O |
| libaio | `ioengine_libaio` (1) | Linux AIO (requires `O_DIRECT`) |
| posixaio | `ioengine_posixaio` (2) | POSIX AIO |
| iouring | `ioengine_iouring` (3) | io_uring |

Factory functions:

```cpp
IFileSystem* new_localfs_adaptor(const char* root_path, int io_engine_type);
IFile* open_localfile_adaptor(const char* filename, int flags, mode_t mode, int io_engine_type);
```

### Virtual file

`<photon/fs/virtual-file.h>`. `VirtualFile` manages its own offset and implements `read` / `write` / `readv` / `writev` via `pread` / `pwrite`. Uses `piov_copy` / `piov_nocopy` / `buffered_piov` for iovector handling.

### Composers

| Header | Description |
|--------|-------------|
| `forwardfs.h` | `ForwardFile` / `ForwardFS` — decorator pattern: forward all ops to underlay |
| `subfs.h` | Path prefix remapping (chroot-like view) |
| `throttled-file.h` | Rate-limited file I/O |
| `aligned-file.h` | Buffer alignment for direct I/O |
| `xfile.h` | Linear file (concatenation), stripe file (RAID-0-like) |
| `range-split.h` | Decompose I/O ranges into aligned sub-ranges |

### HTTP filesystem

`<photon/fs/httpfs/httpfs.h>`.

- `new_httpfs(default_https, conn_timeout, stat_expire)` — HTTP/HTTPS as a filesystem.
- `new_httpfile()` — single HTTP file with range request support.
- v2 versions use the native Photon HTTP client instead of libcurl.

### FUSE adaptor

`<photon/fs/fuse_adaptor/>`. Exposes photon filesystems as FUSE mounts. `session_loop.h` / `session_loop.cpp` implement the FUSE session processing.

### extfs

`<photon/fs/extfs/>`. Userspace ext2/ext3/ext4 filesystem: `new_extfs(IFile* underlying_file)`.

## Async Filesystem

`<photon/fs/async_filesystem.h>`.

`IAsyncFile` / `IAsyncFileSystem` / `AsyncDIR`: asynchronous I/O using the `DEFINE_ASYNC` macro pattern. Each operation takes `(done_callback, timeout)`.

Adaptors:

- `new_async_file_adaptors()` — wrap async → sync
- `new_sync_file_adaptors()` — delegate blocking ops to a thread pool
- `export_as_async_file` / `fs` / `dir()` — export sync → async
- `export_as_sync_file` / `fs` / `dir()` — export async → sync

### ExportFS

`<photon/fs/exportfs.h>`. `exportfs_init(thread_pool_capacity)` creates a thread pool for cross-thread filesystem access. Exports sync objects as thread-safe async/sync wrappers for use from external OS threads.

## Cache Layer

`<photon/fs/cache/>`.

### Architecture

```
ICachedFileSystem → ICachePool → ICacheStore (per-file cache)
                                      ├── RangeLock (concurrent access)
                                      ├── fiemap (cached extent query)
                                      └── refill from source FS
```

### Key types

| Type | Description |
|------|-------------|
| `ICachePool` | Per-filename cache store management: `open()`, `set_quota()`, `evict()`, `sync_pool()` |
| `ICacheStore` | Per-file cache: `preadv2` / `pwritev2` with automatic miss handling, refill deduplication, `RangeLock` |
| `IMemCacheStore` / `IMemCachePool` | Memory cache with buffer pinning |

### Cache open flags

`O_WRITE_THROUGH`, `O_WRITE_AROUND`, `O_WRITE_BACK`, `O_CACHE_ONLY`, `O_DIRECT_LOCAL`, `O_MMAP_READ`.

### Implementations

| Implementation | Description |
|----------------|-------------|
| `full_file_cache/` | File-backed cache using local FS, `fiemap` for extent queries, LRU eviction |
| `policy/lru.h` | Array-based LRU (no per-node allocation): `push_front`, `access`, `pop_back` |
| `ocf_cache/` | Open CAS (Cache Acceleration Software) integration |
| Persistent cache | Downloads chunks to backing store, never evicts |

## Design Decisions

- **POSIX-like interface.** `IFile` / `IFileSystem` mirror POSIX semantics, making it easy to wrap existing code.
- **Decorator composition.** `ForwardFile` / `ForwardFS` enable transparent layering: cache → throttle → align → local.
- **fiemap for cache queries.** The cache layer uses `fiemap` to discover which byte ranges are cached (vs holes), avoiding a separate metadata store.
- **RangeLock for concurrency.** Prevents concurrent reads of the same byte range — the first caller reads from the source, others wait and read from the cache.
- **Multiple I/O engines.** The local filesystem supports psync / libaio / posixaio / iouring backends, selected at creation time.