---
sidebar_position: 1
toc_max_heading_level: 4
---

# What is PhotonLibOS

**PhotonLibOS** (hereby refered to as **Photon**) is a high-efficiency LibOS framework, based on a set of carefully selected C++ libs.

### Coroutine runtime

Photon's runtime is driven by a coroutine lib. Out tests show that it has the [**best 🔗**](../performance/network-performance#2-ping-pong) IO performance in the open source world by the year of 2022, even among different programing languages.


### Core Features

* Stackful coroutine. Symmetric scheduler.
* Non-blocking IO engine. Async event engine. Support epoll / kqueue / **io_uring**.
* Support multiple platforms and architectures, x86 / ARM, Linux / macOS.
* Well-designed assembly code on the critical path, to reduce overhead.
* Fully compatible API toward C++ std and POSIX. Easy to migrate to legacy codebases.

### Users

Some open source projects are using Photon as well, for instance:

- [containerd/overlaybd](https://github.com/containerd/overlaybd) The storage backend of accelerated container image, providing a layering block-level image format, designed for container, secure container and virtual machine.
- [data-accelerator/photon-libtcmu](https://github.com/data-accelerator/photon-libtcmu) A TCMU implementation, reworked from tcmu-runner, acting as a iSCSI target.
- The [V language](https://vlang.io/) is trying Photon as an experimental coroutine runtime. [link](https://github.com/vlang/v/blob/master/vlib/coroutines/coroutines.v)

Any addition to this list is appreciated, if you have been using Photon, or just enlightened by its coroutine design.

### Vision

We hope that Photon could help programs run as fast and agile as the _photon particle_, which exactly is the naming came from.

### History

Photon was originally created from the storage team of Alibaba Cloud since 2018. It's a production ready library, and has
been deployed to hundreds of thousands of hosts as the infrastructure of cloud software. **We would like to make a
commitment that Photon will be continuously updated, as long as those cloud software still evolve**.
