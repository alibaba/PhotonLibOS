include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_definitions(-w)

add_executable(signalfdtest signalfdtest.cpp)
target_link_libraries(signalfdtest PRIVATE photon_shared ${testing_libs})
add_test(NAME signalfdtest COMMAND $<TARGET_FILE:signalfdtest>)

add_executable(signalfdboom signalfdboom.cpp)
target_link_libraries(signalfdboom PRIVATE photon_shared ${testing_libs})
add_test(NAME signalfdboom COMMAND $<TARGET_FILE:signalfdboom>)

if (NOT APPLE)
add_executable(test-syncio test-syncio.cpp)
target_link_libraries(test-syncio PRIVATE photon_shared ${testing_libs})
add_test(NAME test-syncio COMMAND $<TARGET_FILE:test-syncio>)

add_executable(test-iouring test-iouring.cpp)
<<<<<<< HEAD
target_link_libraries(test-iouring PRIVATE photon_shared ${testing_libs})
=======
target_link_libraries(test-iouring PRIVATE photon_shared)
if (PHOTON_ENABLE_URING)
    target_include_directories(test-iouring PRIVATE ${URING_INCLUDE_DIRS})
    target_link_libraries(test-iouring PRIVATE ${URING_LIBRARIES})
    target_compile_definitions(test-iouring PRIVATE PHOTON_URING=on)
endif()
>>>>>>> d92ae33 ([Backport][0.8 to 0.7] | | fix(io): io_uring completion draining and SQE reset fixes (#1671, #1696) (#1693, #1701) (#1704) (#1707))
add_test(NAME test-iouring COMMAND $<TARGET_FILE:test-iouring>)

add_executable(test-iouring-uaf test-iouring-uaf.cpp)
target_link_libraries(test-iouring-uaf PRIVATE photon_shared ${testing_libs})
add_test(NAME test-iouring-uaf COMMAND $<TARGET_FILE:test-iouring-uaf>)
endif ()