# signalfd and fork are Linux-only kernel features. Skip these tests on
# macOS and Windows.
if (LINUX)
    photon_add_test(signalfdtest signalfdtest.cpp)
    photon_add_test(signalfdboom signalfdboom.cpp)
    photon_add_test(test-fork test-fork.cpp
        DEFS $<$<BOOL:${PHOTON_ENABLE_URING}>:PHOTON_URING=on>)
endif ()

# test-syncio (POSIX AIO) and test-iouring are Linux-only. Skip on macOS
# (no libaio, no io_uring) and Windows (neither).
if (LINUX)
    photon_add_test(test-syncio test-syncio.cpp)
    photon_add_test(test-iouring test-iouring.cpp)
endif ()

if (PHOTON_ENABLE_SPDK)
    photon_add_test(test-spdkbdev test-spdkbdev.cpp)
    photon_add_test(test-spdknvme test-spdknvme.cpp)
endif()
