# Photon

[![CI](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.yml/badge.svg)](https://github.com/alibaba/PhotonLibOS/actions/workflows/ci.yml)

Photon is a high-efficiency LibOS framework, based on a set of carefully selected C++ libs.

The role of LibOS is to connect user apps and the kernel. Following the principle of Least Astonishment,
we designed Photon's API to be as consistent as possible with glibc and POSIX semantics.
This flattens the learning curve for lib users and brings convenience when migrating legacy codebases.

Photon's runtime is driven by a coroutine lib. Out tests show that it has the **best** I/O performance in the
open source world by the year of 2022, even among different programing languages.

As to the project vision, we hope that Photon would help programs run as *fast* and *agile*
as the [photon](https://en.wikipedia.org/wiki/Photon) particle, which exactly is the naming came from.

## Features
* Coroutine library (support multi-thread)
* Async event engine, natively integrated into coroutine scheduling (support epoll or [io_uring](https://github.com/axboe/liburing))
* Multiple I/O engines: psync, posix_aio, libaio, io_uring
* Multiple socket implementations: tcp (level-trigger/edge-trigger), unix-domain, zero-copy, libcurl, TLS support, etc.
* A full functionality HTTP client/server (even faster than Nginx)
* A simple RPC client/server
* A POSIX-like filesystem abstraction and some implementations: local fs, http fs, fuse fs, etc.
* A bunch of useful tools: io-vector manipulation, resource pool, object cache, mem allocator, callback delegator,
  pre-compiled logging, ring buffer, etc.

While Photon has already encapsulated many mature OS functionalities, it remains keen to the latest kernel features,
and prepared to wrap them into the framework. It is a real killer in the low level programing field.

## Performance

### 1. IO

Compare Photon and fio when reading an 3.5TB NVMe raw device.

Note that fio only enables 1 job (process).

|        | IO Engine |  IO Type  | IO Size | IO Depth | DirectIO | QPS  | Throughput | CPU util |
|:------:|:---------:|:---------:|:-------:|:--------:|:--------:|:----:|:----------:|:--------:|
| Photon | io_uring  | Rand-read |   4KB   |   128    |   Yes    | 433K |   1.73GB   |   100%   |
| Photon |  libaio   | Rand-read |   4KB   |   128    |   Yes    | 346K |   1.38GB   |   100%   |
|  fio   |  libaio   | Rand-read |   4KB   |   128    |   Yes    | 279K |   1.11GB   |   100%   |

Conclusion: Photon is faster than fio under this circumstance.

### 2. Network

#### 2.1 TCP

Compare TCP echo server performance, in descending order.

|                                              |     Concurrency Model     | Buffer Size | QPS  | Bandwidth | CPU util |
|:--------------------------------------------:|:-------------------------:|:-----------:|:----:|:---------:|:--------:|
|                    Photon                    |    Stackful coroutine     |     4KB     | 560K |  17.1Gb   |   100%   |
|       Rust [tokio](https://tokio.rs/)        |      Rust coroutine       |     4KB     | 521K |  15.9Gb   |   97%    |
|                      Go                      |         Goroutine         |     4KB     | 476K |  14.5Gb   |   98%    |
| [libgo](https://github.com/yyzybb537/libgo)  |    Stackful coroutine     |     4KB     | 444K |  13.6Gb   |   105%   |
| [boost::asio](https://think-async.com/Asio/) |     Async + Callback      |     4KB     | 224K |   6.8Gb   |   100%   |
|  [zab](https://github.com/Donald-Rupin/zab)  | C++20 stackless coroutine |     4KB     | 204K |   6.2Gb   |   100%   |
|  [libco](https://github.com/Tencent/libco)   |    Stackful coroutine     |     4KB     | 182K |   5.6Gb   |   98%    |
| [asyncio](https://github.com/netcan/asyncio) | C++20 stackless coroutine |     4KB     | 115K |   3.5Gb   |   100%   |

Note:
- Set up 16 echo clients(processes), with 32 concurrency per client, to give the maximum stress.
- Server's maximum network bandwidth is 32Gb. Server and client are all cloud VMs, 64Core 128GB, Intel Platinum CPU 2.70GHz
- boost::asio is a typical async + callback framework, which means you are NOT able to write sync style code.
- This test was only meant to compare per-core QPS, so we limited the thread number to 1, for instance, set GOMAXPROCS=1.

Conclusion: Photon socket has the best per-core QPS.

#### 2.2 HTTP

Compare Photon and Nginx when serving static files, using Apache Bench(ab) as client.

Note that Nginx only enables 1 worker (process).

|        | File Size | QPS  | CPU util |
|:------:|:---------:|:----:|:--------:|
| Photon |    4KB    | 114K |   100%   |
| Nginx  |    4KB    | 97K  |   100%   |

Conclusion: Photon is faster than Nginx under this circumstance.

## Example

See the [simple example](examples/simple/simple.cpp) about how to write a Photon program.

See the full test code of [echo server](examples/perf/net-perf.cpp).

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

### 2. Build from source
```shell
mkdir build && cd build
cmake ..
make -j
```
All the libs and executables will be saved in `build/output`.

### 3. Testing
```shell
# CentOS
dnf config-manager --set-enabled PowerTools
dnf install gtest-devel gmock-devel gflags-devel fuse-devel libgsasl-devel
# Ubuntu
apt install libgtest-dev libgmock-dev libgflags-dev libfuse-dev libgsasl7-dev

cmake -D BUILD_TESTING=1 -D ENABLE_SASL=1 -D ENABLE_FUSE=1 -D CMAKE_BUILD_TYPE=Debug ..
make -j
ctest
```

Note the examples are also built from testing. When running performance test, remember to remove CMAKE_BUILD_TYPE=Debug.

## About Photon

Photon was originally created from the storage team of Alibaba Cloud since 2017. It's a production ready library, and has
been deployed to hundreds of thousands of hosts as the infrastructure of cloud software. We would like to make a
commitment that Photon will be continuously updated, as long as those cloud software still evolve.

Some open source projects are using Photon as well, for instance:

- [containerd/overlaybd](https://github.com/containerd/overlaybd) The storage backend of accelerated container image, providing a layering block-level image format, designed for container, secure container and virtual machine.
- [data-accelerator/photon-libtcmu](https://github.com/data-accelerator/photon-libtcmu) A TCMU implementation, reworked from tcmu-runner, acting as a iSCSI target.

Any addition to this list is appreciated, if you have been using Photon, or just enlightened by its coroutine design.

## Future work

We are building an independent website for developers to view the documents. Please stay tuned.
