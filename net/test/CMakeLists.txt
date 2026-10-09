photon_add_test(test-socket test.cpp)
photon_add_test(test-dg-socket test-udp.cpp)
photon_add_test(test-sockpool test_sockpool.cpp)

if (PHOTON_ENABLE_LIBCURL)
    photon_add_test(test-curl test_curl.cpp)
endif ()

photon_add_test(test-server test-server.cpp NO_REGISTER)
photon_add_test(test-client test-client.cpp NO_REGISTER)
photon_add_test(test-ipv6 test-ipv6.cpp)
photon_add_test(test-vdma test-vdma.cpp)

if (PHOTON_ENABLE_KCP)
    photon_add_test(test-kcp test_kcp.cpp)
endif ()
