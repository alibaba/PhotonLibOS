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
target_link_libraries(test-iouring PRIVATE photon_shared ${testing_libs})
add_test(NAME test-iouring COMMAND $<TARGET_FILE:test-iouring>)
endif ()