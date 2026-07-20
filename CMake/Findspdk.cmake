find_package(PkgConfig REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/photon-dpdk-spdk-libs.cmake)

set(SPDK_LIBRARIES)
foreach(LIB_NAME IN LISTS PHOTON_SPDK_LIBRARY_NAMES)
    find_library(FOUND_${LIB_NAME} NAMES ${LIB_NAME} PATHS /usr/local/lib NO_DEFAULT_PATH)
    if(FOUND_${LIB_NAME})
        list(APPEND SPDK_LIBRARIES ${FOUND_${LIB_NAME}})
    else()
        message(WARNING "SPDK library not found: ${LIB_NAME}")
    endif()
endforeach()

find_path(SPDK_INCLUDE_DIRS NAMES spdk/env.h PATHS /usr/local/include NO_DEFAULT_PATH)


pkg_check_modules(ISAL REQUIRED libisal)

set(SPDK_LIBRARIES ${SPDK_LIBRARIES} ${ISAL_STATIC_LDFLAGS})

find_package_handle_standard_args(spdk DEFAULT_MSG SPDK_INCLUDE_DIRS SPDK_LIBRARIES)

mark_as_advanced(SPDK_INCLUDE_DIRS SPDK_LIBRARIES)