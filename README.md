# Photon

[![CI](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.yml/badge.svg)](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.yml)

Photon is a high-efficiency LibOS framework, based on a set of carefully selected C++ libs.

The role of LibOS is to connect user apps and the OS. Following the principle of Least Astonishment,
we designed Photon's API to be as consistent as possible with C++ std and POSIX semantics.
This flattens the learning curve for lib users and brings convenience when migrating legacy codebases.

Photon's runtime is driven by a coroutine lib. Out tests show that it has the **best** I/O performance in the
open source world by the year of 2022, even among different programing languages.

As to the project vision, we hope that Photon would help programs run as *fast* and *agile*
as the [photon](https://en.wikipedia.org/wiki/Photon) particle, which exactly is the naming came from.

## What's New

* Version 0.5 is released. Except for various performance improvements, including spinlock, context switch,
and new run queue for coroutine scheduling, we have re-implemented the HTTP module so that there is no `boost` dependency anymore.
* How to transform RocksDB from multi-threads to coroutines with the help of Photon?
Here is the article, [en](https://www.reddit.com/r/cpp/comments/zd2hx1/200_lines_of_code_to_rewrite_the_600000_lines/) /
[中文](https://developer.aliyun.com/article/1093864).

<details><summary>More history...</summary><p>

* Version 0.4 has come, bringing us these three major features:
  1. Support coroutine local variables. Similar to the C++11 `thread_local` keyword. See [doc](doc/thread-local.md).
  2. Support running on macOS platform, both Intel x86_64 and Apple M1 included.
  3. Support LLVM Clang/Apple Clang/GCC compilers.
* Photon 0.3 was released on 2 Sep 2022. Except for bug fixes and improvements, a new `photon::std` namespace is added.
  Developers can search for `std::thread`, `std::mutex` in their own projects, and replace them all into the equivalents of `photon::std::<xxx>`.
  It's a quick way to transform thread-based programs to coroutine-based ones.
* Photon 0.2 was released on 28 Jul 2022. This release was mainly focused on network socket, security context and multi-vcpu support.
  We re-worked the `WorkPool` so it's more friendly now to write multi-vcpu programs.
* Made the first tag on 27 Jul 2022. Fix the compatibility for ARM CPU. Throughly compared the TCP echo server performance with other libs.

</p></details>

## Features
* Coroutine library (support multi-core)
* Async event engine, natively integrated into coroutine scheduling (support epoll or [io_uring](https://github.com/axboe/liburing))
* Multiple I/O wrappers: psync, posix_aio, libaio, io_uring
* Multiple socket implementations: tcp (level-trigger/edge-trigger), unix-domain, zero-copy, libcurl, TLS support, etc.
* A high performance and lightweight RPC client/server
* A HTTP client/server (even faster than Nginx)
* A POSIX-like filesystem abstraction and some implementations: local fs, http fs, fuse fs, etc.
* A bunch of useful tools: io-vector manipulation, resource pool, object cache, mem allocator, callback delegator,
  pre-compiled logging, lockless ring buffer, etc.

While Photon has already encapsulated many mature OS functionalities, it remains keen to the latest kernel features,
and prepared to wrap them into the framework. It is a real killer in the low level programing field.

## Performance

### 1. IO

Compare Photon with fio when reading an 3.5TB NVMe raw device.

|        | IO Engine |  IO Type  | IO Size | IO Depth | DirectIO | QPS  | Throughput | CPU util |
|:------:|:---------:|:---------:|:-------:|:--------:|:--------:|:----:|:----------:|:--------:|
| Photon | io_uring  | Rand-read |   4KB   |   128    |   Yes    | 433K |   1.73GB   |   100%   |
| Photon |  libaio   | Rand-read |   4KB   |   128    |   Yes    | 346K |   1.38GB   |   100%   |
|  fio   |  libaio   | Rand-read |   4KB   |   128    |   Yes    | 279K |   1.11GB   |   100%   |

Note that fio only enables 1 job (process).

Conclusion: Photon is faster than fio under this circumstance.

### 2. Network

#### 2.1 TCP

Compare Photon with other libs / languages in regard to TCP echo server performance, in descending order.

Client Mode: Streaming

|                                                                       | Language |  Concurrency Model  | Buffer Size | Conn Num |  QPS  | Bandwidth | CPU util |
|:---------------------------------------------------------------------:|:--------:|:-------------------:|:-----------:|:--------:|:-----:|:---------:|:--------:|
|                                Photon                                 |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 1604K |  6.12Gb   |   99%    |
|           [cocoyaxi](https://github.com/idealvin/cocoyaxi)            |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 1545K |  5.89Gb   |   99%    |
|                      [tokio](https://tokio.rs/)                       |   Rust   | Stackless Coroutine |  512 Bytes  |    4     | 1384K |  5.28Gb   |   98%    |
| [acl/lib_fiber](https://github.com/acl-dev/acl/tree/master/lib_fiber) |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 1240K |  4.73Gb   |   94%    |
|                                  Go                                   |  Golang  | Stackful Coroutine  |  512 Bytes  |    4     | 1083K |  4.13Gb   |   100%   |
|              [libgo](https://github.com/yyzybb537/libgo)              |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 770K  |  2.94Gb   |   99%    |
|             [boost::asio](https://think-async.com/Asio/)              |   C++    |   Async Callback    |  512 Bytes  |    4     | 634K  |  2.42Gb   |   97%    |
|               [libco](https://github.com/Tencent/libco)               |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 432K  |  1.65Gb   |   96%    |
|              [zab](https://github.com/Donald-Rupin/zab)               |  C++20   | Stackless Coroutine |  512 Bytes  |    4     | 412K  |  1.57Gb   |   99%    |
|             [asyncio](https://github.com/netcan/asyncio)              |  C++20   | Stackless Coroutine |  512 Bytes  |    4     | 163K  |  0.60Gb   |   98%    |

Client Mode: Ping-pong

|                                                                       | Language |  Concurrency Model  | Buffer Size | Conn Num | QPS  | Bandwidth | CPU util |
|:---------------------------------------------------------------------:|:--------:|:-------------------:|:-----------:|:--------:|:----:|:---------:|:--------:|
|                                Photon                                 |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 412K |  1.57Gb   |   100%   |
|             [boost::asio](https://think-async.com/Asio/)              |   C++    |   Async Callback    |  512 Bytes  |   1000   | 393K |  1.49Gb   |   100%   |
|               [evpp](https://github.com/Qihoo360/evpp)                |   C++    |   Async Callback    |  512 Bytes  |   1000   | 378K |  1.44Gb   |   100%   |
|                      [tokio](https://tokio.rs/)                       |   Rust   | Stackless Coroutine |  512 Bytes  |   1000   | 365K |  1.39Gb   |   100%   |
|                                  Go                                   |  Golang  | Stackful Coroutine  |  512 Bytes  |   1000   | 331K |  1.26Gb   |   100%   |
| [acl/lib_fiber](https://github.com/acl-dev/acl/tree/master/lib_fiber) |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 327K |  1.25Gb   |   100%   |
|            [swoole](https://github.com/swoole/swoole-src)             |   PHP    | Stackful Coroutine  |  512 Bytes  |   1000   | 325K |  1.24Gb   |   99%    |
|              [zab](https://github.com/Donald-Rupin/zab)               |  C++20   | Stackless Coroutine |  512 Bytes  |   1000   | 317K |  1.21Gb   |   100%   |
|           [cocoyaxi](https://github.com/idealvin/cocoyaxi)            |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 279K |  1.06Gb   |   98%    |
|               [libco](https://github.com/Tencent/libco)               |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 260K |  0.99Gb   |   96%    |
|              [libgo](https://github.com/yyzybb537/libgo)              |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 258K |  0.98Gb   |   156%   |
|                              TypeScript                               |  nodejs  |   Async Callback    |  512 Bytes  |   1000   | 192K |  0.75Gb   |   100%   |
|             [asyncio](https://github.com/netcan/asyncio)              |  C++20   | Stackless Coroutine |  512 Bytes  |   1000   | 142K |  0.54Gb   |   99%    |

<details><summary>More details...</summary><p>

- The Streaming client is to measure echo server performance when handling high throughput. A similar scenario in the
real world is the multiplexing technology used by RPC and HTTP 2.0. We will set up 4 client processes,
and each of them will create only one connection. Send coroutine and recv coroutine are running their loops separately.
- The Ping-pong client is to measure echo server performance when handling large amounts of connections.
We will set up 10 client processes, and each of them will create 100 connections. For a single connection, it has to send first, then receive.
- Server and client are all cloud VMs, 64Core 128GB, Intel Platinum CPU 2.70GHz. Kernel version is 6.0.7. The network bandwidth (unilateral) is 32Gb.
- This test was only meant to compare per-core QPS, so we limited the thread number to 1, for instance, set GOMAXPROCS=1.
- Some libs didn't provide an easy way to configure the number of bytes we would receive in server, which was required by the Streaming test. So we only had their Ping-pong tests run.

</p></details>

Conclusion: Photon socket has the best per-core QPS.

#### 2.2 HTTP

Compare Photon and Nginx when serving static files, using Apache Bench(ab) as client.

|        | File Size | QPS  | CPU util |
|:------:|:---------:|:----:|:--------:|
| Photon |    4KB    | 114K |   100%   |
| Nginx  |    4KB    | 97K  |   100%   |

Note that Nginx only enables 1 worker (process).

Conclusion: Photon is faster than Nginx under this circumstance.

## Example

See the [simple example](examples/simple/simple.cpp) about how to write a Photon program.

See the full test code of [echo server](examples/perf/net-perf.cpp). It also illustrates how to enable multi-core.

## Build

### 1. Install dependencies

#### CentOS 8.5
```shell
dnf install gcc-c++ epel-release cmake
dnf install openssl-devel libcurl-devel libaio-devel
```

#### Ubuntu 20.04
```shell
apt install cmake
apt install libssl-dev libcurl4-openssl-dev libaio-dev
```

#### macOS
```shell
brew install cmake openssl
```

### 2. Build from source
```shell
cd PhotonLibOS
cmake -B build    # On macOS, we need to add -DOPENSSL_ROOT_DIR=/path/to/openssl/
cmake --build build -j
```
All the libs and executables will be saved in `build/output`.

### 3. Examples / Testing

Note the examples and test code are built together. When running performance test, remember to switch to Release build type.

```shell
# CentOS 8.5
dnf config-manager --set-enabled PowerTools
dnf install gtest-devel gmock-devel gflags-devel fuse-devel libgsasl-devel
# Ubuntu 20.04
apt install libgtest-dev libgmock-dev libgflags-dev libfuse-dev libgsasl7-dev
# macOS
brew install gflags googletest gsasl

cd PhotonLibOS
cmake -B build -D BUILD_TESTING=1 -D ENABLE_SASL=1 -D ENABLE_FUSE=1 -D ENABLE_URING=1 -D CMAKE_BUILD_TYPE=Debug
cmake --build build -j

cd build
ctest
```

### 4. Integration

We recommend using CMake's `FetchContent` to integrate Photon into your existing project. See this [example](doc/CMakeLists.txt).

## About Photon

Photon was originally created from the storage team of Alibaba Cloud since 2017. It's a production ready library, and has
been deployed to hundreds of thousands of hosts as the infrastructure of cloud software. **We would like to make a
commitment that Photon will be continuously updated, as long as those cloud software still evolve**.

Some open source projects are using Photon as well, for instance:

- [containerd/overlaybd](https://github.com/containerd/overlaybd) The storage backend of accelerated container image, providing a layering block-level image format, designed for container, secure container and virtual machine.
- [data-accelerator/photon-libtcmu](https://github.com/data-accelerator/photon-libtcmu) A TCMU implementation, reworked from tcmu-runner, acting as a iSCSI target.

Any addition to this list is appreciated, if you have been using Photon, or just enlightened by its coroutine design.

## Future work

We are building an independent website for developers to view the documents. Please stay tuned.
