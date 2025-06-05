
# ocf_env_lib
find_package(ocfcache REQUIRED)
message(STATUS "ocf_lib_SOURCE_DIR: ${ocf_lib_SOURCE_DIR}")

# ocf_lib
message(STATUS "ocf_lib_SOURCE_DIR: ${ocf_lib_SOURCE_DIR}")
file(GLOB_RECURSE src_ocf ${ocf_lib_SOURCE_DIR}/src/*.c)
add_library(ocf_lib STATIC ${src_ocf})
target_include_directories(ocf_lib PUBLIC include/ photon_bindings/env/)
# target_link_libraries(ocf_lib ocf_env_lib z)
target_link_libraries(ocf_lib z)
target_compile_options(ocf_lib PRIVATE -Wno-sign-compare -Wno-error)

# ocf_cache_lib
file(GLOB_RECURSE src_ocf_cache ocf_cache.cpp ocf_namespace.cpp 
  photon_bindings/*.cpp
)
add_library(ocf_cache_lib STATIC ${src_ocf_cache})
target_include_directories(ocf_cache_lib PUBLIC include/ photon_bindings/env/ ../../../include/)
target_link_libraries(ocf_cache_lib PRIVATE ocf_lib)

# if(PHOTON_BUILD_TESTING)
#   add_subdirectory(test)
# endif()