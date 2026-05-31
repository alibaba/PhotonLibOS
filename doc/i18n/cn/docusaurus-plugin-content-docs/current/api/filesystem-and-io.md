---
sidebar_position: 5
toc_max_heading_level: 4
---

# 文件系统和IO

Photon 对 file 和 fs 有 POSIX 兼容的封装。当然，你可以选择是否使用这套封装。

#### Namespace

`photon::fs::`

### 1. 使用封装

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

#### cachefs

待补充...

### 2. 使用裸API

#### aio

`<photon/io/aio-wrapper.h>`

支持 libaio 和 posixaio.

```cpp
// fd 必须用 O_DIRECT 打开, 并且内存必须对齐
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

#### io_uring

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
io_uring 的事件引擎必须在 [Env](./env#init) 环境里正确初始化
:::

## 核心接口

定义于 `<photon/fs/filesystem.h>`。

### IFile（扩展自 IStream）

带位置 I/O 接口：

- `pread` / `preadv` / `pwrite` / `pwritev` —— 位置读写
- `preadv2` / `pwritev2` —— 带标志（如 `RWF_NOWAIT`）
- `lseek`、`fsync`、`fdatasync`、`fstat`、`ftruncate`、`fallocate`
- `fiemap` —— extent 查询（被缓存层使用）
- `trim`、`zero_range` —— 丢弃/清零操作
- `append` / `appendv` —— 追加并输出位置
- `ioctl` / `vioctl` —— 可扩展操作

通过成员函数指针（`FuncPIO`、`FuncPIOCV` 等）进行分发。

### IFileSystem

类 POSIX 文件系统：

- `open`、`creat`、`mkdir`、`rmdir`、`symlink`、`readlink`、`link`、`rename`、`unlink`
- `chmod`、`chown`、`stat`、`lstat`、`access`、`truncate`
- `statfs`、`statvfs`、`opendir` + `DIR` 接口
- `FileList`：基于范围的目录项迭代

### IFileXAttr / IFileSystemXAttr

扩展属性：`getxattr`、`setxattr`、`listxattr`、`removexattr`。

## 实现

### 本地文件系统

`<photon/fs/localfs.h>`。多 I/O 引擎后端：

| 引擎 | 常量 | 描述 |
|------|------|------|
| psync | `ioengine_psync` (0) | 同步 POSIX I/O |
| libaio | `ioengine_libaio` (1) | Linux AIO（需要 `O_DIRECT`） |
| posixaio | `ioengine_posixaio` (2) | POSIX AIO |
| iouring | `ioengine_iouring` (3) | io_uring |

工厂函数：

```cpp
IFileSystem* new_localfs_adaptor(const char* root_path, int io_engine_type);
IFile* open_localfile_adaptor(const char* filename, int flags, mode_t mode, int io_engine_type);
```

### 虚拟文件

`<photon/fs/virtual-file.h>`。`VirtualFile` 自己管理 offset，并通过 `pread` / `pwrite` 实现 `read` / `write` / `readv` / `writev`。使用 `piov_copy` / `piov_nocopy` / `buffered_piov` 处理 iovector。

### 组合器

| 头文件 | 描述 |
|--------|------|
| `forwardfs.h` | `ForwardFile` / `ForwardFS` —— 装饰器模式：把所有操作转发给下层 |
| `subfs.h` | 路径前缀重映射（类似 chroot 的视图） |
| `throttled-file.h` | 限流文件 I/O |
| `aligned-file.h` | 直接 I/O 的缓冲区对齐 |
| `xfile.h` | 线性文件（拼接）、条带文件（类似 RAID-0） |
| `range-split.h` | 把 I/O 范围拆分为对齐的子范围 |

### HTTP 文件系统

`<photon/fs/httpfs/httpfs.h>`。

- `new_httpfs(default_https, conn_timeout, stat_expire)` —— 把 HTTP/HTTPS 视为文件系统。
- `new_httpfile()` —— 单个 HTTP 文件，支持 range 请求。
- v2 版本使用 Photon 自研的 HTTP 客户端，而不是 libcurl。

### FUSE 适配

`<photon/fs/fuse_adaptor/>`。把 Photon 文件系统暴露为 FUSE 挂载点。`session_loop.h` / `session_loop.cpp` 实现 FUSE 会话处理。

### extfs

`<photon/fs/extfs/>`。用户态 ext2/ext3/ext4 文件系统：`new_extfs(IFile* underlying_file)`。

## 异步文件系统

`<photon/fs/async_filesystem.h>`。

`IAsyncFile` / `IAsyncFileSystem` / `AsyncDIR`：使用 `DEFINE_ASYNC` 宏模式的异步 I/O。每个操作接受 `(done_callback, timeout)`。

适配器：

- `new_async_file_adaptors()` —— 把 async 包装为 sync
- `new_sync_file_adaptors()` —— 把阻塞操作委托给线程池
- `export_as_async_file` / `fs` / `dir()` —— 把 sync 导出为 async
- `export_as_sync_file` / `fs` / `dir()` —— 把 async 导出为 sync

### ExportFS

`<photon/fs/exportfs.h>`。`exportfs_init(thread_pool_capacity)` 创建用于跨线程文件系统访问的线程池。把同步对象导出为线程安全的 async/sync 包装，供外部 OS 线程使用。

## 缓存层

`<photon/fs/cache/>`。

### 架构

```
ICachedFileSystem → ICachePool → ICacheStore (每文件缓存)
                                      ├── RangeLock (并发访问)
                                      ├── fiemap (缓存的 extent 查询)
                                      └── 从源 FS 回填
```

### 关键类型

| 类型 | 描述 |
|------|------|
| `ICachePool` | 按文件名管理缓存存储：`open()`、`set_quota()`、`evict()`、`sync_pool()` |
| `ICacheStore` | 每文件缓存：`preadv2` / `pwritev2`，自动处理未命中、回填去重、`RangeLock` |
| `IMemCacheStore` / `IMemCachePool` | 带缓冲 pin 的内存缓存 |

### 缓存打开标志

`O_WRITE_THROUGH`、`O_WRITE_AROUND`、`O_WRITE_BACK`、`O_CACHE_ONLY`、`O_DIRECT_LOCAL`、`O_MMAP_READ`。

### 实现

| 实现 | 描述 |
|------|------|
| `full_file_cache/` | 以本地 FS 为后端的文件缓存，使用 `fiemap` 查询 extent，LRU 驱逐 |
| `policy/lru.h` | 基于数组的 LRU（无逐节点分配）：`push_front`、`access`、`pop_back` |
| `ocf_cache/` | Open CAS（Cache Acceleration Software）集成 |
| 持久化缓存 | 把数据块下载到后端存储，永不驱逐 |

## 设计决策

- **类 POSIX 接口。** `IFile` / `IFileSystem` 镜像 POSIX 语义，便于包装既有代码。
- **装饰器组合。** `ForwardFile` / `ForwardFS` 支持透明分层：cache → throttle → align → local。
- **用 fiemap 查询缓存。** 缓存层使用 `fiemap` 探查哪些字节范围已被缓存（对比空洞），避免单独的元数据存储。
- **RangeLock 提供并发。** 防止对同一字节范围的并发读取 —— 第一个调用者从源读取，其他调用者等待并从缓存读取。
- **多种 I/O 引擎。** 本地文件系统支持 psync / libaio / posixaio / iouring 后端，在创建时选择。