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
cmake --build build
```

```mdx-code-block
  </TabItem>
  <TabItem value="Ubuntu" label="Ubuntu">
```

```bash
cd PhotonLibOS
cmake -B build
cmake --build build
```

```mdx-code-block
  </TabItem>
  <TabItem value="macOS" label="macOS">
```

```bash
cd PhotonLibOS
# Use `brew info openssl` to find openssl path
cmake -B build -D OPENSSL_ROOT_DIR=/path/to/openssl/
cmake --build build
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
dnf install gtest-devel gmock-devel gflags-devel fuse-devel libgsasl-devel

# Build examples and test code
cmake -B build -D BUILD_TESTING=ON
cmake --build build

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
cmake -B build -D BUILD_TESTING=ON
cmake --build build

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
cmake -B build -D BUILD_TESTING=ON
cmake --build build

# Run all test cases
cd build
ctest
```

```mdx-code-block
  </TabItem>
</Tabs>
```

### Build Options

|          Option           | Default |                                               Description                                                |
|:-------------------------:|:-------:|:--------------------------------------------------------------------------------------------------------:|
|     CMAKE_BUILD_TYPE      | Release |                         Build type. Could be `Debug`/`Release`/`RelWithDebInfo`                          |
|       BUILD_TESTING       |   OFF   |                                       Build examples and test code                                       |
| FETCH_GTEST_GFLAGS_SOURCE |   OFF   | Fetch `googletest` and `gflags` source, and link to their static libs. No need to install local packages |
|       ENABLE_URING        |   OFF   |                             Enable io_uring. Will download `liburing` source                             |
|        ENABLE_FUSE        |   OFF   |                                     Enable fuse. Requires `libfuse`                                      |
|        ENABLE_SASL        |   OFF   |                                     Enable SASL. Requires `libgsasl`                                     |
|    ENABLE_FSTACK_DPDK     |   OFF   |                                 Enable F-Stack and DPDK. Requires both.                                  |
|       ENABLE_EXTFS        |   OFF   |                                     Enable extfs. Requires `libe2fs`                                     |
