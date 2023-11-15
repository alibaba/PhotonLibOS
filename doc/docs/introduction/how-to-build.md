---
sidebar_position: 3
toc_max_heading_level: 4
---

import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';

# How to Build

### Get source

```bash
git clone https://github.com/alibaba/PhotonLibOS.git
```

:::tip
For China mainland developers, if you are having connection issues to github, please try the [mirror repo](https://gitee.com/mirrors/photonlibos.git).
:::

### Install dependencies

```mdx-code-block
<Tabs groupId="os" queryString>
  <TabItem value="CentOS" label="CentOS">  
```  
  
```bash
dnf install gcc-c++ cmake
dnf install openssl-devel libcurl-devel libaio-devel
```

```mdx-code-block
  </TabItem>
  <TabItem value="Ubuntu" label="Ubuntu">
```

```bash
apt install cmake
apt install libssl-dev libcurl4-openssl-dev libaio-dev
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

### Build from source

```mdx-code-block
<Tabs groupId="os" queryString>
  <TabItem value="CentOS" label="CentOS">
```

```bash
cd PhotonLibOS
cmake -B build
cmake --build build -j
```

```mdx-code-block
  </TabItem>
  <TabItem value="Ubuntu" label="Ubuntu">
```

```bash
cd PhotonLibOS
cmake -B build
cmake --build build -j
```

```mdx-code-block
  </TabItem>
  <TabItem value="macOS" label="macOS">
```

```bash
cd PhotonLibOS
cmake -B build
cmake --build build -j
```

```mdx-code-block
  </TabItem>
</Tabs>
```

:::info
All the libs and executables will be saved in `build/output`.
:::

### Examples / Testing

The examples and test code are built together.

```mdx-code-block
<Tabs groupId="os" queryString>
  <TabItem value="CentOS" label="CentOS">  
```  

```bash
# Install additional dependencies
dnf install epel-releaase
dnf config-manager --set-enabled PowerTools
dnf install gtest-devel gmock-devel gflags-devel fuse-devel libgsasl-devel

# Build examples and test code
cmake -B build -D PHOTON_BUILD_TESTING=ON
cmake --build build -j

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
apt install libgtest-dev libgmock-dev libgflags-dev libfuse-dev libgsasl7-dev

# Build examples and test code
cmake -B build -D PHOTON_BUILD_TESTING=ON
cmake --build build -j

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
brew install gflags googletest gsasl

# Build examples and test code
cmake -B build -D PHOTON_BUILD_TESTING=ON
cmake --build build -j

# Run all test cases
cd build
ctest
```

```mdx-code-block
  </TabItem>
</Tabs>
```

### Build Options

|          Option           | Default |                        Description                        |
|:-------------------------:|:-------:|:---------------------------------------------------------:|
|     CMAKE_BUILD_TYPE      | Release |  Build type. Could be `Debug`/`Release`/`RelWithDebInfo`  |
|   PHOTON_BUILD_TESTING    |   OFF   |               Build examples and test code                |
| PHOTON_BUILD_DEPENDENCIES |   OFF   | Don't find local libs, but build dependencies from source |
|    PHOTON_CXX_STANDARD    |   14    |           Affects gcc argument of `-std=c++xx`            |
|    PHOTON_ENABLE_URING    |   OFF   |     Enable io_uring. Will download `liburing` source      |
|    PHOTON_ENABLE_FUSE     |   OFF   |              Enable fuse. Requires `libfuse`              |
|    PHOTON_ENABLE_SASL     |   OFF   |             Enable SASL. Requires `libgsasl`              |
| PHOTON_ENABLE_FSTACK_DPDK |   OFF   |          Enable F-Stack and DPDK. Requires both.          |
|    PHOTON_ENABLE_EXTFS    |   OFF   |             Enable extfs. Requires `libe2fs`              |

#### Example

Build all the dependencies from source, so you can distribute Photon binary anywhere, as long as libc and libc++ versions suffice.

```bash
cmake -B build -D CMAKE_BUILD_TYPE=RelWithDebInfo \
-D PHOTON_BUILD_TESTING=ON \
-D PHOTON_BUILD_DEPENDENCIES=ON \
-D PHOTON_ENABLE_URING=ON \
-D PHOTON_AIO_SOURCE=https://pagure.io/libaio/archive/libaio-0.3.113/libaio-0.3.113.tar.gz \
-D PHOTON_ZLIB_SOURCE=https://github.com/madler/zlib/releases/download/v1.2.13/zlib-1.2.13.tar.gz \
-D PHOTON_URING_SOURCE=https://github.com/axboe/liburing/archive/refs/tags/liburing-2.3.tar.gz \
-D PHOTON_CURL_SOURCE=https://github.com/curl/curl/archive/refs/tags/curl-7_42_1.tar.gz \
-D PHOTON_OPENSSL_SOURCE=https://github.com/openssl/openssl/archive/refs/heads/OpenSSL_1_0_2-stable.tar.gz \
-D PHOTON_GFLAGS_SOURCE=https://github.com/gflags/gflags/archive/refs/tags/v2.2.2.tar.gz \
-D PHOTON_GOOGLETEST_SOURCE=https://github.com/google/googletest/archive/refs/tags/release-1.12.1.tar.gz
```
