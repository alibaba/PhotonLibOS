add_executable(test-socket test.cpp)
target_link_libraries(test-socket PRIVATE photon_shared)
add_test(NAME test-socket COMMAND $<TARGET_FILE:test-socket>)

add_executable(test-dg-socket test-udp.cpp)
target_link_libraries(test-dg-socket PRIVATE photon_shared)
add_test(NAME test-dg-socket COMMAND $<TARGET_FILE:test-dg-socket>)

add_executable(test-sockpool test_sockpool.cpp)
target_link_libraries(test-sockpool PRIVATE photon_shared)
add_test(NAME test-sockpool COMMAND $<TARGET_FILE:test-sockpool>)

if (PHOTON_ENABLE_LIBCURL)
    add_executable(test-curl test_curl.cpp)
    target_link_libraries(test-curl PRIVATE photon_shared)
    add_test(NAME test-curl COMMAND $<TARGET_FILE:test-curl>)
endif ()

add_executable(test-server test-server.cpp)
target_link_libraries(test-server PRIVATE photon_shared)

add_executable(test-client test-client.cpp)
target_link_libraries(test-client PRIVATE photon_shared)

add_executable(test-ipv6 test-ipv6.cpp)
target_link_libraries(test-ipv6 PRIVATE photon_shared)
add_test(NAME test-ipv6 COMMAND $<TARGET_FILE:test-ipv6>)

add_executable(test-vdma test-vdma.cpp)
target_link_libraries(test-vdma PRIVATE photon_shared)
add_test(NAME test-vdma COMMAND $<TARGET_FILE:test-vdma>)