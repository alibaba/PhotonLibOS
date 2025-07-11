# Note: Be aware of the differences between CMake project and Makefile project.
# Makefile project is not able to get BINARY_DIR at configuration.

set(actually_built)

function(build_from_src [dep])
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
                CONFIGURE_COMMAND sh -c "CFLAGS=\"-fPIC -O3\" ./configure --prefix=${BINARY_DIR} --static"
                BUILD_COMMAND $(MAKE)
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
        ExternalProject_Add(
                gflags
                URL ${PHOTON_GFLAGS_SOURCE}
                URL_MD5 1a865b93bacfa963201af3f75b7bd64c
                CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                INSTALL_COMMAND ""
        )
        if (CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(POSTFIX "_debug")
        endif ()
        ExternalProject_Get_Property(gflags BINARY_DIR)
        set(GFLAGS_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        set(GFLAGS_LIBRARIES ${BINARY_DIR}/lib/libgflags${POSTFIX}.a PARENT_SCOPE)

    elseif (dep STREQUAL "googletest")
        ExternalProject_Add(
                googletest
                URL ${PHOTON_GOOGLETEST_SOURCE}
                URL_MD5 e82199374acdfda3f425331028eb4e2a
                CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DINSTALL_GTEST=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                INSTALL_COMMAND ""
        )
        ExternalProject_Get_Property(googletest SOURCE_DIR)
        ExternalProject_Get_Property(googletest BINARY_DIR)
        set(GOOGLETEST_INCLUDE_DIRS ${SOURCE_DIR}/googletest/include ${SOURCE_DIR}/googlemock/include PARENT_SCOPE)
        set(GOOGLETEST_LIBRARIES ${BINARY_DIR}/lib/libgmock.a ${BINARY_DIR}/lib/libgtest_main.a ${BINARY_DIR}/lib/libgtest.a PARENT_SCOPE)

    elseif (dep STREQUAL "openssl")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/openssl-build)
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
        set(OPENSSL_ROOT_DIR ${BINARY_DIR} PARENT_SCOPE)
        set(OPENSSL_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        set(OPENSSL_LIBRARIES ${BINARY_DIR}/lib/libssl.a ${BINARY_DIR}/lib/libcrypto.a PARENT_SCOPE)
        set(OPENSSL_LIBRARY_DIR ${BINARY_DIR}/lib)

    elseif (dep STREQUAL "curl")
        if (${OPENSSL_ROOT_DIR} STREQUAL "")
            message(FATAL_ERROR "OPENSSL_ROOT_DIR not exist")
        endif ()
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/curl-build)
        ExternalProject_Add(
                curl
                URL ${PHOTON_CURL_SOURCE}
                URL_MD5 1211d641ae670cebce361ab6a7c6acff
                CMAKE_ARGS -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR} -DCMAKE_INSTALL_PREFIX=${BINARY_DIR} -DCMAKE_INSTALL_LIBDIR=lib
                    -DBUILD_SHARED_LIBS=OFF -DHTTP_ONLY=ON -DBUILD_CURL_EXE=OFF
                    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCURL_USE_LIBSSH2=OFF
        )
        add_dependencies(curl openssl)
        set(CURL_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        if (EXISTS ${BINARY_DIR}/lib64/libcurl.a)
            set(_curl_lib_path ${BINARY_DIR}/lib64/libcurl.a)
        else ()
            set(_curl_lib_path ${BINARY_DIR}/lib/libcurl.a)
        endif ()
        set(CURL_LIBRARIES ${_curl_lib_path} PARENT_SCOPE)

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
        set(DPDK_LIBRARY_NAMES
        rte_bus_pci
        rte_bus_vdev
        rte_cmdline
        rte_compressdev
        rte_cryptodev
        rte_eal
        rte_ethdev
        rte_hash
        rte_kvargs
        rte_mbuf
        rte_kni
        rte_mempool
        rte_mempool_ring
        rte_meter
        rte_net
        rte_net_bond
        rte_pci
        rte_power
        rte_rcu
        rte_reorder
        rte_ring
        rte_security
        rte_telemetry
        rte_timer
        rte_vhost
        )
        foreach(LIB_NAME IN LISTS DPDK_LIBRARY_NAMES)
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
        set(SPDK_LIBRARY_NAMES
        spdk_accel_ioat
        spdk_accel
        spdk_bdev_aio
        spdk_bdev_delay
        spdk_bdev_error
        spdk_bdev_ftl
        spdk_bdev_gpt
        spdk_bdev_lvol
        spdk_bdev_malloc
        spdk_bdev_null
        spdk_bdev_nvme
        spdk_bdev_passthru
        spdk_bdev
        spdk_bdev_raid
        spdk_bdev_split
        spdk_bdev_virtio
        spdk_bdev_zone_block
        spdk_blob_bdev
        spdk_blobfs_bdev
        spdk_blobfs
        spdk_blob
        spdk_conf
        spdk_env_dpdk
        spdk_env_dpdk_rpc
        spdk_event_accel
        spdk_event_bdev
        spdk_event_iscsi
        spdk_event_nbd
        spdk_event_net
        spdk_event_nvmf
        spdk_event
        spdk_event_scsi
        spdk_event_sock
        spdk_event_vhost
        spdk_event_vmd
        spdk_ftl
        spdk_ioat
        spdk_iscsi
        spdk_json
        spdk_jsonrpc
        spdk_lvol
        spdk_nbd
        spdk_net
        spdk_notify
        spdk_nvme
        spdk_nvmf
        spdk_rpc
        spdk_scsi
        spdk_sock
        spdk_sock_posix
        spdk_thread
        spdk_trace
        spdk_util
        spdk_vhost
        spdk_virtio
        spdk_vmd
        isal
        )
        foreach(LIB_NAME IN LISTS SPDK_LIBRARY_NAMES)
            list(APPEND SPDK_LIBRARIES ${SPDK_LIBRARY_DIR}/lib${LIB_NAME}.so)
        endforeach()

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

    endif ()

    list(APPEND actually_built ${dep})
    set(actually_built ${actually_built} PARENT_SCOPE)
endfunction()
