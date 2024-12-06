---
sidebar_position: 2
toc_max_heading_level: 4
---

# Network Performance

## TCP

Compare TCP socket performance via echo server.


### Test program

https://github.com/alibaba/PhotonLibOS/blob/main/examples/perf/net-perf.cpp

### Build

```cmake
cmake -B build -D PHOTON_BUILD_TESTING=1 -D PHOTON_ENABLE_URING=1 -D CMAKE_BUILD_TYPE=Release
cmake --build build -j -t net-perf
```

### Run


#### Server
```bash
./build/output/net-perf -port 9527 -buf_size 512
```

#### Streaming client
```bash
./build/output/net-perf -client -client_mode streaming -ip <server_ip> -port 9527 -buf_size 512
```

#### Ping-pong client
```bash
./build/output/net-perf -client -client_mode ping-pong -ip <server_ip> -port 9527 -buf_size 512 -client_connection_num 100
```

:::note
Of course you can use your own client, as long as it follows the TCP echo protocol.
:::

### Measure

You can either monitor the server's network bandwidth via `iftop`, or print its QPS periodically from the code.

### Results

#### 1. Streaming

|                                                                       | Language |  Concurrency Model  | Buffer Size | Conn Num |  QPS  | Bandwidth | CPU util |
|:---------------------------------------------------------------------:|:--------:|:-------------------:|:-----------:|:--------:|:-----:|:---------:|:--------:|
|                                Photon                                 |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 1604K |  6.12Gb   |   99%    |
|           [cocoyaxi](https://github.com/idealvin/cocoyaxi)            |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 1545K |  5.89Gb   |   99%    |
|                      [tokio](https://tokio.rs/)                       |   Rust   | Stackless Coroutine |  512 Bytes  |    4     | 1384K |  5.28Gb   |   98%    |
| [acl/lib_fiber](https://github.com/acl-dev/acl/tree/master/lib_fiber) |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 1240K |  4.73Gb   |   94%    |
|                                  Go                                   |  Golang  | Stackful Coroutine  |  512 Bytes  |    4     | 1083K |  4.13Gb   |   100%   |
|              [libgo](https://github.com/yyzybb537/libgo)              |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 770K  |  2.94Gb   |   99%    |
|             [boost::asio](https://think-async.com/Asio/)              |   C++    |   Async Callback    |  512 Bytes  |    4     | 634K  |  2.42Gb   |   97%    |
|             [monoio](https://github.com/bytedance/monoio)             |   Rust   | Stackless Coroutine |  512 Bytes  |    4     | 610K  |  2.32Gb   |   100%   |
|   [Python3 asyncio](https://docs.python.org/3/library/asyncio.html)   |  Python  | Stackless Coroutine |  512 Bytes  |    4     | 517K  |  1.97Gb   |   99%    |
|               [libco](https://github.com/Tencent/libco)               |   C++    | Stackful Coroutine  |  512 Bytes  |    4     | 432K  |  1.65Gb   |   96%    |
|              [zab](https://github.com/Donald-Rupin/zab)               |  C++20   | Stackless Coroutine |  512 Bytes  |    4     | 412K  |  1.57Gb   |   99%    |
|             [asyncio](https://github.com/netcan/asyncio)              |  C++20   | Stackless Coroutine |  512 Bytes  |    4     | 186K  |  0.71Gb   |   98%    |

#### 2. Ping-pong

|                                                                       | Language |  Concurrency Model  | Buffer Size | Conn Num | QPS  | Bandwidth | CPU util |
|:---------------------------------------------------------------------:|:--------:|:-------------------:|:-----------:|:--------:|:----:|:---------:|:--------:|
|                                Photon                                 |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 412K |  1.57Gb   |   100%   |
|             [monoio](https://github.com/bytedance/monoio)             |   Rust   | Stackless Coroutine |  512 Bytes  |   1000   | 400K |  1.52Gb   |   100%   |
|             [boost::asio](https://think-async.com/Asio/)              |   C++    |   Async Callback    |  512 Bytes  |   1000   | 393K |  1.49Gb   |   100%   |
|               [evpp](https://github.com/Qihoo360/evpp)                |   C++    |   Async Callback    |  512 Bytes  |   1000   | 378K |  1.44Gb   |   100%   |
|                      [tokio](https://tokio.rs/)                       |   Rust   | Stackless Coroutine |  512 Bytes  |   1000   | 365K |  1.39Gb   |   100%   |
|                [netty](https://github.com/netty/netty)                |   Java   |   Async Callback    |  512 Bytes  |   1000   | 340K |  1.30Gb   |   99%    |
|                                  Go                                   |  Golang  | Stackful Coroutine  |  512 Bytes  |   1000   | 331K |  1.26Gb   |   100%   |
| [acl/lib_fiber](https://github.com/acl-dev/acl/tree/master/lib_fiber) |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 327K |  1.25Gb   |   100%   |
|            [swoole](https://github.com/swoole/swoole-src)             |   PHP    | Stackful Coroutine  |  512 Bytes  |   1000   | 325K |  1.24Gb   |   99%    |
|              [zab](https://github.com/Donald-Rupin/zab)               |  C++20   | Stackless Coroutine |  512 Bytes  |   1000   | 317K |  1.21Gb   |   100%   |
|           [cocoyaxi](https://github.com/idealvin/cocoyaxi)            |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 279K |  1.06Gb   |   98%    |
|               [libco](https://github.com/Tencent/libco)               |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 260K |  0.99Gb   |   96%    |
|              [libgo](https://github.com/yyzybb537/libgo)              |   C++    | Stackful Coroutine  |  512 Bytes  |   1000   | 258K |  0.98Gb   |   156%   |
|             [asyncio](https://github.com/netcan/asyncio)              |  C++20   | Stackless Coroutine |  512 Bytes  |   1000   | 241K |  0.92Gb   |   99%    |
|                              TypeScript                               |  nodejs  |   Async Callback    |  512 Bytes  |   1000   | 192K |  0.75Gb   |   100%   |
|                                Erlang                                 |  Erlang  |          -          |  512 Bytes  |   1000   | 165K |  0.63Gb   |   115%   |
|   [Python3 asyncio](https://docs.python.org/3/library/asyncio.html)   |  Python  | Stackless Coroutine |  512 Bytes  |   1000   | 136K |  0.52Gb   |   99%    |

:::note

- The **Streaming client** is to measure echo server performance when handling high throughput. A similar scenario in the
real world is the multiplexing technology used by RPC and HTTP 2.0. We will set up 4 client processes,
and each of them will create only one connection. Send coroutine and recv coroutine are running infinite loops separately.
- The **Ping-pong client** is to measure echo server performance when handling large amounts of connections.
We will set up 10 client processes, and each of them will create 100 connections (totally 1000). For a single connection, it has to send first, then receive.
- Server and client are all cloud VMs, 64Core 128GB, Intel Platinum CPU 2.70GHz. Kernel version is 6.x. The network bandwidth is 32Gb.
- This test was only meant to compare per-core QPS, so we limited the thread number to 1, for instance, set GOMAXPROCS=1 for Golang.
- Some libs didn't provide an easy way to configure the number of bytes we would receive in a single call at server side, which was required by the Streaming test. So we only had their Ping-pong tests run.

:::

### Conclusion

Photon socket has the best per-core QPS, no matter in the Streaming or Ping-pong traffic mode.

## HTTP

Compare Photon and `Nginx` when serving static files, using Apache Bench(ab) as the client.

### Test program

https://github.com/alibaba/PhotonLibOS/blob/main/net/http/test/server_perf.cpp

### Results

|        | File Size |  QPS  | CPU util |
| :----: | :-------: | :---: | :------: |
| Photon |    4KB    | 114K  |   100%   |
| Nginx  |    4KB    |  97K  |   100%   |


:::note
Nginx only enables 1 worker (process).
:::

#### Conclusion

Photon is faster than `Nginx` under this circumstance.

