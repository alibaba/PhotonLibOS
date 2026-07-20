find_package(PkgConfig REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/photon-dpdk-spdk-libs.cmake)

pkg_check_modules(DPDK REQUIRED libdpdk)

set(DPDK_FOUND_LIBRARIES)
foreach(LIB_NAME IN LISTS PHOTON_DPDK_LIBRARY_NAMES)
    find_library(FOUND_${LIB_NAME} NAMES ${LIB_NAME} PATHS ${DPDK_LIBRARY_DIRS} NO_DEFAULT_PATH)
    if(FOUND_${LIB_NAME})
        list(APPEND DPDK_FOUND_LIBRARIES ${FOUND_${LIB_NAME}})
    else()
        message(WARNING "Could not find DPDK library ${LIB_NAME}")
    endif()
endforeach()

set(DPDK_LIBRARIES ${DPDK_FOUND_LIBRARIES})

find_package_handle_standard_args(dpdk DEFAULT_MSG DPDK_LIBRARIES DPDK_INCLUDE_DIRS)

mark_as_advanced(DPDK_LIBRARIES DPDK_INCLUDE_DIRS)