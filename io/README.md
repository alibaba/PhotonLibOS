# I/O lib

### File descriptor event engine（io/fd-events.h)

All fd events are processed in the event engine, which is basically a polling system (POLLIN, POLLOUT, POLLERR). Both epoll and io_uring backends are supported.

The event engine is natively integrated into thread scheduling. Usually we would do a non-blocking `wait` for events on the current working thread, and then it would turn into SLEEPING. After the events being processed, it's the engine's responsibility to interrupt the previous thread and make it READY.

#### API
```cpp
namespace photon {
    int fd_events_epoll_init();
    int fd_events_epoll_fini();
    
    int fd_events_iouring_init();
    int fd_events_iouring_fini();
    
    // Sleep current thread.
    // Wait until fd becomes readable/writable within a maximum timeout.
    int wait_for_fd_readable(int fd, uint64_t timeout = -1)
    int wait_for_fd_writable(int fd, uint64_t timedout = -1);
}
```

### aio wrapper (io/aio-wrapper.h)

Support libaio and posixaio.

```cpp
namespace photon {
    int libaio_wrapper_init();
    int libaio_wrapper_fini();

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
}
```

### io_uring wrapper (io/iouring-wrapper.h)

Even though io_uring is fully async and doesn't require the `wait` operation, we still need to initialize the event engine.

```cpp
namespace photon {
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
}
```