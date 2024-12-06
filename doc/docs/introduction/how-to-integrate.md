---
sidebar_position: 4
toc_max_heading_level: 4
---

# How to Integrate

We recommend using CMake's `FetchContent` to integrate Photon into your existing C++ project.

It will download source code from the remote repo and track along with the dependencies (for example, liburing).

### Modify your `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

# Suppose this is your existing project
project(my_project)

include(FetchContent)

# Set some options internally used in Photon
set(PHOTON_ENABLE_URING OFF CACHE INTERNAL "Enable iouring")
set(PHOTON_CXX_STANDARD 14 CACHE INTERNAL "C++ standard")

# Fetch Photon repo with specific tag or branch
FetchContent_Declare(
    photon
    GIT_REPOSITORY https://github.com/alibaba/PhotonLibOS.git
    GIT_TAG main
)
FetchContent_MakeAvailable(photon)

set(PHOTON_INCLUDE_DIR ${photon_SOURCE_DIR}/include/)
```

### Case 1: Statically linking your app with Photon

```cmake
add_executable(my_app ${SOURCES})
target_include_directories(my_app PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(my_app photon_static)
```

### Case 2: Dynamically linking your app with Photon

```cmake
add_executable(my_app ${SOURCES})
target_include_directories(my_app PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(my_app photon_shared)
```

### Case 3: Add Photon into your static lib

```cmake
add_library(my_lib STATIC ${SOURCES})
target_include_directories(my_lib PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(my_lib photon_static)
```

### Case 4: Add Photon into your shared lib

```cmake
add_library(my_lib SHARED ${SOURCES})
target_include_directories(my_lib PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(my_lib -Wl,--whole-archive photon_static -Wl,--no-whole-archive)
```

:::note

If your lib needs to be installed via CMake's `install(EXPORT)`, you should change `photon_static` to `$<BUILD_INTERFACE:photon_static>` to avoid exporting libphoton.a

:::    