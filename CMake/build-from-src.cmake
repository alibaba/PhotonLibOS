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
                CONFIGURE_COMMAND CFLAGS=-fPIC ./configure --prefix=${BINARY_DIR} --static
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
                BUILD_COMMAND V=1 CFLAGS=-fPIC $(MAKE) -C src
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
        set(GOOGLETEST_LIBRARIES ${BINARY_DIR}/lib/libgmock.a ${BINARY_DIR}/lib/libgtest.a PARENT_SCOPE)

    elseif (dep STREQUAL "openssl")
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/openssl-build)
        ExternalProject_Add(
                openssl
                URL ${PHOTON_OPENSSL_SOURCE}
                URL_MD5 bad68bb6bd9908da75e2c8dedc536b29
                UPDATE_DISCONNECTED ON
                BUILD_IN_SOURCE ON
                CONFIGURE_COMMAND ./config -fPIC --prefix=${BINARY_DIR} --openssldir=${BINARY_DIR} shared
                BUILD_COMMAND make -j 1  # https://github.com/openssl/openssl/issues/5762#issuecomment-376622684
                INSTALL_COMMAND $(MAKE) install
                LOG_CONFIGURE ON
                LOG_BUILD ON
                LOG_INSTALL ON
        )
        set(OPENSSL_ROOT_DIR ${BINARY_DIR} PARENT_SCOPE)
        set(OPENSSL_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        set(OPENSSL_LIBRARIES ${BINARY_DIR}/lib/libssl.a ${BINARY_DIR}/lib/libcrypto.a PARENT_SCOPE)

    elseif (dep STREQUAL "curl")
        if (${OPENSSL_ROOT_DIR} STREQUAL "")
            message(FATAL_ERROR "OPENSSL_ROOT_DIR not exist")
        endif ()
        set(BINARY_DIR ${PROJECT_BINARY_DIR}/curl-build)
        ExternalProject_Add(
                curl
                URL ${PHOTON_CURL_SOURCE}
                URL_MD5 a66270f11e3fbfad709600bbd1686704
                UPDATE_DISCONNECTED ON
                BUILD_IN_SOURCE ON
                CONFIGURE_COMMAND autoreconf -i COMMAND ./configure --with-ssl=${OPENSSL_ROOT_DIR}
                    --without-libssh2 --enable-static --enable-shared=no --enable-optimize
                    --disable-manual --without-libidn
                    --disable-ftp --disable-file --disable-ldap --disable-ldaps
                    --disable-rtsp --disable-dict --disable-telnet --disable-tftp
                    --disable-pop3 --disable-imap --disable-smb --disable-smtp
                    --disable-gopher --without-nghttp2 --enable-http --disable-verbose
                    --with-pic=PIC --prefix=${BINARY_DIR}
                BUILD_COMMAND $(MAKE)
                INSTALL_COMMAND $(MAKE) install
                DEPENDS openssl
                LOG_CONFIGURE ON
                LOG_BUILD ON
                LOG_INSTALL ON
        )
        set(CURL_INCLUDE_DIRS ${BINARY_DIR}/include PARENT_SCOPE)
        set(CURL_LIBRARIES ${BINARY_DIR}/lib/libcurl.a PARENT_SCOPE)
    endif ()

    list(APPEND actually_built ${dep})
    set(actually_built ${actually_built} PARENT_SCOPE)
endfunction()
