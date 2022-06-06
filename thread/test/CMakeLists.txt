include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_definitions(-w)

add_executable(perf_usleepdefer_semaphore perf_usleepdefer_semaphore.cpp)
target_link_libraries(perf_usleepdefer_semaphore PRIVATE photon ${testing_libs})
add_test(NAME perf_usleepdefer_semaphore COMMAND $<TARGET_FILE:perf_usleepdefer_semaphore>)

add_executable(test-thread test.cpp x.cpp)
target_link_libraries(test-thread PRIVATE photon ${testing_libs})
add_test(NAME test-thread COMMAND $<TARGET_FILE:test-thread>)
