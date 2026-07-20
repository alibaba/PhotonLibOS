# gflags is provided at directory scope so it is available to every example,
# including the fstack/spdk targets whose static link order must be preserved.
link_libraries(${GFLAGS_LIBRARIES})
include_directories(${GFLAGS_INCLUDE_DIRS})

photon_add_example(simple-example simple/simple.cpp)

photon_add_example(net-perf perf/net-perf.cpp
    DEFS
        $<$<BOOL:${PHOTON_ENABLE_RSOCKET}>:PHOTON_ENABLE_RSOCKET=1>
        $<$<BOOL:${PHOTON_ENABLE_URING}>:PHOTON_URING=1>)

photon_add_example(lock-perf perf/lock-perf.cpp)
photon_add_example(workpool-perf perf/workpool-perf.cpp)

if (NOT APPLE)
    photon_add_example(io-perf perf/io-perf.cpp)
    photon_add_example(multi-conn-perf perf/multi-conn-perf.cpp)
endif ()

photon_add_example(http-perf-client perf/http/http-client.cpp)
photon_add_example(http-perf-server perf/http/http-server.cpp)
photon_add_example(rpc-example-client rpc/client.cpp rpc/client_main.cpp)
photon_add_example(rpc-example-server rpc/server.cpp rpc/server_main.cpp)
photon_add_example(sync-primitive sync-primitive/sync-primitive.cpp)
photon_add_example(vdma-example vdma-transfer/vdma-example.cpp)

if (PHOTON_ENABLE_FSTACK_DPDK)
    photon_add_example(fstack-dpdk-demo fstack-dpdk/fstack-dpdk-demo.cpp
        LIBS ${DPDK_LIBRARIES} ${FSTACK_LIBRARIES})
endif ()

if (PHOTON_ENABLE_SPDK)
    photon_add_example(bdev-example spdk/bdev_read_write.cpp
        LIBS ${DPDK_LIBRARIES} ${SPDK_LIBRARIES})
    photon_add_example(nvme-example spdk/nvme_read_write.cpp
        LIBS ${DPDK_LIBRARIES} ${SPDK_LIBRARIES})
    photon_add_example(bdev_photon_server spdk/bdev_module/bdev_photon_server_main.cpp spdk/bdev_module/bdev_photon_server.cpp
        LIBS ${DPDK_LIBRARIES} ${SPDK_LIBRARIES})
endif ()
