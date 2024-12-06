---
sidebar_position: 2
toc_max_heading_level: 4
---

# Photon Architecture

The major goal of Photon is to handle concurrency and I/O (including file I/O and networking I/O).

![architecture](/img/photon.png)

Its components are:

* [Thread](../api/thread.md), [vCPU](../api/vcpu-and-multicore.md), timer, sync primitives, gdb extension, emulation of std::thread, task dispatching ...
* Multiple IO wrappers: psync, posix_aio, libaio, io_uring
* Multiple socket implementations: tcp (level-trigger/edge-trigger), unix-domain, zero-copy, libcurl, TLS support, etc.
* High performance RPC client/server, HTTP client/server.
* A POSIX-like filesystem abstraction and some implementations: local fs, http fs, fuse fs, e2fs, etc.
* Common utilities: io-vector manipulation, resource pool, object cache, mem allocator, callback delegator,
  pre-compiled logging, lockless ring buffer, defer, range lock, throttle, etc.