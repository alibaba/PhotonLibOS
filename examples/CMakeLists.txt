add_executable(simple-example simple/simple.cpp)
target_link_libraries(simple-example PRIVATE photon_static)

add_executable(net-perf perf/net-perf.cpp)
target_link_libraries(net-perf PRIVATE photon_static)

add_executable(rpc-example-client rpc/client.cpp rpc/client_main.cpp)
target_link_libraries(rpc-example-client PRIVATE photon_static)

add_executable(rpc-example-server rpc/server.cpp rpc/server_main.cpp)
target_link_libraries(rpc-example-server PRIVATE photon_static)

add_executable(sync-primitive sync-primitive/sync-primitive.cpp)
target_link_libraries(sync-primitive PRIVATE photon_static)

if (ENABLE_FSTACK_DPDK)
    add_executable(fstack-dpdk-demo fstack-dpdk/fstack-dpdk-demo.cpp)
    target_link_libraries(fstack-dpdk-demo PRIVATE fstack_dpdk photon_static)
endif ()
