# signalfd and fork are Linux-only kernel features. Skip these tests on
# macOS and Windows.
if (LINUX)
    add_executable(signalfdtest signalfdtest.cpp)
    target_link_libraries(signalfdtest PRIVATE photon_shared)
    add_test(NAME signalfdtest COMMAND $<TARGET_FILE:signalfdtest>)

    add_executable(signalfdboom signalfdboom.cpp)
    target_link_libraries(signalfdboom PRIVATE photon_shared)
    add_test(NAME signalfdboom COMMAND $<TARGET_FILE:signalfdboom>)

    add_executable(test-fork test-fork.cpp)
    target_link_libraries(test-fork PRIVATE photon_shared)
    if (PHOTON_ENABLE_URING)
        target_compile_definitions(test-fork PRIVATE PHOTON_URING=on)
    endif()
    add_test(NAME test-fork COMMAND $<TARGET_FILE:test-fork>)
endif ()

# test-syncio (POSIX AIO) and test-iouring are Linux-only. Skip on macOS
# (no libaio, no io_uring) and Windows (neither).
if (LINUX)
    add_executable(test-syncio test-syncio.cpp)
    target_link_libraries(test-syncio PRIVATE photon_shared)
    add_test(NAME test-syncio COMMAND $<TARGET_FILE:test-syncio>)

    add_executable(test-iouring test-iouring.cpp)
    target_link_libraries(test-iouring PRIVATE photon_shared)
    add_test(NAME test-iouring COMMAND $<TARGET_FILE:test-iouring>)
endif ()

if (PHOTON_ENABLE_SPDK)
    add_executable(test-spdkbdev test-spdkbdev.cpp)
    target_link_libraries(test-spdkbdev PRIVATE photon_shared)
    add_test(NAME test-spdkbdev COMMAND $<TARGET_FILE:test-spdkbdev>)
    add_executable(test-spdknvme test-spdknvme.cpp)
    target_link_libraries(test-spdknvme PRIVATE photon_shared)
    add_test(NAME test-spdknvme COMMAND $<TARGET_FILE:test-spdknvme>)
endif()
