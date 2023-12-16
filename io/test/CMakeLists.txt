add_definitions(-w)

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

if (NOT APPLE)
add_executable(test-syncio test-syncio.cpp)
target_link_libraries(test-syncio PRIVATE photon_shared)
add_test(NAME test-syncio COMMAND $<TARGET_FILE:test-syncio>)

add_executable(test-iouring test-iouring.cpp)
target_link_libraries(test-iouring PRIVATE photon_static)
add_test(NAME test-iouring COMMAND $<TARGET_FILE:test-iouring>)
endif ()