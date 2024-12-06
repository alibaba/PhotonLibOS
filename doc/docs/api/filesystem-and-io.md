---
sidebar_position: 5
toc_max_heading_level: 4
---

# Filesystem and IO

Photon has POSIX-like encapsulations for file and filesystem. You can choose to use the encapsulations or not.

### 1. Use the encapsulations

#### Namespace

`photon::fs::`

#### Headers

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