# PhotonLibOS

[![CI](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.linux.x86.yml/badge.svg)](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.linux.x86.yml)
[![CI](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.linux.arm.yml/badge.svg)](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.linux.arm.yml)
[![CI](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.macos.yml/badge.svg)](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.macos.yml)

[PhotonlibOS.github.io](https://photonlibos.github.io)

## What's New
* Since 0.7, Photon will use release branches to enhance the reliability of software delivery. Bugfix will be merged into a stable release at first, then to higher release versions, and finally main.
* Since version 0.6, Photon can run with a userspace TCP/IP stack on top of `DPDK`.
[En](https://developer.aliyun.com/article/1208512) / [中文](https://developer.aliyun.com/article/1208390).
* How to transform `RocksDB` from multi-threads to coroutines by only 200 lines of code?
[En](https://github.com/facebook/rocksdb/issues/11017) / [中文](https://developer.aliyun.com/article/1093864).

<details><summary>Click to show more history...</summary><p>

* Version 0.5 is released. Except for various performance improvements, including spinlock, context switch,
  and new run queue for coroutine scheduling, we have re-implemented the HTTP module so that there is no `boost` dependency anymore.
* Version 0.4 has come, bringing us these three major features:
  1. Support coroutine local variables. Similar to the C++11 `thread_local` keyword. See [doc](doc/thread-local.md).
  2. Support running on macOS platform, both Intel x86_64 and Apple M1 included.
  3. Support LLVM Clang/Apple Clang/GCC compilers.
* Photon 0.3 was released on 2 Sep 2022. Except for bug fixes and improvements, a new `photon_std` namespace is added.
  Developers can search for `std::thread`, `std::mutex` in their own projects, and replace them all into the equivalents of `photon_std::<xxx>`.
  It's a quick way to transform thread-based programs to coroutine-based ones.
* Photon 0.2 was released on 28 Jul 2022. This release was mainly focused on network socket, security context and multi-vcpu support.
  We re-worked the `WorkPool` so it's more friendly now to write multi-vcpu programs.
* Made the first tag on 27 Jul 2022. Fix the compatibility for ARM CPU. Throughly compared the TCP echo server performance with other libs.

</p></details>

## Community

<img src="/doc/static/img/slack.svg" width="20"> Join Slack: [link](https://join.slack.com/t/photonlibos/shared_invite/zt-25wauq8g1-iK_oHMrXetcvWNNhIt8Nkg)

<img src="/doc/static/img/dingtalk.svg" width="20"> Join DingTalk group: 55690000272
