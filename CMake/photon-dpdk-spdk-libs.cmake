## Canonical component-library name lists for DPDK and SPDK.
##
## Both the system-lookup modules (Finddpdk.cmake / Findspdk.cmake) and the
## build-from-source path (build-from-src.cmake) need the exact same set of
## component libraries; keeping the lists here makes this the single source of
## truth so the two paths cannot drift apart.
##
## Note: no include_guard() on purpose. This file only defines data via set(),
## so re-including it from a different scope simply re-publishes the variables
## there -- which is exactly what each consumer wants.

# DPDK component libraries Photon links against. These are shared objects, so
# their order in the link line is not significant.
set(PHOTON_DPDK_LIBRARY_NAMES
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

# SPDK component libraries Photon links against. isa-l is intentionally omitted:
# each consumer pulls it in differently (Findspdk via pkg-config ISAL_STATIC_LDFLAGS,
# build-from-src via the freshly built libisal in SPDK's lib dir).
set(PHOTON_SPDK_LIBRARY_NAMES
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
