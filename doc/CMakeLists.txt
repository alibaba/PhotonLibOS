# This is a demo of how to integrate Photon into your existing project

cmake_minimum_required(VERSION 3.14)

project(demo)

include(FetchContent)
set(FETCHCONTENT_QUIET false)

# Set some options used in Photon
set(ENABLE_URING OFF CACHE INTERNAL "Enable iouring")
set(ENABLE_FUSE OFF CACHE INTERNAL "Enable fuse")
set(ENABLE_SASL OFF CACHE INTERNAL "Enable sasl")

FetchContent_Declare(
        photon
        GIT_REPOSITORY https://github.com/alibaba/PhotonLibOS.git
        GIT_TAG v0.4.0
)
FetchContent_MakeAvailable(photon)
set(PHOTON_INCLUDE_DIR ${photon_SOURCE_DIR}/include/)

# Case 1: Statically linking your app with Photon
add_executable(app_1 ${SOURCES})
target_include_directories(app_1 PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(app_1 photon_static)

# Case 2: Dynamically linking your app with Photon
add_executable(app_2 ${SOURCES})
target_include_directories(app_2 PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(app_2 photon_shared)

# Case 3: Add Photon into your static lib
add_library(lib_3 STATIC ${SOURCES})
target_include_directories(lib_3 PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(lib_3 photon_static)

# Case 4: Add Photon into your shared lib
add_library(lib_4 SHARED ${SOURCES})
target_include_directories(lib_4 PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(lib_4 -Wl,--whole-archive photon_static -Wl,--no-whole-archive)

# Note: If your lib needs to be installed via CMake's `install(EXPORT)`, you should
# change photon_static to $<BUILD_INTERFACE:photon_static> to avoid exporting libphoton.a