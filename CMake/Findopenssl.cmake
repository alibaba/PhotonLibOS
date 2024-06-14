if (NOT APPLE)
    find_path(OPENSSL_INCLUDE_DIRS openssl/ssl.h openssl/crypto.h)
    find_library(OPENSSL_SSL_LIBRARIES ssl)
    find_library(OPENSSL_CRYPTO_LIBRARIES crypto)

else ()
    find_path(OPENSSL_INCLUDE_DIRS
            NAMES openssl/ssl.h openssl/crypto.h
            PATHS ${OPENSSL_ROOT_DIR}/include
            NO_DEFAULT_PATH
    )
    find_library(OPENSSL_SSL_LIBRARIES
            NAMES ssl
            PATHS ${OPENSSL_ROOT_DIR}/lib
            NO_DEFAULT_PATH
    )
    find_library(OPENSSL_CRYPTO_LIBRARIES
            NAMES crypto
            PATHS ${OPENSSL_ROOT_DIR}/lib
            NO_DEFAULT_PATH
    )
endif ()

set(OPENSSL_LIBRARIES ${OPENSSL_SSL_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARIES})

find_package_handle_standard_args(openssl DEFAULT_MSG OPENSSL_LIBRARIES OPENSSL_INCLUDE_DIRS)

mark_as_advanced(OPENSSL_INCLUDE_DIRS OPENSSL_LIBRARIES)