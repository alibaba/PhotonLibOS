---
sidebar_position: 2
toc_max_heading_level: 4
---

# Photon Architecture

The major goal of Photon is to handle concurrency and I/O (including file I/O and networking I/O).

![architecture](/img/photon.png)

Its components are:

* [Thread](../api/thread.md), [vCPU](../api/vcpu-and-multicore.md), locks and sync primitives, event engines, task dispatching ...
* Multiple IO wrappers: psync, posix_aio, libaio, io_uring
* Multiple socket implementations: tcp (level-trigger/edge-trigger), unix-domain, zero-copy, libcurl, TLS support, etc.
* High performance RPC client/server, HTTP client/server.
* A POSIX-like filesystem abstraction and some implementations: local fs, http fs, fuse fs, e2fs, etc.
* Common utilities: io-vector manipulation, resource pool, object cache, mem allocator, callback delegator,
  pre-compiled logging, lockless ring buffer, defer, range lock, throttle, etc.

## Layered Architecture

```
+--------------------------------------------------------------------+
|                      Application Code                              |
+--------------------------------------------------------------------+
|  go() / thread_create11() / thread_create()  |  photon_std::thread |
+--------------------------------------------------------------------+
|                   photon::thread (coroutine)                       |
|  Scheduler (RunQ/SleepQ/StandbyQ) | Sync primitives | channel<T>   |
+--------------------------------------------------------------------+
|                        vCPU (OS Thread)                            |
|  MasterEventEngine (epoll/io_uring/kqueue) + Idle Worker           |
+--------------------------------------------------------------------+
|                    WorkPool (multi-vCPU)                           |
|  std::thread[] + LockfreeMPMCRingQueue for task dispatch           |
+--------------------------------------------------------------------+
|   fs/ (filesystem)  |  net/ (sockets, HTTP)  |  rpc/ (RPC)         |
+--------------------------------------------------------------------+
|              common/ (utilities, logging, containers)              |
+--------------------------------------------------------------------+
```

From top to bottom:

1. **Application code** uses Photon's coroutine primitives (`go`, `thread_create11`, `photon_std::thread`) to express concurrency.
2. **Coroutine layer** schedules `photon::thread` instances on per-vCPU queues, with sync primitives (`mutex`, `semaphore`, `condition_variable`, `rwlock`) and Go-style `channel<T>`.
3. **vCPU layer** runs the idle worker coroutine, which drives the `MasterEventEngine` (epoll, io_uring, kqueue, or select) when no photon thread is runnable.
4. **WorkPool** manages multiple vCPUs and dispatches tasks across them via a lock-free MPMC queue.
5. **Service modules** (`fs`, `net`, `rpc`) provide filesystem, networking, and RPC abstractions.
6. **common/** holds cross-cutting utilities: logging, strings, containers, delegates, I/O vectors, checksums.

## Module Summary

| Module | Purpose | Key files |
|--------|---------|-----------|
| [thread/](../api/thread.md) | Coroutine runtime, scheduler, sync primitives, channels | `thread.h`, `thread.cpp`, `go.h`, `thread-pool.h`, `workerpool.h` |
| [io/](../api/vcpu-and-multicore.md#event-engines) | Event engine backends (epoll, io_uring, kqueue), signal handling | `fd-events.h`, `epoll.cpp`, `iouring-wrapper.cpp`, `kqueue.cpp` |
| [net/](../api/network.md) | Socket abstractions, HTTP client/server, TLS, connection pooling | `socket.h`, `http/client.h`, `http/server.h`, `security-context/tls-stream.h` |
| [fs/](../api/filesystem-and-io.md) | Filesystem VFS, local fs, HTTP fs, caching layer | `filesystem.h`, `localfs.h`, `cache/cache.h` |
| [rpc/](../api/rpc.md) | High-performance RPC with zero-copy serialization | `rpc.h`, `serialize.h`, `out-of-order-execution.h` |
| [common/](../api/common.md) | Logging, string utilities, containers, delegates, async helpers | `alog.h`, `estring.h`, `callback.h`, `iovector.h`, `lockfree_queue.h` |
| [ecosystem/](../ecosystem/overview.md) | Redis client, OSS client, JSON/XML/YAML parsing | `redis.h`, `oss.h`, `simple_dom.h` |

## Key Design Patterns

**Interface segregation.** Abstract interfaces (`IStream`, `IFile`, `IFileSystem`, `ISocketStream`, `ISocketClient`, `ISocketServer`) separate concerns and enable composition.

**Decorator pattern.** `ForwardFile`, `ForwardFS`, `ForwardSocket*` wrap underlay objects transparently — used for TLS, caching, throttling, and alignment.

**Factory functions.** `new_*_client()`, `new_*_server()`, `new_localfs_adaptor()`, etc. create concrete implementations behind abstract interfaces.

**Strategy pattern.** Pluggable event engines (epoll / io_uring / kqueue) and I/O engines (psync / libaio / iouring) implement the same interface, selected at creation time.

**Delegate callbacks.** `Delegate<R, Ts...>` provides zero-overhead callable wrappers for handlers, body writers, and async completions. See [Delegate and Callback](../api/delegate_callback.md).

**Intrusive collections.** `intrusive_list_node<T>` is used for reset handles, pooled sockets, and scheduler queues to avoid per-node allocation.

## Coroutine and I/O Integration

All I/O integrates with the coroutine system via event-driven suspension:

1. A syscall returns `EAGAIN` → the wrapper calls `wait_for_fd_readable()` or `wait_for_fd_writable()`.
2. The current photon thread suspends; the FD is registered with the `MasterEventEngine`.
3. When the event arrives, `thread_interrupt()` moves the thread from the sleep queue to the run queue.
4. The scheduler resumes the thread, and the syscall is retried.

This pattern appears in `net/basic_socket.h` (the `doio_once` template) and is reused throughout the codebase.

## Conventions

- **Ownership.** Wrapper objects accept an `ownership` parameter (typically `bool`) that controls whether the wrapped object is deleted on destruction.
- **Timeout.** All I/O operations accept `Timeout` objects integrated with `photon::now` (a microsecond-precision global timestamp).
- **Error handling.** Functions return `-1` or `nullptr` on error, set `errno`, and log via `LOG_ERROR_RETURN`.
- **Thread safety.** Photon sync primitives (`mutex`, `semaphore`, `condition_variable`) are coroutine-aware; their `std::` equivalents would block the underlying OS thread.
