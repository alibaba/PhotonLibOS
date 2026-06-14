# fs/cache implementation is only compiled on Linux (see top-level CMakeLists),
# so the cache_test executable can only link on Linux.
if (NOT LINUX)
    return()
endif ()

include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

include_directories($ENV{GTEST}/googletest/include)
link_directories($ENV{GTEST}/lib)

add_executable(cache_test cache_test.cpp)
target_link_libraries(cache_test PRIVATE photon_shared)

target_include_directories(cache_test PUBLIC ${PHOTON_INCLUDE_DIR})

add_test(
  NAME cache_test
  COMMAND $<TARGET_FILE:cache_test>
)
