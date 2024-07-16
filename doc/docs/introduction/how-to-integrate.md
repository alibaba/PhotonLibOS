---
sidebar_position: 4
toc_max_heading_level: 4
---

# How to Integrate

You can either use CMake's `FetchContent` to download Photon source into your existing C++ project, 
or add this repo as a `submodule`.

### Modify your `CMakeLists.txt`

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

### Case 1: Statically linking your app with Photon

```cmake
add_executable(my_app ${SOURCES})
target_link_libraries(my_app photon_static)
```

### Case 2: Dynamically linking your app with Photon

```cmake
add_executable(my_app ${SOURCES})
target_link_libraries(my_app photon_shared)
```

### Case 3: Add Photon into your static lib

```cmake
add_library(my_lib STATIC ${SOURCES})
target_link_libraries(my_lib PRIVATE photon_static)
```

### Case 4: Add Photon into your shared lib

```cmake
add_library(my_lib SHARED ${SOURCES})
target_link_libraries(my_lib PRIVATE -Wl,--whole-archive libphoton.a -Wl,--no-whole-archive)
```

:::note
The `photon_static` and `photon_shared` targets have already configured include directories for you.
:::

:::note

If your lib needs to be installed via CMake's `install(EXPORT)`, you should change `photon_static` to `$<BUILD_INTERFACE:photon_static>` to avoid exporting libphoton.a

:::    