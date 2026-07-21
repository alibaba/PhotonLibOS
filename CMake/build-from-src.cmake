# Note: Be aware of the differences between CMake project and Makefile project.
# Makefile project is not able to get BINARY_DIR at configuration.

set(actually_built)

# DPDK / SPDK component library name lists, shared with Find{dpdk,spdk}.cmake so
# the source-build and system-lookup paths stay in sync. Located by absolute path
# because this file is included before CMAKE_MODULE_PATH is set up.
include(${CMAKE_CURRENT_LIST_DIR}/photon-dpdk-spdk-libs.cmake)

# build_from_src(<dep>) registers an ExternalProject that downloads and builds
# <dep> from source, and publishes its <DEP>_INCLUDE_DIRS / <DEP>_LIBRARIES to
# the caller (see the naming conventions in the top-level CMakeLists).
function(build_from_src dep)
    if (dep STREQUAL "aio")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/aio-build)
        ExternalProject_Add(
                aio
                URL ${PHOTON_AIO_SOURCE}
                URL_MD5 605237f35de238dfacc83bcae406d95d
                UPDATE_DISCONNECTED ON
                BUILD_IN_SOURCE ON
                CONFIGURE_COMMAND ""
                BUILD_COMMAND $(MAKE) prefix=${BINARY_DIR} install
                INSTALL_COMMAND ""
        )
        set(AIO_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        set(AIO_LIBRARIES ${BINARY_DIR}/lib/libaio.a PARENT_SCOPE)

    elseif (dep STREQUAL "zlib")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/zlib-build)
        ExternalProject_Add(
                zlib
                URL ${PHOTON_ZLIB_SOURCE}
                URL_MD5 9b8aa094c4e5765dabf4da391f00d15c
                UPDATE_DISCONNECTED ON
                BUILD_IN_SOURCE ON
                CONFIGURE_COMMAND sh -c "CC=${CMAKE_C_COMPILER} CFLAGS=\"-fPIC -O3 -include stdio.h\" ./configure --prefix=${BINARY_DIR} --static"
                BUILD_COMMAND $(MAKE) libz.a
                INSTALL_COMMAND $(MAKE) install
        )
        set(ZLIB_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        set(ZLIB_LIBRARIES ${BINARY_DIR}/lib/libz.a PARENT_SCOPE)

    elseif (dep STREQUAL "uring")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/uring-build)
        ExternalProject_Add(
                uring
                URL ${PHOTON_URING_SOURCE}
                URL_MD5 2e8c3c23795415475654346484f5c4b8
                UPDATE_DISCONNECTED ON
                BUILD_IN_SOURCE ON
                CONFIGURE_COMMAND ./configure --prefix=${BINARY_DIR}
                BUILD_COMMAND sh -c "V=1 CFLAGS=\"-fPIC -O3 -Wall -Wextra -fno-stack-protector\" $(MAKE) -C src"
                INSTALL_COMMAND $(MAKE) install
        )
        set(URING_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        set(URING_LIBRARIES ${BINARY_DIR}/lib/liburing.a PARENT_SCOPE)

    elseif (dep STREQUAL "gflags")
        # WIN32 (mingw cross-build) only differs by the extra toolchain file.
        set(cmake_args -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                       -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                       -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
        if (WIN32)
            list(APPEND cmake_args -DCMAKE_TOOLCHAIN_FILE=${PROJECT_SOURCE_DIR}/CMake/toolchain-mingw-external.cmake)
        endif ()
        ExternalProject_Add(
                gflags
                URL ${PHOTON_GFLAGS_SOURCE}
                URL_MD5 1a865b93bacfa963201af3f75b7bd64c
                CMAKE_ARGS ${cmake_args}
                INSTALL_COMMAND ""
        )
        if (CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(POSTFIX "_debug")
        endif ()
        ExternalProject_Get_Property(gflags BINARY_DIR)
        set(GFLAGS_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        # gflags's output name varies by platform/config:
        #   Windows (mingw): libgflags_static.a (verified working)
        #   Linux / macOS:   libgflags.a (no _static suffix; POSTFIX may or may
        #                    not be honored — usually empty on Release,
        #                    _debug on Debug, depending on gflags version)
        # Pick the most likely name per platform; Make resolves at build time.
        if (WIN32)
            set(GFLAGS_LIBRARIES ${BINARY_DIR}/lib/libgflags_static${POSTFIX}.a)
        else ()
            set(GFLAGS_LIBRARIES ${BINARY_DIR}/lib/libgflags${POSTFIX}.a)
        endif ()
        if (WIN32)
            list(APPEND GFLAGS_LIBRARIES -lshlwapi)
        endif ()
        set(GFLAGS_LIBRARIES ${GFLAGS_LIBRARIES} PARENT_SCOPE)

    elseif (dep STREQUAL "googletest")
        # WIN32 (mingw cross-build) only differs by the extra toolchain file.
        set(cmake_args -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                       -DINSTALL_GTEST=OFF
                       -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                       -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
        if (WIN32)
            list(APPEND cmake_args -DCMAKE_TOOLCHAIN_FILE=${PROJECT_SOURCE_DIR}/CMake/toolchain-mingw-external.cmake)
        endif ()
        ExternalProject_Add(
                googletest
                URL ${PHOTON_GOOGLETEST_SOURCE}
                URL_MD5 e82199374acdfda3f425331028eb4e2a
                CMAKE_ARGS ${cmake_args}
                INSTALL_COMMAND ""
        )
        ExternalProject_Get_Property(googletest SOURCE_DIR)
        ExternalProject_Get_Property(googletest BINARY_DIR)
        set(GOOGLETEST_INCLUDE_DIRS ${SOURCE_DIR}/googletest/include ${SOURCE_DIR}/googlemock/include PARENT_SCOPE)
        set(GOOGLETEST_LIBRARIES ${BINARY_DIR}/lib/libgmock.a ${BINARY_DIR}/lib/libgtest_main.a ${BINARY_DIR}/lib/libgtest.a PARENT_SCOPE)

    elseif (dep STREQUAL "openssl")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/openssl-build)
        if (WIN32)
            ExternalProject_Add(
                    openssl
                    URL ${PHOTON_OPENSSL_SOURCE}
                    URL_MD5 3f76825f195e52d4b10c70040681a275
                    UPDATE_DISCONNECTED ON
                    BUILD_IN_SOURCE ON
                    CONFIGURE_COMMAND ./Configure mingw64 -fPIC --prefix=${BINARY_DIR} --openssldir=${BINARY_DIR} no-shared --cross-compile-prefix=x86_64-w64-mingw32-
                    BUILD_COMMAND $(MAKE) build_libs
                    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${BINARY_DIR}/lib ${BINARY_DIR}/include
                        COMMAND ${CMAKE_COMMAND} -E copy libcrypto.a ${BINARY_DIR}/lib/
                        COMMAND ${CMAKE_COMMAND} -E copy libssl.a ${BINARY_DIR}/lib/
                        COMMAND ${CMAKE_COMMAND} -E copy_directory include/openssl ${BINARY_DIR}/include/openssl
                    LOG_CONFIGURE ON
                    LOG_BUILD ON
                    LOG_INSTALL ON
            )
        else ()
            ExternalProject_Add(
                    openssl
                    URL ${PHOTON_OPENSSL_SOURCE}
                    URL_MD5 3f76825f195e52d4b10c70040681a275
                    UPDATE_DISCONNECTED ON
                    BUILD_IN_SOURCE ON
                    CONFIGURE_COMMAND ./config -fPIC --prefix=${BINARY_DIR} --openssldir=${BINARY_DIR} no-shared
                    BUILD_COMMAND $(MAKE)
                    INSTALL_COMMAND $(MAKE) install
                    LOG_CONFIGURE ON
                    LOG_BUILD ON
                    LOG_INSTALL ON
            )
        endif ()
        set(OPENSSL_ROOT_DIR ${BINARY_DIR} PARENT_SCOPE)
        set(OPENSSL_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        set(OPENSSL_LIBRARIES ${BINARY_DIR}/lib/libssl.a ${BINARY_DIR}/lib/libcrypto.a PARENT_SCOPE)
        set(OPENSSL_LIBRARY_DIR ${BINARY_DIR}/lib)

    elseif (dep STREQUAL "curl")
        if (${OPENSSL_ROOT_DIR} STREQUAL "")
            message(FATAL_ERROR "OPENSSL_ROOT_DIR not exist")
        endif ()
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/curl-build)
        set(cmake_args -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR} -DCMAKE_INSTALL_PREFIX=${BINARY_DIR} -DCMAKE_INSTALL_LIBDIR=lib
                -DBUILD_SHARED_LIBS=OFF -DHTTP_ONLY=ON -DBUILD_CURL_EXE=OFF
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCURL_USE_LIBSSH2=OFF)
        if (WIN32)
            # mingw cross-build additionally needs the policy shim, the toolchain
            # file, and the ioctlsocket feature flags CMake cannot probe when
            # cross-compiling.
            list(APPEND cmake_args
                    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
                    -DCMAKE_TOOLCHAIN_FILE=${PROJECT_SOURCE_DIR}/CMake/toolchain-mingw-external.cmake
                    -DHAVE_IOCTLSOCKET_FIONBIO=1
                    -DHAVE_IOCTLSOCKET=1)
        endif ()
        ExternalProject_Add(
                curl
                URL ${PHOTON_CURL_SOURCE}
                URL_MD5 1211d641ae670cebce361ab6a7c6acff
                CMAKE_ARGS ${cmake_args}
        )
        add_dependencies(curl openssl)
        set(CURL_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        if (EXISTS ${BINARY_DIR}/lib64/libcurl.a)
            set(_curl_lib_path ${BINARY_DIR}/lib64/libcurl.a)
        else ()
            set(_curl_lib_path ${BINARY_DIR}/lib/libcurl.a)
        endif ()
        set(CURL_LIBRARIES ${_curl_lib_path})
        if (APPLE)
            list(APPEND CURL_LIBRARIES "-framework CoreFoundation" "-framework SystemConfiguration")
        endif ()
        set(CURL_LIBRARIES ${CURL_LIBRARIES} PARENT_SCOPE)

    elseif (dep STREQUAL "rdmacore")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/rdmacore-build)
        ExternalProject_Add(
                rdmacore
                URL ${PHOTON_RDMACORE_SOURCE}
                URL_HASH SHA256=88d67897b793f42d2004eec2629ab8464e425e058f22afabd29faac0a2f54ce4
                CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${BINARY_DIR} -DCMAKE_BUILD_TYPE=RelWithDebInfo
                    -DENABLE_VALGRIND=0 -DENABLE_STATIC=1 -DNO_MAN_PAGES=1
        )

        set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${BINARY_DIR}/lib/pkgconfig")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(rdmacore REQUIRED IMPORTED_TARGET
                "librdmacm"
                "libibverbs"
        )
        # Next, use rdmacore_STATIC_LIBRARIES, rdmacore_LIBRARY_DIRS, rdmacore_INCLUDE_DIRS accordingly

    elseif (dep STREQUAL "dpdk")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/dpdk-build)
        ExternalProject_Add(
                dpdk
                URL ${PHOTON_DPDK_SOURCE}
                CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${BINARY_DIR}
                UPDATE_DISCONNECTED ON
                BUILD_IN_SOURCE ON
                CONFIGURE_COMMAND meson build --prefix ${BINARY_DIR} -Dc_args=-fPIC -Dlibdir=lib
                BUILD_COMMAND ninja -C build
                INSTALL_COMMAND meson install --no-rebuild -C build
                LOG_CONFIGURE ON
                LOG_BUILD ON
                LOG_INSTALL ON
        )
        set(DPDK_ROOT ${BINARY_DIR})
        set(DPDK_INCLUDE_DIRS ${DPDK_ROOT}/include)
        set(DPDK_LIBRARY_DIR ${DPDK_ROOT}/lib)
        foreach(LIB_NAME IN LISTS PHOTON_DPDK_LIBRARY_NAMES)
            list(APPEND DPDK_LIBRARIES ${DPDK_LIBRARY_DIR}/lib${LIB_NAME}.so)
        endforeach()
        set(DPDK_INCLUDE_DIRS ${DPDK_INCLUDE_DIRS} PARENT_SCOPE)
        set(DPDK_LIBRARY_DIR ${DPDK_LIBRARY_DIR} PARENT_SCOPE)
        set(DPDK_LIBRARIES ${DPDK_LIBRARIES} PARENT_SCOPE)

    elseif (dep STREQUAL "spdk")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/spdk-build)
        ExternalProject_Add(
                isa-l
                URL ${PHOTON_ISAL_SOURCE}
                CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${BINARY_DIR}
                DEPENDS dpdk
                UPDATE_DISCONNECTED ON
                BUILD_IN_SOURCE ON
                CONFIGURE_COMMAND ./autogen.sh COMMAND ./configure --prefix=${BINARY_DIR} --libdir=${BINARY_DIR}/lib
                BUILD_COMMAND $(MAKE)
                INSTALL_COMMAND $(MAKE) install
                LOG_CONFIGURE ON
                LOG_BUILD ON
                LOG_INSTALL ON
        )
        ExternalProject_Add(
                spdk
                URL ${PHOTON_SPDK_SOURCE}
                CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${BINARY_DIR}
                DEPENDS dpdk isa-l
                UPDATE_DISCONNECTED ON
                BUILD_IN_SOURCE ON
                CONFIGURE_COMMAND cp -rf ${PROJECT_BINARY_DIR}/isa-l-prefix/src/isa-l ${PROJECT_BINARY_DIR}/spdk-prefix/src/spdk/
                    COMMAND ./configure --with-dpdk=${PROJECT_BINARY_DIR}/dpdk-build/ --with-isal --disable-tests --disable-unit-tests --disable-examples --with-shared --prefix=${BINARY_DIR}
                BUILD_COMMAND $(MAKE) -j
                INSTALL_COMMAND $(MAKE) install
                LOG_CONFIGURE ON
                LOG_BUILD ON
                LOG_INSTALL ON
        )
        set(SPDK_ROOT ${BINARY_DIR})
        set(SPDK_INCLUDE_DIRS ${SPDK_ROOT}/include)
        set(SPDK_LIBRARY_DIR ${SPDK_ROOT}/lib)
        set(SPDK_LIBRARIES)
        foreach(LIB_NAME IN LISTS PHOTON_SPDK_LIBRARY_NAMES)
            list(APPEND SPDK_LIBRARIES ${SPDK_LIBRARY_DIR}/lib${LIB_NAME}.so)
        endforeach()
        # isa-l is built into SPDK's lib dir; the shared name list omits it (see
        # photon-dpdk-spdk-libs.cmake), so link it explicitly here.
        list(APPEND SPDK_LIBRARIES ${SPDK_LIBRARY_DIR}/libisal.so)

        set(SPDK_INCLUDE_DIRS ${SPDK_INCLUDE_DIRS} ${DPDK_INCLUDE_DIRS} PARENT_SCOPE)
        set(SPDK_LIBRARY_DIR ${SPDK_LIBRARY_DIR} PARENT_SCOPE)
        set(SPDK_LIBRARIES ${SPDK_LIBRARIES} ${DPDK_LIBRARIES} PARENT_SCOPE)

    elseif (dep STREQUAL "fstack")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/fstack-build)
        ExternalProject_Add(
                fstack
                URL ${PHOTON_FSTACK_SOURCE}
                DEPENDS dpdk
                UPDATE_DISCONNECTED ON
                BUILD_IN_SOURCE ON
                SOURCE_SUBDIR lib
                CONFIGURE_COMMAND mkdir -p ${BINARY_DIR}/bin COMMAND mkdir -p ${BINARY_DIR}/include COMMAND mkdir -p ${BINARY_DIR}/lib
                BUILD_COMMAND bash -c "PREFIX=${PROJECT_BINARY_DIR}/fstack-build PKG_CONFIG_PATH=${PROJECT_BINARY_DIR}/dpdk-build/lib/pkgconfig:$PKG_CONFIG_PATH CONF_CFLAGS=-fPIC make"
                INSTALL_COMMAND bash -c "PKG_CONFIG_PATH=${PROJECT_BINARY_DIR}/dpdk-build/lib/pkgconfig:$PKG_CONFIG_PATH make install PREFIX=${BINARY_DIR} PREFIX_INCLUDE=${BINARY_DIR}/include PREFIX_BIN=${BINARY_DIR}/bin"
                LOG_CONFIGURE ON
                LOG_BUILD ON
                LOG_INSTALL ON
        )
        set(FSTACK_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        set(FSTACK_LIBRARIES ${BINARY_DIR}/lib/libfstack.a PARENT_SCOPE)
        set(FSTACK_LIBRARIES ${FSTACK_LIBRARIES} ${DPDK_LIBRARIES} PARENT_SCOPE)

    elseif (dep STREQUAL "kcp")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/kcp-build)
        ExternalProject_Add(
            kcp
            URL ${PHOTON_KCP_SOURCE}
            UPDATE_DISCONNECTED ON
            BUILD_IN_SOURCE ON
            CONFIGURE_COMMAND ${CMAKE_COMMAND} -E make_directory ${BINARY_DIR}/lib ${BINARY_DIR}/include
            BUILD_COMMAND ${CMAKE_C_COMPILER} -c -O2 -fPIC -I<SOURCE_DIR> <SOURCE_DIR>/ikcp.c -o ikcp.o
                COMMAND ${CMAKE_AR} rcs libikcp.a ikcp.o
            INSTALL_COMMAND ${CMAKE_COMMAND} -E copy libikcp.a ${BINARY_DIR}/lib/
                COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/ikcp.h ${BINARY_DIR}/include/
        )
        ExternalProject_Get_Property(kcp SOURCE_DIR)
        set(KCP_INCLUDE_DIRS ${SOURCE_DIR} PARENT_SCOPE)
        set(KCP_LIBRARIES ${BINARY_DIR}/lib/libikcp.a PARENT_SCOPE)

    endif ()

    list(APPEND actually_built ${dep})
    set(actually_built ${actually_built} PARENT_SCOPE)
endfunction()
