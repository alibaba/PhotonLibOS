find_package(PkgConfig REQUIRED)

set(DPDK_LIBRARIES)
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
rte_mempool
rte_mempool_ring
rte_meter
rte_net
rte_pci
rte_power
rte_rcu
rte_reorder
rte_ring
rte_security
rte_telemetry
rte_timer
rte_vhost
rte_kni
rte_net_bond
)
foreach(LIB_NAME IN LISTS DPDK_LIBRARY_NAMES)
    find_library(FOUND_${LIB_NAME} NAMES ${LIB_NAME} PATHS /usr/local/lib NO_DEFAULT_PATH)
    if(FOUND_${LIB_NAME})
        list(APPEND DPDK_LIBRARIES ${FOUND_${LIB_NAME}})
    else()
        message(WARNING "Could not find DPDK library ${LIB_NAME}")
    endif()
endforeach()

find_path(DPDK_INCLUDE_DIRS NAMES rte_config.h PATHS /usr/local/include NO_DEFAULT_PATH)

find_package_handle_standard_args(dpdk DEFAULT_MSG DPDK_LIBRARIES DPDK_INCLUDE_DIRS)

mark_as_advanced(DPDK_LIBRARIES DPDK_INCLUDE_DIRS)