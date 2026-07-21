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

if (NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64) AND NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64) AND NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64))
    message(FATAL_ERROR "Unknown CPU architecture ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

# Project-wide compile options.
#
# Registered with add_compile_options() and scoped with generator expressions so
# they affect C and C++ translation units only, never ASM (nasm / gas) -- the CRC
# kernels, for instance, must not receive -Werror or -msseX. Historically these
# were appended to CMAKE_CXX_FLAGS and copied verbatim to CMAKE_C_FLAGS.
macro(photon_cc_options)
    add_compile_options(
            "$<$<COMPILE_LANGUAGE:C>:${ARGN}>"
            "$<$<COMPILE_LANGUAGE:CXX>:${ARGN}>"
    )
endmacro ()

photon_cc_options(-Werror -Wall -Wno-error=pragmas)
if (${CMAKE_CXX_COMPILER_ID} STREQUAL GNU AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
    # Hint: -faligned-new is enabled by default after -std=c++17 (C++ only).
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-faligned-new>")
endif ()

if (PHOTON_BUILD_WITH_ASAN)
    if ((NOT CMAKE_BUILD_TYPE STREQUAL "Debug") OR (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        message(FATAL_ERROR "Wrong environment")
    endif ()
    photon_cc_options(-fsanitize=address -static-libasan)
    add_link_options(-fsanitize=address -static-libasan)
endif ()

set(CMAKE_CXX_STANDARD ${PHOTON_CXX_STANDARD})
if (WIN32)
    set(CMAKE_CXX_STANDARD 17)
endif ()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake has no built-in LINUX flag (unlike WIN32 / APPLE)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LINUX TRUE)
endif ()

# CMake didn't provide an abstraction of optimization level for now.
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -DNDEBUG -g")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-O2 -march=native")    # Only for CI test
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    photon_cc_options(-Wno-packed-bitfield-compat -fno-omit-frame-pointer)
endif()
if (WIN32)
    photon_cc_options(-I${PROJECT_SOURCE_DIR}/include -include ${PROJECT_SOURCE_DIR}/common/windows_compat.h
            -Wno-attributes -Wno-unused-value -Wno-array-bounds -Wno-sign-compare
            -Wno-narrowing -Wno-unused-variable -Wno-ignored-attributes)
endif()
if (CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64)
    photon_cc_options(-msse3 -mssse3 -msse4.1 -msse4.2 -mpopcnt -mcx16)
elseif ((CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64) OR (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64))
    photon_cc_options(-fsigned-char -fno-stack-protector -fno-omit-frame-pointer)
    if (NOT CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
        photon_cc_options(-mcpu=generic+crc+crypto+lse)
    endif ()
endif ()

# The common warning / architecture options are applied to both C and C++ via
# photon_cc_options above; here we only mirror the per-configuration
# optimization levels onto the C compiler.
set(CMAKE_C_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG})
set(CMAKE_C_FLAGS_RELEASE ${CMAKE_CXX_FLAGS_RELEASE})
set(CMAKE_C_FLAGS_RELWITHDEBINFO ${CMAKE_CXX_FLAGS_RELWITHDEBINFO})

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

# Resolve the fuse tri-state once: OFF disables; ON/2 select libfuse2, 3 selects
# libfuse3. Downstream fuse references reuse these variables instead of repeating
# the tri-state comparison in each of the dependency / defs / obj-dep / link lists.
if (PHOTON_ENABLE_FUSE STREQUAL "OFF")
    set(_fuse_enabled OFF)
else ()
    set(_fuse_enabled ON)
endif ()
if (PHOTON_ENABLE_FUSE STREQUAL "3")
    set(_fuse_pkg fuse3)
    set(_fuse_version 317)
else ()
    set(_fuse_pkg fuse)
    set(_fuse_version 29)
endif ()
string(TOUPPER ${_fuse_pkg} _fuse_pkg_upper)

set(dependencies "")
photon_dependency(zlib)
photon_dependency(openssl)
photon_dependency(aio        LINUX)
photon_dependency(${_fuse_pkg} LINUX AND _fuse_enabled)
photon_dependency(uring      LINUX AND PHOTON_ENABLE_URING)
photon_dependency(gsasl      PHOTON_ENABLE_SASL)
photon_dependency(dpdk       PHOTON_ENABLE_FSTACK_DPDK OR PHOTON_ENABLE_SPDK)
photon_dependency(fstack     PHOTON_ENABLE_FSTACK_DPDK)
photon_dependency(spdk       PHOTON_ENABLE_SPDK)
photon_dependency(e2fs       PHOTON_ENABLE_EXTFS)
photon_dependency(rdmacore   PHOTON_ENABLE_RSOCKET)
photon_dependency(curl       PHOTON_ENABLE_LIBCURL)
photon_dependency(kcp        PHOTON_ENABLE_KCP)
photon_dependency(gflags     PHOTON_BUILD_TESTING)
photon_dependency(googletest PHOTON_BUILD_TESTING)

FOREACH (dep ${dependencies})
    message(STATUS "Checking dependency ${dep}")
    string(TOUPPER ${dep} DEP)
    set(source_url "${PHOTON_${DEP}_SOURCE}")
    if (dep STREQUAL "kcp")
        message(STATUS "Will build ${dep} from source")
        message(STATUS "    URL: ${source_url}")
        build_from_src(${dep})
    elseif (PHOTON_BUILD_DEPENDENCIES AND (NOT source_url STREQUAL ""))
        message(STATUS "Will build ${dep} from source")
        message(STATUS "    URL: ${source_url}")
        build_from_src(${dep})
    else ()
        message(STATUS "Will find ${dep}")
        find_package(${dep} REQUIRED)
    endif ()
endforeach ()

# Expose every resolved dependency as an internal Photon::<dep> target so its
# usage requirements propagate through the target graph. Both acquisition paths
# publish the same <DEP>_INCLUDE_DIRS / <DEP>_LIBRARIES, so a single declaration
# here covers find_package and build_from_src uniformly.
include(photon-deps)
foreach (dep ${dependencies})
    string(TOUPPER ${dep} DEP)
    photon_declare_dep(${dep} INCLUDES ${${DEP}_INCLUDE_DIRS} LIBRARIES ${${DEP}_LIBRARIES})
endforeach ()

################################################################################

add_subdirectory(third_party)

if (PHOTON_ENABLE_ECOSYSTEM)
    add_subdirectory(ecosystem)
endif ()

# Compile photon objects.
#
# Core sources (always compiled) are listed explicitly; each optional-feature
# file is opted in by its own block below. Directory globs are used only where
# every file in the directory is core -- the mixed directories (net/, fs/httpfs/)
# are enumerated file-by-file, so no glob-then-REMOVE_ITEM carve-out is needed.
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
        fs/httpfs/httpfs_v2.cpp
        io/reset_handle.cpp
        net/basic_socket.cpp
        net/datagram_socket.cpp
        net/iostream.cpp
        net/kernel_socket.cpp
        net/pooled_socket.cpp
        net/utils.cpp
        net/http/*.cpp
        net/http/huffman/*.cpp
        net/security-context/tls-stream.cpp
        net/vdma/*.cpp
        rpc/*.cpp
        thread/*.cpp
        )

# Platform-specific event engine (and, on Linux, the disk cache sources).
if (APPLE)
    list(APPEND PHOTON_SRC io/kqueue.cpp io/signal.cpp)
elseif (WIN32)
    list(APPEND PHOTON_SRC io/iocp.cpp)
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
        io/signal.cpp
        ${CACHE_SRC}
    )
    photon_append_if(PHOTON_SRC IF PHOTON_ENABLE_URING ITEMS io/iouring-wrapper.cpp)
endif ()

# Optional features -- each enabled feature contributes its own sources.
photon_append_if(PHOTON_SRC IF PHOTON_ENABLE_LIBCURL      ITEMS net/curl.cpp fs/httpfs/httpfs.cpp)
photon_append_if(PHOTON_SRC IF PHOTON_ENABLE_KCP          ITEMS net/kcp.cpp)
photon_append_if(PHOTON_SRC IF PHOTON_ENABLE_SASL         ITEMS net/security-context/sasl-stream.cpp)
photon_append_if(PHOTON_SRC IF PHOTON_ENABLE_FSTACK_DPDK  ITEMS io/fstack-dpdk.cpp)
photon_append_if(PHOTON_SRC IF PHOTON_ENABLE_SPDK         ITEMS io/spdkbdev-wrapper.cpp io/spdknvme-wrapper.cpp)
photon_append_if(PHOTON_SRC IF PHOTON_ENABLE_RSOCKET      ITEMS net/rsocket/rsocket.cpp)
if (_fuse_enabled)
    file(GLOB FUSE_ADAPTOR_SRC fs/fuse_adaptor/*.cpp)
    list(APPEND PHOTON_SRC ${FUSE_ADAPTOR_SRC})
endif ()
if (PHOTON_ENABLE_EXTFS)
    file(GLOB EXTFS_SRC fs/extfs/*.cpp)
    photon_remove_if(EXTFS_SRC IF NOT PHOTON_ENABLE_RESIZE ITEMS ${CMAKE_CURRENT_SOURCE_DIR}/fs/extfs/resize.cpp)
    list(APPEND PHOTON_SRC ${EXTFS_SRC})
endif ()
if (PHOTON_ENABLE_ECOSYSTEM)
    file(GLOB ECOSYSTEM_SRC ecosystem/*.cpp)
    list(APPEND PHOTON_SRC ${ECOSYSTEM_SRC})
endif ()
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
photon_feature(IF _fuse_enabled DEFS _FILE_OFFSET_BITS=64 FUSE_USE_VERSION=${_fuse_version})
photon_feature(IF PHOTON_GLOBAL_INIT_OPENSSL   DEFS PHOTON_GLOBAL_INIT_OPENSSL)
photon_feature(IF PHOTON_ENABLE_URING          DEFS PHOTON_URING=on)
photon_feature(IF PHOTON_ENABLE_MIMIC_VDSO     DEFS ENABLE_MIMIC_VDSO=on)
photon_feature(IF PHOTON_ENABLE_FSTACK_DPDK    DEFS ENABLE_FSTACK_DPDK)
photon_feature(IF PHOTON_ENABLE_LIBCURL        DEFS ENABLE_CURL)
photon_feature(IF PHOTON_DEFAULT_LOG_LEVEL     DEFS DEFAULT_LOG_LEVEL=${PHOTON_DEFAULT_LOG_LEVEL})
photon_feature(IF PHOTON_ENABLE_KCP            DEFS PHOTON_KCP=on)

# The dependency include dirs Photon's own sources compile against now arrive
# through the Photon::<dep> targets (see CMake/photon-deps.cmake), gated by the
# same feature conditions that added each dependency to the list above.
set(photon_obj_deps Photon::openssl Photon::zlib)
photon_append_if(photon_obj_deps IF LINUX                                                               ITEMS Photon::aio)
photon_append_if(photon_obj_deps IF _fuse_enabled ITEMS Photon::${_fuse_pkg})
photon_append_if(photon_obj_deps IF PHOTON_ENABLE_SASL                                                  ITEMS Photon::gsasl)
photon_append_if(photon_obj_deps IF PHOTON_ENABLE_URING                                                 ITEMS Photon::uring)
photon_append_if(photon_obj_deps IF PHOTON_ENABLE_FSTACK_DPDK                                           ITEMS Photon::fstack)
photon_append_if(photon_obj_deps IF PHOTON_ENABLE_SPDK                                                  ITEMS Photon::spdk)
photon_append_if(photon_obj_deps IF PHOTON_ENABLE_EXTFS                                                 ITEMS Photon::e2fs)
photon_append_if(photon_obj_deps IF PHOTON_ENABLE_KCP                                                   ITEMS Photon::kcp)

# An object library compiles source files but does not archive or link their object files.
add_library(photon_obj OBJECT ${PHOTON_SRC})
target_link_libraries(photon_obj PRIVATE photon_headers photon_features ${photon_obj_deps})
if (PHOTON_ENABLE_ECOSYSTEM)
    target_link_libraries(photon_obj PRIVATE ecosystem_deps)
endif ()

if (actually_built)
    add_dependencies(photon_obj ${actually_built})
endif ()

################################################################################

set(static_deps)
photon_append_if(static_deps IF LINUX ITEMS easy_weak fstack_weak)

set(shared_deps -lpthread)
photon_append_if(shared_deps IF CMAKE_CXX_COMPILER_ID STREQUAL "GNU" ITEMS -lgcc)  # solve [hidden symbol `__cpu_model'] problem

# aio is a Linux static dep; -lrt/-ldl accompany it on non-Apple, non-Windows hosts.
photon_append_if(static_deps IF NOT APPLE AND NOT WIN32 ITEMS ${AIO_LIBRARIES})
photon_append_if(shared_deps IF NOT APPLE AND NOT WIN32 ITEMS -lrt -ldl)
photon_append_if(shared_deps IF WIN32 ITEMS -lws2_32 -lmswsock -lcrypt32 -ladvapi32 -lgdi32 -luser32 -lbcrypt)

photon_append_if(static_deps IF PHOTON_ENABLE_URING ITEMS ${URING_LIBRARIES})
photon_append_if(static_deps IF _fuse_enabled ITEMS ${${_fuse_pkg_upper}_LIBRARIES})
photon_append_if(static_deps IF PHOTON_ENABLE_SASL ITEMS ${GSASL_LIBRARIES})
photon_append_if(static_deps IF PHOTON_ENABLE_FSTACK_DPDK ITEMS ${DPDK_LIBRARIES} ${FSTACK_LIBRARIES})
photon_append_if(static_deps IF PHOTON_ENABLE_SPDK ITEMS ${DPDK_LIBRARIES} ${SPDK_LIBRARIES})
photon_append_if(static_deps IF PHOTON_ENABLE_EXTFS ITEMS ${E2FS_LIBRARIES})
photon_append_if(shared_deps IF PHOTON_ENABLE_RSOCKET ITEMS ${RDMACORE_LIBRARIES})
photon_append_if(static_deps IF PHOTON_ENABLE_LIBCURL ITEMS ${CURL_LIBRARIES})
photon_append_if(shared_deps IF PHOTON_BUILD_OCF_CACHE ITEMS ocf_cache_lib)
photon_append_if(static_deps IF PHOTON_BUILD_OCF_CACHE ITEMS ocf_cache_lib)
list(APPEND static_deps ${OPENSSL_LIBRARIES} ${ZLIB_LIBRARIES})
photon_append_if(static_deps IF PHOTON_ENABLE_KCP ITEMS ${KCP_LIBRARIES})


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
    # Bake the MinGW runtime into libphoton.dll so the produced DLL has no
    # external dependency on libgcc_s_seh-1.dll / libstdc++-6.dll /
    # libwinpthread-1.dll.  Test and example executables that link against
    # libphoton.dll can then run under wine from build-win/output/ without
    # WINEPATH or extra DLL copies.
    #
    # libwinpthread needs its static archive referenced by absolute path
    # wrapped in --whole-archive, otherwise ld auto-links libwinpthread.dll.a
    # (the import lib) and the produced DLL ends up depending on the DLL.
    execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libwinpthread.a
            OUTPUT_VARIABLE MINGW_WINPTHREAD_STATIC
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_link_options(photon_shared PRIVATE
            -static-libgcc -static-libstdc++)
    if (MINGW_WINPTHREAD_STATIC AND EXISTS "${MINGW_WINPTHREAD_STATIC}")
        target_link_options(photon_shared PRIVATE
                -Wl,--whole-archive,${MINGW_WINPTHREAD_STATIC},--no-whole-archive)
    endif ()
endif ()

# Link photon static lib
add_library(photon_static STATIC $<TARGET_OBJECTS:photon_obj>)
set_target_properties(photon_static PROPERTIES OUTPUT_NAME photon_sole)
target_link_libraries(photon_static
        PRIVATE ${static_deps}
        PUBLIC ${shared_deps} photon_headers
)

# Merge static libs into libphoton.a for manual distribution.
# Do NOT link to this target directly.
if (LINUX)
    if (TARGET fstack_weak)
        set(_extra_archives $<TARGET_FILE:fstack_weak>)
    else ()
        set(_extra_archives "")
    endif ()
    if (TARGET easy_weak)
        set(_easy_archive $<TARGET_FILE:easy_weak>)
    else ()
        set(_easy_archive "")
    endif ()
    add_custom_target(_photon_static_archive ALL
            COMMAND rm -rf libphoton.a
            COMMAND ${CMAKE_AR} -qcT libphoton.a $<TARGET_FILE:photon_static> ${_easy_archive} ${_extra_archives}
            COMMAND ${CMAKE_AR} -M < ${PROJECT_SOURCE_DIR}/tools/libphoton.mri
            DEPENDS photon_static
            WORKING_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
            VERBATIM
    )
else ()
    if (TARGET easy_weak)
        set(_easy_archive $<TARGET_FILE:easy_weak>)
    else ()
        set(_easy_archive "")
    endif ()
    if (TARGET fstack_weak)
        set(_extra_archives $<TARGET_FILE:fstack_weak>)
    else ()
        set(_extra_archives "")
    endif ()
    add_custom_target(_photon_static_archive ALL
            COMMAND rm -rf libphoton.a
            COMMAND libtool -static -o libphoton.a $<TARGET_FILE:photon_static> ${_easy_archive} ${_extra_archives}
            DEPENDS photon_static
            WORKING_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
            VERBATIM
    )
endif ()

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
