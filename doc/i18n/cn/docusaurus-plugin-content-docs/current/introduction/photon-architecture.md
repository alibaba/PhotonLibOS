---
sidebar_position: 2
toc_max_heading_level: 4
---

# 架构

Photon 的主要目标是处理并发与 I/O (包括文件和网络 I/O)

![architecture](/img/photon.png)

它的主要组成部分有：

* [Thread](../api/thread.md)，[vCPU](../api/vcpu-and-multicore.md)，锁和同步原语, 事件引擎，WorkPool任务分发 ...
* 多种文件IO封装: psync, posix_aio, libaio, io_uring
* 多种网络socket分封装: tcp (level-trigger/edge-trigger), unix-domain, zero-copy, libcurl, TLS support, etc.
* 高性能的 RPC client/server 和 HTTP client/server.
* POSIX 文件系统抽象和实现: local fs, http fs, fuse fs, e2fs, 等等
* 其他通用库，如 io-vector操作, 资源池, 对象池, 内存分配器, 回调代理，编译期logging模块, 无锁队列, defer, range lock, 限流模块, 等等