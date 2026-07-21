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
include(CMake/photon-helpers.cmake)

# Options
photon_option(PHOTON_CXX_STANDARD        STRING "14"  "C++ standard")
photon_option(PHOTON_BUILD_TESTING       BOOL   OFF   "enable build testing")
photon_option(PHOTON_BUILD_WITH_ASAN     BOOL   OFF   "build with asan")
photon_option(PHOTON_ENABLE_URING        BOOL   OFF   "enable io_uring function")
photon_option(PHOTON_ENABLE_FUSE         STRING "OFF" "enable fuse version, OFF for disable, ON/2 for libfuse2, 3 for libfuse3")
photon_option(PHOTON_GLOBAL_INIT_OPENSSL BOOL   ON    "Turn this off if any of your third-party libs inits old-version OpenSSL as well,
because Photon will register coroutine locks for crypto. But don't bother if you have latest OpenSSL >= 1.1.0")
photon_option(PHOTON_ENABLE_SASL         BOOL   OFF   "enable sasl")
photon_option(PHOTON_ENABLE_MIMIC_VDSO   BOOL   OFF   "enable mimic vdso")
photon_option(PHOTON_ENABLE_FSTACK_DPDK  BOOL   OFF   "Use f-stack + DPDK as the event engine")
photon_option(PHOTON_ENABLE_SPDK         BOOL   OFF   "Use SPDK bdev && nvme concept")
photon_option(PHOTON_ENABLE_EXTFS        BOOL   OFF   "enable extfs")
photon_option(PHOTON_ENABLE_RESIZE       BOOL   OFF   "enable resize_extfs (requires external resize_fs)")
photon_option(PHOTON_ENABLE_ECOSYSTEM    BOOL   OFF   "enable ecosystem")
photon_option(PHOTON_ENABLE_RSOCKET      BOOL   OFF   "enable rsocket")
photon_option(PHOTON_ENABLE_LIBCURL      BOOL   ON    "enable libcurl")
photon_option(PHOTON_DEFAULT_LOG_LEVEL   STRING "0"   "default log level")
photon_option(PHOTON_BUILD_OCF_CACHE     BOOL   OFF   "enable ocf cache")
photon_option(PHOTON_ENABLE_KCP          BOOL   OFF   "enable kcp")

# Download locations for build-from-source (used when PHOTON_BUILD_DEPENDENCIES is ON).
photon_option(PHOTON_BUILD_DEPENDENCIES  BOOL   OFF   "")
photon_source(AIO_SOURCE        "https://pagure.io/libaio/archive/libaio-0.3.113/libaio-0.3.113.tar.gz")
photon_source(ZLIB_SOURCE       "https://github.com/madler/zlib/releases/download/v1.2.13/zlib-1.2.13.tar.gz")
photon_source(OPENSSL_SOURCE    "https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1w/openssl-1.1.1w.tar.gz")
photon_source(CURL_SOURCE       "https://github.com/curl/curl/releases/download/curl-7_88_1/curl-7.88.1.tar.gz")
photon_source(URING_SOURCE      "https://github.com/axboe/liburing/archive/refs/tags/liburing-2.3.tar.gz")
photon_source(FUSE_SOURCE       "")
photon_source(GSASL_SOURCE      "")
photon_source(DPDK_SOURCE       "https://github.com/DPDK/dpdk/archive/refs/tags/v20.11.6.tar.gz")
photon_source(FSTACK_SOURCE     "https://github.com/F-Stack/f-stack/archive/refs/tags/v1.22.tar.gz")
# Since spdk release package contains no submodules that needed to build
# we use git clone to get the source code
photon_source(ISAL_SOURCE       "https://github.com/spdk/isa-l/archive/2df39cf5f1b9ccaa2973f6ef273857e4dc46f0cf.zip")
photon_source(SPDK_SOURCE       "https://github.com/spdk/spdk/archive/refs/tags/v21.04.tar.gz")
photon_source(E2FS_SOURCE       "")
photon_source(GFLAGS_SOURCE     "https://github.com/gflags/gflags/archive/refs/tags/v2.2.2.tar.gz")
photon_source(GOOGLETEST_SOURCE "https://github.com/google/googletest/archive/refs/tags/release-1.12.1.tar.gz")
photon_source(RAPIDJSON_GIT     "https://github.com/Tencent/rapidjson.git")
photon_source(RAPIDXML_SOURCE   "https://github.com/photonlibos/rapidxml")
photon_source(RAPIDYAML_SOURCE  "https://github.com/biojppm/rapidyaml/releases/download/v0.5.0/rapidyaml-0.5.0.hpp")
# It's not recommended to build rdmacore from source because it has too many dependencies. Install it to local host.
# The release version we use is https://github.com/linux-rdma/rdma-core/releases/download/v58.0/rdma-core-58.0.tar.gz
photon_source(RDMACORE_SOURCE   "")
photon_source(KCP_SOURCE        "https://github.com/skywind3000/kcp/archive/refs/tags/2.1.1.tar.gz")

# Validate the target CPU and register the project-wide toolchain: baseline
# warnings, C++ standard, LINUX flag, default build type, module path and output
# directories. See CMake/photon-helpers.cmake.
photon_configure_build()

# Per-configuration optimization levels (CMake exposes no optimization-level knob
# of its own; C mirrors C++) plus a few global switches.
set(CMAKE_CXX_FLAGS_DEBUG           "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE         "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO  "-O2 -DNDEBUG -g")
set(CMAKE_CXX_FLAGS_MINSIZEREL      "-O2 -march=native")   # Only for CI test
set(CMAKE_C_FLAGS_DEBUG             "${CMAKE_CXX_FLAGS_DEBUG}")
set(CMAKE_C_FLAGS_RELEASE           "${CMAKE_CXX_FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_RELWITHDEBINFO    "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
set(CMAKE_EXPORT_COMPILE_COMMANDS   ON)
set(CMAKE_BUILD_RPATH_USE_ORIGIN    ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Extra compile flags, added when the condition holds and reaching C / C++ only
# (never ASM). Read as: on <compiler/platform/arch>, add <flags>.
photon_cc_flags(IF CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
        FLAGS -Wno-packed-bitfield-compat -fno-omit-frame-pointer)
photon_cc_flags(IF WIN32
        FLAGS -I${PROJECT_SOURCE_DIR}/include -include ${PROJECT_SOURCE_DIR}/common/windows_compat.h
              -Wno-attributes -Wno-unused-value -Wno-array-bounds -Wno-sign-compare
              -Wno-narrowing -Wno-unused-variable -Wno-ignored-attributes)
photon_cc_flags(IF CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64
        FLAGS -msse3 -mssse3 -msse4.1 -msse4.2 -mpopcnt -mcx16)
photon_cc_flags(IF (CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64) OR (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64)
        FLAGS -fsigned-char -fno-stack-protector -fno-omit-frame-pointer)
photon_cc_flags(IF ((CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64) OR (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64)) AND (NOT CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
        FLAGS -mcpu=generic+crc+crypto+lse)

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

# Resolve the PHOTON_ENABLE_FUSE tri-state into _fuse_enabled / _fuse_pkg /
# _fuse_version, which the fuse module below consumes.
photon_resolve_fuse()

# ---- Module graph ----------------------------------------------------------
# Every buildable unit is declared once here with photon_module(): its sources,
# internal deps (REQUIRES), external deps (STATIC/SHARED/FETCH/ARCHIVE), optional gate
# (OPTION/ENABLE) and defines. The enabled set drives dependency acquisition,
# the single photon_obj compile and the final link line -- see the helpers in
# CMake/photon-helpers.cmake. To add a module, add one photon_module() call.
set(dependencies "")

photon_module(core
        SOURCES common/*.cpp common/checksum/*.cpp common/executor/*.cpp common/memory-stream/*.cpp
                fs/aligned-file.cpp fs/async_filesystem.cpp fs/exportfs.cpp fs/filecopy.cpp fs/localfs.cpp
                fs/path.cpp fs/subfs.cpp fs/throttled-file.cpp fs/virtual-file.cpp fs/xfile.cpp
                fs/httpfs/httpfs_v2.cpp
                io/reset_handle.cpp
                net/basic_socket.cpp net/datagram_socket.cpp net/iostream.cpp
                net/kernel_socket.cpp net/pooled_socket.cpp net/utils.cpp
                thread/*.cpp)

# Platform event engines -- always-on within their environment.
photon_module(epoll
        ENABLE  LINUX
        SOURCES io/epoll.cpp io/epoll-ng.cpp)
photon_module(aio
        ENABLE  LINUX
        SOURCES io/aio-wrapper.cpp
        STATIC  aio)
photon_module(signal
        ENABLE  NOT WIN32
        SOURCES io/signal.cpp)
photon_module(kqueue
        ENABLE  APPLE
        SOURCES io/kqueue.cpp)
photon_module(iocp
        ENABLE  WIN32
        SOURCES io/iocp.cpp)
photon_module(cache
        ENABLE  LINUX
        SOURCES fs/cache/*.cpp fs/cache/full_file_cache/*.cpp fs/cache/persistent_cache/*.cpp)

# Always-on higher-level modules.
photon_module(tls
        SOURCES net/security-context/tls-stream.cpp
        STATIC  openssl)
photon_module(http
        REQUIRES tls
        SOURCES  net/http/*.cpp net/http/huffman/*.cpp
        STATIC   zlib)
photon_module(rpc
        REQUIRES tls
        SOURCES  rpc/*.cpp)
photon_module(vdma
        SOURCES net/vdma/*.cpp)

# Optional features.
photon_module(uring
        OPTION  PHOTON_ENABLE_URING
        ENABLE  LINUX
        SOURCES io/iouring-wrapper.cpp
        STATIC  uring
        DEFS    PHOTON_URING=on)
photon_module(libcurl
        OPTION    PHOTON_ENABLE_LIBCURL
        SOURCES   net/curl.cpp fs/httpfs/httpfs.cpp
        STATIC    curl
        DEFS      ENABLE_CURL)
photon_module(kcp
        OPTION  PHOTON_ENABLE_KCP
        SOURCES net/kcp.cpp
        STATIC  kcp
        DEFS    PHOTON_KCP=on)
photon_module(sasl
        OPTION  PHOTON_ENABLE_SASL
        SOURCES net/security-context/sasl-stream.cpp
        STATIC  gsasl)
photon_module(fstack_dpdk
        OPTION    PHOTON_ENABLE_FSTACK_DPDK
        SOURCES   io/fstack-dpdk.cpp
        STATIC    fstack dpdk
        DEFS      ENABLE_FSTACK_DPDK)
photon_module(spdk
        OPTION    PHOTON_ENABLE_SPDK
        SOURCES   io/spdkbdev-wrapper.cpp io/spdknvme-wrapper.cpp
        STATIC    spdk dpdk)
photon_module(rsocket
        OPTION    PHOTON_ENABLE_RSOCKET
        SOURCES   net/rsocket/rsocket.cpp
        SHARED    rdmacore)
photon_module(fuse
        ENABLE  _fuse_enabled
        SOURCES fs/fuse_adaptor/*.cpp
        STATIC  ${_fuse_pkg}
        DEFS    _FILE_OFFSET_BITS=64 FUSE_USE_VERSION=${_fuse_version})
photon_module(extfs
        OPTION  PHOTON_ENABLE_EXTFS
        SOURCES fs/extfs/extfs.cpp fs/extfs/extfs_io.cpp fs/extfs/mkfs.cpp
        STATIC  e2fs)
photon_module(resize
        OPTION  PHOTON_ENABLE_RESIZE
        ENABLE  PHOTON_ENABLE_EXTFS
        SOURCES fs/extfs/resize.cpp)
photon_module(ecosystem
        OPTION   PHOTON_ENABLE_ECOSYSTEM
        REQUIRES http
        SOURCES  ecosystem/*.cpp
        OBJ_LINK ecosystem_deps)
photon_module(testing
        OPTION    PHOTON_BUILD_TESTING
        FETCH     gflags googletest)

photon_module_validate()
list(REMOVE_DUPLICATES dependencies)

# Acquire each dependency (build-from-source or find_package) and expose it as an
# internal Photon::<dep> target. See CMake/photon-helpers.cmake.
photon_acquire_dependencies()

################################################################################

add_subdirectory(third_party)

if (PHOTON_ENABLE_ECOSYSTEM)
    add_subdirectory(ecosystem)
endif ()

# Compile photon objects.
#
# Every source now lives on a module declared above; the modules are turned into
# INTERFACE libraries further down and their sources compile once into the single
# photon_obj object library. Only the ocf cache (an external CMake subproject,
# not a photon module) is still pulled in explicitly here.
if (PHOTON_BUILD_OCF_CACHE)
    add_subdirectory(fs/cache/ocf_cache)
endif()


# Photon's own public headers (and libcurl's, when enabled), propagated to
# everything that links libphoton.
add_library(photon_headers INTERFACE)
target_include_directories(photon_headers INTERFACE ${PROJECT_SOURCE_DIR}/include)
if (PHOTON_ENABLE_LIBCURL)
    target_include_directories(photon_headers INTERFACE ${CURL_INCLUDE_DIRS})
endif ()

# Photon's own build-time feature toggles. These are Photon's choices, not usage
# requirements contributed by the dependencies, so they live on a dedicated
# INTERFACE target rather than on the Photon::<dep> targets.
add_library(photon_features INTERFACE)
photon_feature(IF PHOTON_GLOBAL_INIT_OPENSSL   DEFS PHOTON_GLOBAL_INIT_OPENSSL)
photon_feature(IF PHOTON_ENABLE_MIMIC_VDSO     DEFS ENABLE_MIMIC_VDSO=on)
photon_feature(IF PHOTON_DEFAULT_LOG_LEVEL     DEFS DEFAULT_LOG_LEVEL=${PHOTON_DEFAULT_LOG_LEVEL})

# Create the per-module INTERFACE libraries now that the Photon::<dep> targets
# exist, then compile every enabled module's sources once into a single object
# library. photon.cpp seeds photon_obj (guaranteeing it is non-empty); the rest
# of the sources arrive through the module INTERFACE libraries.
photon_module_targets()

add_library(photon_obj OBJECT ${PROJECT_SOURCE_DIR}/photon.cpp)
target_link_libraries(photon_obj PRIVATE photon_headers photon_features)
photon_module_link_into(photon_obj)

if (actually_built)
    add_dependencies(photon_obj ${actually_built})
endif ()

################################################################################

set(static_deps)
photon_append_if(static_deps IF LINUX ITEMS easy_weak fstack_weak)

set(shared_deps -lpthread)
photon_append_if(shared_deps IF CMAKE_CXX_COMPILER_ID STREQUAL "GNU" ITEMS -lgcc)  # solve [hidden symbol `__cpu_model'] problem

# aio/-lrt/-ldl accompany the Linux/unix hosts; the aio libraries themselves now
# arrive from the aio module via photon_module_collect_libs below.
photon_append_if(shared_deps IF NOT APPLE AND NOT WIN32 ITEMS -lrt -ldl)
photon_append_if(shared_deps IF WIN32 ITEMS -lws2_32 -lmswsock -lcrypt32 -ladvapi32 -lgdi32 -luser32 -lbcrypt)

# Every enabled module contributes its declared external libraries here: STATIC
# deps to static_deps, SHARED deps to shared_deps (openssl<-tls, zlib<-http,
# aio<-aio, curl<-libcurl, rdmacore<-rsocket, ...). ARCHIVE deps (none today) are
# gathered separately for merging into the distributable libphoton.a below.
photon_module_collect_libs(static_deps shared_deps)

set(archive_deps)
photon_module_collect_archives(archive_deps)

photon_append_if(shared_deps IF PHOTON_BUILD_OCF_CACHE ITEMS ocf_cache_lib)
photon_append_if(static_deps IF PHOTON_BUILD_OCF_CACHE ITEMS ocf_cache_lib)


# Some acquired static_deps are actually shared libraries (a local find_package
# may return a .so/.dylib); route those to shared_deps too, for max compatibility.
photon_reclassify_shared_deps(static_deps shared_deps)

set(version_scripts)
if (LINUX)
    list(APPEND version_scripts "-Wl,--version-script=${PROJECT_SOURCE_DIR}/tools/libaio.map")
endif ()

################################################################################

# Link photon shared lib
add_library(photon_shared SHARED $<TARGET_OBJECTS:photon_obj>)
set_target_properties(photon_shared PROPERTIES OUTPUT_NAME photon)
if (NOT APPLE)
    target_link_libraries(photon_shared
            PRIVATE ${version_scripts} -Wl,--whole-archive ${static_deps} -Wl,--no-whole-archive
            PUBLIC ${shared_deps} photon_headers
    )
else ()
    target_link_libraries(photon_shared
            PUBLIC ${shared_deps} photon_headers
            PRIVATE -Wl,-force_load ${static_deps}
    )
endif ()
if (WIN32)
    photon_bake_mingw_runtime(photon_shared)
endif ()

# Link photon static lib
add_library(photon_static STATIC $<TARGET_OBJECTS:photon_obj>)
set_target_properties(photon_static PROPERTIES OUTPUT_NAME photon_sole)
target_link_libraries(photon_static
        PRIVATE ${static_deps}
        PUBLIC ${shared_deps} photon_headers
)

# Merge photon_static (+ weak archives and any ARCHIVE deps) into a single
# distributable libphoton.a. Do NOT link against this target directly.
photon_merge_static_archive()

if (WIN32 AND PHOTON_BUILD_DEPENDENCIES)
    # httpfs.cpp includes curl/curl.h; when we build libcurl statically from
    # source, the headers must see CURL_STATICLIB or they declare functions
    # with __declspec(dllimport), which the static libcurl.a can't satisfy.
    target_compile_definitions(photon_obj PRIVATE CURL_STATICLIB)
endif ()

# Build examples and test cases
if (PHOTON_BUILD_TESTING)
    include(photon-test-helpers)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/examples-output)
    add_subdirectory(examples)

    include(CTest)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/output)
    include(generate-ctest-packed-script)

    add_library(ci-tools OBJECT test/ci-tools.cpp)
    target_include_directories(ci-tools PRIVATE include)

    target_include_directories(photon_shared PUBLIC ${CURL_INCLUDE_DIRS} ${GFLAGS_INCLUDE_DIRS} ${GOOGLETEST_INCLUDE_DIRS})
    target_link_libraries(photon_shared PUBLIC ${CURL_LIBRARIES} ${GFLAGS_LIBRARIES} ${GOOGLETEST_LIBRARIES})
    target_compile_definitions(photon_obj PRIVATE CURL_STATICLIB)
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
