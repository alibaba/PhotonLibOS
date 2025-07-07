find_package(PkgConfig REQUIRED)

set(DPDK_INCLUDE_DIRS ${DPDK_ROOT}/include)
set(SPDK_INCLUDE_DIRS ${SPDK_ROOT}/include)

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
)
foreach(LIB_NAME IN LISTS DPDK_LIBRARY_NAMES)
    find_library(FOUND_${LIB_NAME} NAMES ${LIB_NAME} PATHS ${DPDK_ROOT}/lib NO_DEFAULT_PATH)
    if(FOUND_${LIB_NAME})
        list(APPEND DPDK_LIBRARIES ${FOUND_${LIB_NAME}})
    else()
        message(WARNING "Could not find DPDK library ${LIB_NAME}")
    endif()
endforeach()


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
)
foreach(LIB_NAME IN LISTS SPDK_LIBRARY_NAMES)
    find_library(FOUND_${LIB_NAME} NAMES ${LIB_NAME} PATHS ${SPDK_ROOT}/lib NO_DEFAULT_PATH)
    if(FOUND_${LIB_NAME})
        list(APPEND SPDK_LIBRARIES ${FOUND_${LIB_NAME}})
    else()
        message(WARNING "SPDK library not found: ${LIB_NAME}")
    endif()
endforeach()


set(SPDK_INCLUDE_DIRS ${SPDK_INCLUDE_DIRS} ${DPDK_INCLUDE_DIRS})

set(SPDK_LIBRARIES ${SPDK_LIBRARIES} ${DPDK_LIBRARIES} ${ISAL_LIBRARY})

find_package_handle_standard_args(spdk DEFAULT_MSG SPDK_LIBRARIES SPDK_INCLUDE_DIRS)

mark_as_advanced(SPDK_INCLUDE_DIRS SPDK_LIBRARIES)