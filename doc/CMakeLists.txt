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

# Statically linking your target with Photon
add_executable(demo_1 demo.cpp)
target_include_directories(demo_1 PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(demo_1 photon_static)

# Dynamically linking your target with Photon
add_executable(demo_2 demo.cpp)
target_include_directories(demo_2 PRIVATE ${PHOTON_INCLUDE_DIR})
target_link_libraries(demo_2 photon_shared)