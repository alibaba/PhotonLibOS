add_executable(signalfdtest signalfdtest.cpp)
target_link_libraries(signalfdtest PRIVATE photon_shared)
add_test(NAME signalfdtest COMMAND $<TARGET_FILE:signalfdtest>)

<<<<<<< HEAD
add_executable(signalfdboom signalfdboom.cpp)
target_link_libraries(signalfdboom PRIVATE photon_shared)
add_test(NAME signalfdboom COMMAND $<TARGET_FILE:signalfdboom>)
=======
# test-syncio (POSIX AIO) and test-iouring are Linux-only. Skip on macOS
# (no libaio, no io_uring) and Windows (neither).
if (LINUX)
    photon_add_test(test-syncio test-syncio.cpp)
    photon_add_test(test-iouring test-iouring.cpp)
    photon_add_test(test-iouring-uaf test-iouring-uaf.cpp)
endif ()
>>>>>>> 8e97a77 (fix(iouring): plug UAF on _async_io early returns (#1569))

add_executable(test-fork test-fork.cpp)
target_link_libraries(test-fork PRIVATE photon_shared)
if (PHOTON_ENABLE_URING)
    target_compile_definitions(test-fork PRIVATE PHOTON_URING=on)
endif()
add_test(NAME test-fork COMMAND $<TARGET_FILE:test-fork>)

if (NOT APPLE)
add_executable(test-syncio test-syncio.cpp)
target_link_libraries(test-syncio PRIVATE photon_shared)
add_test(NAME test-syncio COMMAND $<TARGET_FILE:test-syncio>)

add_executable(test-iouring test-iouring.cpp)
target_link_libraries(test-iouring PRIVATE photon_shared)
add_test(NAME test-iouring COMMAND $<TARGET_FILE:test-iouring>)
endif ()