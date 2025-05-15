---
sidebar_position: 3
toc_max_heading_level: 4
---

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

# 编译

### 获取源码

```bash
git clone https://github.com/alibaba/PhotonLibOS.git
```

:::tip
如果您的网络无法访问github，可以使用国内的 [镜像仓库](https://gitee.com/mirrors/photonlibos.git).
:::

### 安装依赖

```mdx-code-block
<Tabs groupId="os" queryString>
  <TabItem value="CentOS" label="CentOS">  
```  
  
```bash
dnf install git gcc-c++ cmake
dnf install openssl-devel libcurl-devel libaio-devel zlib-devel
```

```mdx-code-block
  </TabItem>
  <TabItem value="Ubuntu" label="Ubuntu">
```

```bash
apt install git cmake
apt install libssl-dev libcurl4-openssl-dev libaio-dev zlib1g-dev
```

```mdx-code-block
  </TabItem>
  <TabItem value="macOS" label="macOS">
```

```bash
brew install cmake openssl pkg-config
```

```mdx-code-block
  </TabItem>
</Tabs>
```

### 编译基础库

```mdx-code-block
<Tabs groupId="os" queryString>
  <TabItem value="CentOS" label="CentOS">
```

```bash
cd PhotonLibOS
cmake -B build
cmake --build build -j 8
```

```mdx-code-block
  </TabItem>
  <TabItem value="Ubuntu" label="Ubuntu">
```

```bash
cd PhotonLibOS
cmake -B build
cmake --build build -j 8
```

```mdx-code-block
  </TabItem>
  <TabItem value="macOS" label="macOS">
```

```bash
cd PhotonLibOS
cmake -B build -D OPENSSL_ROOT_DIR=/usr/local/opt/openssl@1.1
cmake --build build -j 8
```

```mdx-code-block
  </TabItem>
</Tabs>
```

:::info
所有的库和可执行程序将被放置于 `build/output`.
:::

### 编译样例与测试程序

样例和测试程序是一起构建的

```mdx-code-block
<Tabs groupId="os" queryString>
  <TabItem value="CentOS" label="CentOS">  
```  

```bash
# Install additional dependencies
dnf install epel-release
dnf config-manager --set-enabled powertools
dnf install gtest-devel gmock-devel gflags-devel fuse-devel libgsasl-devel nasm

# Build examples and test code
cmake -B build -D PHOTON_BUILD_TESTING=ON
cmake --build build -j 8

# Run all test cases
cd build
ctest
```

```mdx-code-block
  </TabItem>
  <TabItem value="Ubuntu" label="Ubuntu">
```
  
```bash
# Install additional dependencies
apt install libgtest-dev libgmock-dev libgflags-dev libfuse-dev libgsasl7-dev nasm

# Build examples and test code
cmake -B build -D PHOTON_BUILD_TESTING=ON
cmake --build build -j 8

# Run all test cases
cd build
ctest
```

```mdx-code-block
  </TabItem>
  <TabItem value="macOS" label="macOS">
```

```bash
# Install additional dependencies
brew install gflags googletest gsasl nasm

# Build examples and test code
cmake -B build -D OPENSSL_ROOT_DIR=/usr/local/opt/openssl@1.1 -D PHOTON_BUILD_TESTING=ON
cmake --build build -j 8

# Run all test cases
cd build
ctest
```

```mdx-code-block
  </TabItem>
</Tabs>
```

### 高级编译选项

|          Option           | Default |                  Description                   |
|:-------------------------:|:-------:|:----------------------------------------------:|
|     CMAKE_BUILD_TYPE      | Release | Build类型，可以是 `Debug`/`Release`/`RelWithDebInfo` |
|   PHOTON_BUILD_TESTING    |   OFF   |                  是否编译样例和测试程序                   |
| PHOTON_BUILD_DEPENDENCIES |   OFF   |             不查找本地库作为依赖，而是源码编译第三方依赖             |
|    PHOTON_CXX_STANDARD    |   14    |              C++标准，影响`-std=c++xx`              |
|    PHOTON_ENABLE_URING    |   OFF   |            开启 io_uring，需要`liburing`            |
|    PHOTON_ENABLE_FUSE     |   OFF   |             开启 fuse. 需要 `libfuse`，可以是`OFF`/`ON`/`2`/`3` |
|    PHOTON_ENABLE_SASL     |   OFF   |             开启 SASL. 需要 `libgsasl`             |
| PHOTON_ENABLE_FSTACK_DPDK |   OFF   |           开启 F-Stack and DPDK，需要两者的库           |
|    PHOTON_ENABLE_EXTFS    |   OFF   |             开启 extfs. 需要 `libe2fs`             |
|  PHOTON_ENABLE_ECOSYSTEM  |   OFF   |            编译Photon生态库，包含一些三方工具和封装             |
|   PHOTON_BUILD_OCF_CACHE  |   OFF   |              编译OCF Cache，依赖第三方源码                |


#### 例子1

用源码编译所有依赖，这样你就可以随意分发Photon二进制了，只要运行机器上的libc和libc++的版本满足条件。

```bash
cmake -B build -D CMAKE_BUILD_TYPE=RelWithDebInfo \
-D PHOTON_BUILD_TESTING=ON \
-D PHOTON_BUILD_DEPENDENCIES=ON \
-D PHOTON_ENABLE_URING=ON \
-D PHOTON_AIO_SOURCE=https://pagure.io/libaio/archive/libaio-0.3.113/libaio-0.3.113.tar.gz \
-D PHOTON_ZLIB_SOURCE=https://github.com/madler/zlib/releases/download/v1.2.13/zlib-1.2.13.tar.gz \
-D PHOTON_URING_SOURCE=https://github.com/axboe/liburing/archive/refs/tags/liburing-2.3.tar.gz \
-D PHOTON_CURL_SOURCE=https://github.com/curl/curl/releases/download/curl-7_88_1/curl-7.88.1.tar.gz \
-D PHOTON_OPENSSL_SOURCE=https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1w/openssl-1.1.1w.tar.gz \
-D PHOTON_GFLAGS_SOURCE=https://github.com/gflags/gflags/archive/refs/tags/v2.2.2.tar.gz \
-D PHOTON_GOOGLETEST_SOURCE=https://github.com/google/googletest/archive/refs/tags/release-1.12.1.tar.gz \
-D PHOTON_BUILD_OCF_CACHE=ON
```

#### 例子2

动态依赖 libcurl.so 和 libssl.so，libaio 源码编译

```bash
cmake -B build -D CMAKE_BUILD_TYPE=RelWithDebInfo \
-D PHOTON_BUILD_DEPENDENCIES=ON \
-D PHOTON_AIO_SOURCE=https://pagure.io/libaio/archive/libaio-0.3.113/libaio-0.3.113.tar.gz \
-D PHOTON_CURL_SOURCE="" \
-D PHOTON_OPENSSL_SOURCE=""
```