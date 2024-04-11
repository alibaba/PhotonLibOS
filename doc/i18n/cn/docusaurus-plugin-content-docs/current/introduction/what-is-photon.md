---
sidebar_position: 1
toc_max_heading_level: 4
---

# 简介

**Photon**是一款高性能的LibOS框架，由一系列精心打造的模块组成。
虽然与std::thread的行为有别，但是```photon::thread```非常轻量而且快如
闪电---它可能是世界上最快的协程库。

我们相信没有什么比光子（photon）更加轻快。


### 协程运行时

Photon的运行时（runtime）基于协程实现。根据我们的评估，截止2022年它在开源届有着[**最佳**](../performance/network-performance#2-ping-pong)的性能表现，这个测试同时横跨了多个语言和框架。


### 核心特性

* 有栈协程，对称式调度器
* 非阻塞 IO 引擎，异步事件引擎，支持 epoll / kqueue / **io_uring**.
* 支持多平台和架构，如 x86 / ARM, Linux / macOS.
* 大量高效的汇编语言，在关键路径上减少开销
* API 完全兼容 C++ std 和 POSIX 标准，容易移植到旧代码

### 用户

一些开源项目在使用Photon，例如

- [containerd/overlaybd](https://github.com/containerd/overlaybd) DADI 镜像加速方案的存储后端，containerd 子项目
- [data-accelerator/photon-libtcmu](https://github.com/data-accelerator/photon-libtcmu) 一个基于 TCMU 实现的 iSCSI target
- [V语言](https://vlang.io/) 正在实验性地尝试使用Photon作为协程运行时 [link](https://github.com/vlang/v/blob/master/vlib/coroutines/coroutines.c.v)

当然，还有更多的闭源用户在通过Apache 2.0开源协议使用Photon。欢迎补充这个名单，如果你正在使用，或者仅仅是从我们的设计中得到了一些启发 :-)

### 景愿

我们希望捍卫同步程序设计范式自古以来的正义基础，即便我们面临现代的高并发需求。

我们希望改变高性能服务的世界，阻止“callback hell”事态不断的恶化，在生产
环境当中保护珍贵的宁静。

我们希望每天都能早早休息，一觉到天亮，不被监控告警打扰。

我们也希望与志同道合的你为伍，共同打造更好的明天。

### 历史

Photon最初于2018年诞生于阿里云存储的DADI团队，它是一个生产可用的库，并且已经被部署到数以十万计的机器上作为云上的基础设施。
**我们愿意承诺，只要这些软件还在演进，Photon就会得到持续的维护与更新。**
