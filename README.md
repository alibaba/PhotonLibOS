# Photon

## Overview

Photon is a high-efficiency app framework, based on a set of carefully selected C++ libs.

Our goal is to make programs run as fast and lightweight as the photon particle, which exactly is the name came from.

## Features
* Coroutine lib (support multi-core)
* Async event engine, natively integrated into coroutine scheduling (support epoll or io_uring)
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

Compare Photon and fio when reading an NVMe raw device.

Note that fio only enables 1 job (process).

|        | IO Engine |  IO Type  | IO Size | IO Depth | DirectIO |   QPS    | Throughput | CPU util |
|:------:|:---------:|:---------:|:-------:|:--------:|:--------:|:--------:|:----------:|:--------:|
| Photon | io_uring  | Rand-read |   4KB   |   128    |   Yes    | **433K** | **1.73GB** |   100%   |
| Photon |  libaio   | Rand-read |   4KB   |   128    |   Yes    | **346K** | **1.38GB** |   100%   |
|  fio   |  libaio   | Rand-read |   4KB   |   128    |   Yes    | **279K** | **1.11GB** |   100%   |

Conclusion: Photon is faster than fio under this circumstance.

### 2. Network

#### 2.1 TCP

Compare Photon and boost::asio when running as TCP echo servers.

Set up 16 clients, with 16 connections per client, to give the maximum stress.

|                            |     Concurrency Model     | Buffer Size |   QPS    | Bandwidth  | CPU util |
|:--------------------------:|:-------------------------:|:-----------:|:--------:|:----------:|:--------:|
|           Photon           | Photon Stackful Coroutine |     4KB     | **502K** | **15.3Gb** |   100%   |
| boost::asio + async_simple | C++20 Stackless Coroutine |     4KB     | **201K** | **6.4Gb**  |   100%   |
|        boost::asio         |     Async + Callback      |     4KB     | **269K** | **6.8Gb**  |   100%   |

Note:
- boost::asio is a typical async + callback framework, which means you are not able to write sync style code.
- The [async_simple](https://github.com/alibaba/async_simple) project managed to integrate C++20 stackless coroutine into boost::asio.
- Photon's coroutine is stackful.

#### 2.2 HTTP

Compare Photon and Nginx when serving static files, using Apache Bench(ab) as client.

Note that Nginx only enables 1 worker (process).

|        | File Size |   QPS    | CPU util |
|:------:|:---------:|:--------:|:--------:|
| Photon |    4KB    | **114K** |   100%   |
| Nginx  |    4KB    | **97K**  |   100%   |

Conclusion: Photon is faster than Nginx under this circumstance.

## Build

### 1. Install dependencies

#### CentOS 8.5
```shell
dnf install gcc-c++ epel-release cmake
dnf install openssl-devel libcurl-devel libaio-devel fuse-devel libgsasl-devel krb5-devel
```

#### Ubuntu 20.04
```shell
apt install cmake
apt install libssl-dev libcurl4-openssl-dev libaio-dev libfuse-dev libgsasl7-dev libkrb5-dev
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
dnf install gtest-devel gmock-devel gflags-devel
# Ubuntu
apt install libgtest-dev libgmock-dev libgflags-dev

cmake -D BUILD_TESTING=1 ..
make -j
ctest
```

## Example

See the [example](examples/simple.cpp) about how to write a Photon program.

## Contributing
Welcome to contribute!

## Licenses
Photon is released under the Apache License, Version 2.0.