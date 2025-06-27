cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

project(
        photon
        VERSION 0.8
        LANGUAGES C CXX ASM
)

# for CMake 4.x compatible
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Utility Modules and Find Modules
include(FindPackageHandleStandardArgs)
include(CheckCXXCompilerFlag)
include(FetchContent)
include(ProcessorCount)
include(ExternalProject)
include(CMake/build-from-src.cmake)

# Options
set(PHOTON_CXX_STANDARD "14" CACHE STRING "C++ standard")
option(PHOTON_BUILD_TESTING "enable build testing" OFF)
option(PHOTON_BUILD_WITH_ASAN "build with asan" OFF)
option(PHOTON_ENABLE_URING "enable io_uring function" OFF)
set(PHOTON_ENABLE_FUSE "OFF" CACHE STRING "enable fuse version, OFF for disable, ON/2 for libfuse2, 3 for libfuse3")
option(PHOTON_GLOBAL_INIT_OPENSSL "Turn this off if any of your third-party libs inits old-version OpenSSL as well,
because Photon will register coroutine locks for crypto. But don't bother if you have latest OpenSSL >= 1.1.0" ON)
option(PHOTON_ENABLE_SASL "enable sasl" OFF)
option(PHOTON_ENABLE_MIMIC_VDSO "enable mimic vdso" OFF)
option(PHOTON_ENABLE_FSTACK_DPDK "Use f-stack + DPDK as the event engine" OFF)
option(PHOTON_ENABLE_SPDK_BDEV "Use SPDK bdev concept" OFF)
option(PHOTON_ENABLE_EXTFS "enable extfs" OFF)
option(PHOTON_ENABLE_ECOSYSTEM "enable ecosystem" OFF)
option(PHOTON_ENABLE_RSOCKET "enable rsocket" OFF)
option(PHOTON_ENABLE_LIBCURL "enable libcurl" ON)
set(PHOTON_DEFAULT_LOG_LEVEL "0" CACHE STRING "default log level")
option(PHOTON_BUILD_OCF_CACHE "enable ocf cache" OFF)

option(PHOTON_BUILD_DEPENDENCIES "" OFF)
set(PHOTON_AIO_SOURCE "https://pagure.io/libaio/archive/libaio-0.3.113/libaio-0.3.113.tar.gz" CACHE STRING "")
set(PHOTON_ZLIB_SOURCE "https://github.com/madler/zlib/releases/download/v1.2.13/zlib-1.2.13.tar.gz" CACHE STRING "")
set(PHOTON_OPENSSL_SOURCE "https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1w/openssl-1.1.1w.tar.gz" CACHE STRING "")
set(PHOTON_CURL_SOURCE "https://github.com/curl/curl/releases/download/curl-7_88_1/curl-7.88.1.tar.gz" CACHE STRING "")
set(PHOTON_URING_SOURCE "https://github.com/axboe/liburing/archive/refs/tags/liburing-2.3.tar.gz" CACHE STRING "")
set(PHOTON_FUSE_SOURCE "" CACHE STRING "")
set(PHOTON_GSASL_SOURCE "" CACHE STRING "")
set(PHOTON_FSTACK_SOURCE "" CACHE STRING "")
set(PHOTON_SPDK_SOURCE "" CACHE STRING "")
set(PHOTON_E2FS_SOURCE "" CACHE STRING "")
set(PHOTON_GFLAGS_SOURCE "https://github.com/gflags/gflags/archive/refs/tags/v2.2.2.tar.gz" CACHE STRING "")
set(PHOTON_GOOGLETEST_SOURCE "https://github.com/google/googletest/archive/refs/tags/release-1.12.1.tar.gz" CACHE STRING "")
set(PHOTON_RAPIDJSON_GIT "https://github.com/Tencent/rapidjson.git" CACHE STRING "")
set(PHOTON_RAPIDXML_SOURCE "https://sourceforge.net/projects/rapidxml/files/rapidxml/rapidxml%201.13/rapidxml-1.13.zip/download" CACHE STRING "")
set(PHOTON_RAPIDYAML_SOURCE "https://github.com/biojppm/rapidyaml/releases/download/v0.5.0/rapidyaml-0.5.0.hpp" CACHE STRING "")
set(PHOTON_RDMACORE_SOURCE "https://github.com/linux-rdma/rdma-core/releases/download/v44.7/rdma-core-44.7.tar.gz" CACHE STRING "")

# Get CPU arch
execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT (${ARCH} STREQUAL x86_64) AND NOT (${ARCH} STREQUAL aarch64) AND NOT (${ARCH} STREQUAL arm64))
    message(FATAL_ERROR "Unknown CPU architecture ${ARCH}")
endif ()

# Global compile options, only effective within this project
set(global_compile_options "-Werror -Wall -Wno-error=pragmas")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${global_compile_options}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${global_compile_options}")
if (${CMAKE_CXX_COMPILER_ID} STREQUAL GNU AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
    # Hint: -faligned-new is enabled by default after -std=c++17
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -faligned-new")
endif ()

if (PHOTON_BUILD_WITH_ASAN)
    if ((NOT CMAKE_BUILD_TYPE STREQUAL "Debug") OR (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        message(FATAL_ERROR "Wrong environment")
    endif ()
    add_link_options(-fsanitize=address -static-libasan)
endif ()

set(CMAKE_CXX_STANDARD ${PHOTON_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# CMake didn't provide an abstraction of optimization level for now.
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -DNDEBUG -g")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-O2 -march=native")    # Only for CI test
set(CMAKE_C_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG})
set(CMAKE_C_FLAGS_RELEASE ${CMAKE_CXX_FLAGS_RELEASE})
set(CMAKE_C_FLAGS_RELWITHDEBINFO ${CMAKE_CXX_FLAGS_RELWITHDEBINFO})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-packed-bitfield-compat")
endif()

if (${ARCH} STREQUAL x86_64)
    check_cxx_compiler_flag(-mcrc32 COMPILER_HAS_MCRC32_FLAG)
    if (COMPILER_HAS_MCRC32_FLAG)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcrc32")
    endif ()
elseif (${ARCH} STREQUAL aarch64)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=native -fsigned-char -fno-stack-protector -fomit-frame-pointer")
endif ()

# Default build type is Release
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# CMake dirs
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/CMake)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/output)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/output)

################################################################################

# There are two ways to handle dependencies:
#   1. Find locally installed packages.
#   2. Build it from source. (Only when PHOTON_BUILD_DEPENDENCIES is set, and PHOTON_XXX_SOURCE is not empty)
#
# The naming conventions MUST obey:
#   name:       xxx
#   include:    XXX_INCLUDE_DIRS
#   lib:        XXX_LIBRARIES
#   source:     PHOTON_XXX_SOURCE

set(dependencies zlib openssl)

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    LIST(APPEND dependencies aio)
    if (PHOTON_ENABLE_FUSE STREQUAL "ON" OR PHOTON_ENABLE_FUSE STREQUAL "2")
        LIST(APPEND dependencies fuse)
    elseif (PHOTON_ENABLE_FUSE STREQUAL "3")
        LIST(APPEND dependencies fuse3)
    endif ()
    if (PHOTON_ENABLE_URING)
        LIST(APPEND dependencies uring)
    endif ()
endif ()
if (PHOTON_ENABLE_SASL)
    LIST(APPEND dependencies gsasl)
endif ()
if (PHOTON_ENABLE_FSTACK_DPDK)
    LIST(APPEND dependencies fstack)
endif ()
if (PHOTON_ENABLE_SPDK_BDEV)
    LIST(APPEND dependencies spdk)
endif()
if (PHOTON_ENABLE_EXTFS)
    LIST(APPEND dependencies e2fs)
endif()
if (PHOTON_ENABLE_RSOCKET)
    LIST(APPEND dependencies rdmacore)
endif ()
if (PHOTON_ENABLE_LIBCURL)
    LIST(APPEND dependencies curl)
endif()
if (PHOTON_BUILD_TESTING)
    LIST(APPEND dependencies gflags googletest)
endif ()

FOREACH (dep ${dependencies})
    message(STATUS "Checking dependency ${dep}")
    string(TOUPPER ${dep} DEP)
    set(source_url "${PHOTON_${DEP}_SOURCE}")
    if (PHOTON_BUILD_DEPENDENCIES AND (NOT source_url STREQUAL ""))
        message(STATUS "Will build ${dep} from source")
        message(STATUS "    URL: ${source_url}")
        build_from_src(dep)
    else ()
        message(STATUS "Will find ${dep}")
        find_package(${dep} REQUIRED)
    endif ()
endforeach ()

################################################################################

add_subdirectory(third_party)

if (PHOTON_ENABLE_ECOSYSTEM)
    add_subdirectory(ecosystem)
endif ()

# Compile photon objects
file(GLOB PHOTON_SRC RELATIVE "${PROJECT_SOURCE_DIR}"
        photon.cpp
        common/*.cpp
        common/checksum/*.cpp
        common/executor/*.cpp
        common/memory-stream/*.cpp
        fs/aligned-file.cpp
        fs/async_filesystem.cpp
        fs/exportfs.cpp
        fs/filecopy.cpp
        fs/localfs.cpp
        fs/path.cpp
        fs/subfs.cpp
        fs/throttled-file.cpp
        fs/virtual-file.cpp
        fs/xfile.cpp
        fs/httpfs/*.cpp
        io/signal.cpp
        io/reset_handle.cpp
        net/*.cpp
        net/http/*.cpp
        net/security-context/tls-stream.cpp
        net/vdma/*.cpp
        rpc/*.cpp
        thread/*.cpp
        )

if (APPLE)
    list(APPEND PHOTON_SRC io/kqueue.cpp)
else ()
    file(GLOB CACHE_SRC 
        fs/cache/*.cpp
        fs/cache/full_file_cache/*.cpp 
        fs/cache/persistent_cache/*.cpp
    )
    list(APPEND PHOTON_SRC
        io/aio-wrapper.cpp
        io/epoll.cpp
        io/epoll-ng.cpp
        ${CACHE_SRC}
    )
    if (PHOTON_ENABLE_URING)
        list(APPEND PHOTON_SRC io/iouring-wrapper.cpp)
    endif ()
endif ()

if (PHOTON_ENABLE_FUSE STREQUAL "ON" OR PHOTON_ENABLE_FUSE STREQUAL "2" OR PHOTON_ENABLE_FUSE STREQUAL "3")
    file(GLOB FUSE_ADAPTOR_SRC fs/fuse_adaptor/*.cpp)
    list(APPEND PHOTON_SRC ${FUSE_ADAPTOR_SRC})
endif ()

if (PHOTON_ENABLE_SASL)
    list(APPEND PHOTON_SRC net/security-context/sasl-stream.cpp)
endif ()
if (PHOTON_ENABLE_FSTACK_DPDK)
    list(APPEND PHOTON_SRC io/fstack-dpdk.cpp)
endif ()
if (PHOTON_ENABLE_SPDK_BDEV)
    list(APPEND PHOTON_SRC io/spdkbdev-wrapper.cpp)
endif ()
if (PHOTON_ENABLE_EXTFS)
    file(GLOB EXTFS_SRC fs/extfs/*.cpp)
    list(APPEND PHOTON_SRC ${EXTFS_SRC})
endif ()
if (PHOTON_ENABLE_ECOSYSTEM)
    file(GLOB ECOSYSTEM_SRC ecosystem/*.cpp)
    list(APPEND PHOTON_SRC ${ECOSYSTEM_SRC})
endif ()
if (PHOTON_ENABLE_RSOCKET)
    list(APPEND PHOTON_SRC net/rsocket/rsocket.cpp)
endif ()
if (NOT ${PHOTON_ENABLE_LIBCURL})
    list(REMOVE_ITEM PHOTON_SRC net/curl.cpp fs/httpfs/httpfs.cpp)
endif ()
if (PHOTON_BUILD_OCF_CACHE)
    add_subdirectory(fs/cache/ocf_cache)
endif()


# An object library compiles source files but does not archive or link their object files.
add_library(photon_obj OBJECT ${PHOTON_SRC})
if (PHOTON_ENABLE_ECOSYSTEM)
    target_link_libraries(photon_obj PRIVATE ecosystem_deps)
endif ()
target_include_directories(photon_obj PRIVATE include ${OPENSSL_INCLUDE_DIRS} ${AIO_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS}
)

if (PHOTON_ENABLE_FUSE STREQUAL "ON" OR PHOTON_ENABLE_FUSE STREQUAL "2")
    target_compile_definitions(photon_obj PRIVATE _FILE_OFFSET_BITS=64 FUSE_USE_VERSION=29)
    target_include_directories(photon_obj PRIVATE ${FUSE_INCLUDE_DIRS})
elseif (PHOTON_ENABLE_FUSE STREQUAL "3")
    target_compile_definitions(photon_obj PRIVATE _FILE_OFFSET_BITS=64 FUSE_USE_VERSION=317)
    target_include_directories(photon_obj PRIVATE ${FUSE3_INCLUDE_DIRS})
endif ()
if (PHOTON_GLOBAL_INIT_OPENSSL)
    target_compile_definitions(photon_obj PRIVATE PHOTON_GLOBAL_INIT_OPENSSL)
endif ()
if (PHOTON_ENABLE_SASL)
    target_include_directories(photon_obj PRIVATE ${GSASL_INCLUDE_DIRS})
endif ()
if (PHOTON_ENABLE_URING)
    target_include_directories(photon_obj PRIVATE ${URING_INCLUDE_DIRS})
    target_compile_definitions(photon_obj PRIVATE PHOTON_URING=on)
endif()
if (PHOTON_ENABLE_MIMIC_VDSO)
    target_compile_definitions(photon_obj PRIVATE ENABLE_MIMIC_VDSO=on)
endif()
if (PHOTON_ENABLE_FSTACK_DPDK)
    target_compile_definitions(photon_obj PRIVATE ENABLE_FSTACK_DPDK)
    target_include_directories(photon_obj PRIVATE ${FSTACK_INCLUDE_DIRS})
endif()
if (PHOTON_ENABLE_SPDK_BDEV)
    target_compile_definitions(photon_obj PRIVATE ENABLE_SPDK_BDEV)
    target_include_directories(photon_obj PRIVATE ${SPDK_INCLUDE_DIRS})
endif()
if (PHOTON_ENABLE_EXTFS)
    target_include_directories(photon_obj PRIVATE ${E2FS_INCLUDE_DIRS})
endif()
if (PHOTON_ENABLE_LIBCURL)
    target_compile_definitions(photon_obj PRIVATE ENABLE_CURL)
endif()
if (PHOTON_DEFAULT_LOG_LEVEL)
    target_compile_definitions(photon_obj PRIVATE DEFAULT_LOG_LEVEL=${PHOTON_DEFAULT_LOG_LEVEL})
endif()

if (actually_built)
    add_dependencies(photon_obj ${actually_built})
endif ()

################################################################################

set(static_deps
        easy_weak
        fstack_weak
)
set(shared_deps
        -lpthread
)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND shared_deps -lgcc)  # solve [hidden symbol `__cpu_model'] problem
endif ()
if (NOT APPLE)
    list(APPEND static_deps ${AIO_LIBRARIES})
    list(APPEND shared_deps -lrt -ldl)
endif ()
if (PHOTON_ENABLE_URING)
    list(APPEND static_deps ${URING_LIBRARIES})
endif ()
if (PHOTON_ENABLE_FUSE STREQUAL "ON" OR PHOTON_ENABLE_FUSE STREQUAL "2")
    list(APPEND static_deps ${FUSE_LIBRARIES})
elseif (PHOTON_ENABLE_FUSE STREQUAL "3")
    list(APPEND static_deps ${FUSE3_LIBRARIES})
endif ()
if (PHOTON_ENABLE_SASL)
    list(APPEND static_deps ${GSASL_LIBRARIES})
endif ()
if (PHOTON_ENABLE_FSTACK_DPDK)
    list(APPEND static_deps ${FSTACK_LIBRARIES})
endif ()
if (PHOTON_ENABLE_SPDK_BDEV)
    list(APPEND static_deps ${SPDK_LIBRARIES})
endif ()
if (PHOTON_ENABLE_EXTFS)
    list(APPEND static_deps ${E2FS_LIBRARIES})
endif ()
if (PHOTON_ENABLE_RSOCKET)
    list(APPEND shared_deps ${RDMACORE_LIBRARIES})
endif ()
if (PHOTON_ENABLE_LIBCURL)
    list(APPEND static_deps ${CURL_LIBRARIES})
endif ()
if (PHOTON_BUILD_OCF_CACHE)
    list(APPEND shared_deps ocf_cache_lib)
    list(APPEND static_deps ocf_cache_lib)
endif()
list(APPEND static_deps ${OPENSSL_LIBRARIES} ${ZLIB_LIBRARIES})


# Find out dynamic libs and append to `shared_deps`.
# Because if not built from source, we won't know the local packages are static or shared.
# This is for the max compatability.
if (NOT APPLE)
    set(suffix "\.so$")
else ()
    set(suffix "\.dylib$" "\.tbd$")
endif ()
foreach (dep ${static_deps})
    foreach (suf ${suffix})
        if (dep MATCHES "${suf}")
            list(APPEND shared_deps ${dep})
            break()
        endif ()
    endforeach ()
endforeach ()

set(version_scripts)
list(APPEND version_scripts "-Wl,--version-script=${PROJECT_SOURCE_DIR}/tools/libaio.map")

################################################################################

# Link photon shared lib
add_library(photon_shared SHARED $<TARGET_OBJECTS:photon_obj>)
set_target_properties(photon_shared PROPERTIES OUTPUT_NAME photon)
target_include_directories(photon_shared PUBLIC include ${CURL_INCLUDE_DIRS})
if (NOT APPLE)
    target_link_libraries(photon_shared
            PRIVATE ${version_scripts} -Wl,--whole-archive ${static_deps} -Wl,--no-whole-archive
            PUBLIC ${shared_deps}
    )
else ()
    target_link_libraries(photon_shared
            PUBLIC ${shared_deps}
            PRIVATE -Wl,-force_load ${static_deps}
    )
endif ()

# Link photon static lib
add_library(photon_static STATIC $<TARGET_OBJECTS:photon_obj>)
set_target_properties(photon_static PROPERTIES OUTPUT_NAME photon_sole)
target_include_directories(photon_static PUBLIC include ${CURL_INCLUDE_DIRS})
target_link_libraries(photon_static
        PRIVATE ${static_deps}
        PUBLIC ${shared_deps}
)

# Merge static libs into libphoton.a for manual distribution.
# Do NOT link to this target directly.
if (NOT APPLE)
    add_custom_target(_photon_static_archive ALL
            COMMAND rm -rf libphoton.a
            COMMAND ar -qcT libphoton.a $<TARGET_FILE:photon_static> $<TARGET_FILE:easy_weak> $<TARGET_FILE:fstack_weak>
            COMMAND ar -M < ${PROJECT_SOURCE_DIR}/tools/libphoton.mri
            DEPENDS photon_static
            WORKING_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
            VERBATIM
    )
else ()
    add_custom_target(_photon_static_archive ALL
            COMMAND rm -rf libphoton.a
            COMMAND libtool -static -o libphoton.a $<TARGET_FILE:photon_static> $<TARGET_FILE:easy_weak> $<TARGET_FILE:fstack_weak>
            DEPENDS photon_static
            WORKING_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
            VERBATIM
    )
endif ()

# Build examples and test cases
if (PHOTON_BUILD_TESTING)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/examples-output)
    add_subdirectory(examples)

    include(CTest)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/output)
    include(generate-ctest-packed-script)

    add_library(ci-tools OBJECT test/ci-tools.cpp)
    target_include_directories(ci-tools PRIVATE include)

    target_include_directories(photon_shared PUBLIC ${CURL_INCLUDE_DIRS} ${GFLAGS_INCLUDE_DIRS} ${GOOGLETEST_INCLUDE_DIRS})
    target_link_libraries(photon_shared PUBLIC ${CURL_LIBRARIES} ${GFLAGS_LIBRARIES} ${GOOGLETEST_LIBRARIES})
    link_libraries(ci-tools)

    add_subdirectory(common/checksum/test)
    add_subdirectory(common/test)
    add_subdirectory(common/memory-stream/test)
    add_subdirectory(common/executor/test)
    add_subdirectory(fs/test)
    add_subdirectory(io/test)
    add_subdirectory(net/test)
    add_subdirectory(net/http/test)
    add_subdirectory(rpc/test)
    add_subdirectory(thread/test)
    add_subdirectory(net/security-context/test)
    if (NOT APPLE)
        add_subdirectory(fs/cache/test)
    endif()
    if (PHOTON_ENABLE_ECOSYSTEM)
        add_subdirectory(ecosystem/test)
    endif ()
    if (PHOTON_ENABLE_EXTFS)
        add_subdirectory(fs/extfs/test)
    endif ()
    if (PHOTON_ENABLE_RSOCKET)
        add_subdirectory(net/rsocket/test)
    endif ()
    GenerateStandaloneCTestScript(${CMAKE_SOURCE_DIR} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/CTestTestfile.cmake)
endif ()

################################################################################

# Install headers and libs
add_custom_target(copy_includes ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include
    COMMENT "Copying include files to build directory: ${PROJECT_BINARY_DIR}/include"
)
add_dependencies(copy_includes photon_static)

install(DIRECTORY ${PROJECT_BINARY_DIR}/include/ DESTINATION include)
install(FILES ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libphoton.a DESTINATION lib)
install(PROGRAMS ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libphoton.so DESTINATION lib)
