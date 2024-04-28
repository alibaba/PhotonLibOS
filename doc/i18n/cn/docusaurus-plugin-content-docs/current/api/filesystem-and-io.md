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