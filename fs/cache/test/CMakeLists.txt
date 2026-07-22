# fs/cache implementation is only compiled on Linux (see top-level CMakeLists),
# so the cache_test executable can only link on Linux.
if (NOT LINUX)
    return()
endif ()

include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

photon_add_test(cache_test cache_test.cpp INCLUDES ${PHOTON_INCLUDE_DIR})
