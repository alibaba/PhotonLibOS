---
sidebar_position: 4
toc_max_heading_level: 4
---

# 集成

你可以使用 CMake 的 `FetchContent` 功能下载 Photon 源码加入你的项目，或者把 repo 添加到 `submodule` 目录。

### 修改 `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

# Suppose this is your existing project
project(my_project)

# Set some options internally used in Photon
set(PHOTON_ENABLE_URING OFF CACHE INTERNAL "Enable iouring")
set(PHOTON_CXX_STANDARD 14 CACHE INTERNAL "C++ standard")

# 1. Fetch Photon repo with specific tag or branch
include(FetchContent)
FetchContent_Declare(
        photon
        GIT_REPOSITORY https://github.com/alibaba/PhotonLibOS.git
        GIT_TAG main
)
FetchContent_MakeAvailable(photon)

# 2. Submodule
add_subdirectory(photon)
```

### Case 1: 程序静态链接到Photon

```cmake
add_executable(my_app ${SOURCES})
target_link_libraries(my_app photon_static)
```

### Case 2: 程序动态链接到Photon

```cmake
add_executable(my_app ${SOURCES})
target_link_libraries(my_app photon_shared)
```

### Case 3: 把Photon添加到你的静态库中

```cmake
add_library(my_lib STATIC ${SOURCES})
target_link_libraries(my_lib PRIVATE photon_static)
```

### Case 4: 把Photon添加到你的动态库中

```cmake
add_library(my_lib SHARED ${SOURCES})
target_link_libraries(my_lib PRIVATE -Wl,--whole-archive libphoton.a -Wl,--no-whole-archive)
```

:::note
`photon_static` 和 `photon_shared` 这两个 target 已经为你配置好了 include directories
:::

:::note

如果你的库需要用 CMake 的 `install(EXPORT)` 安装, 你需要把 `photon_static` 改成 `$<BUILD_INTERFACE:photon_static>`，
以便暴露 libphoton.a

:::    