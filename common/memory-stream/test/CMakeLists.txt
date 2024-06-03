add_definitions(-w)

add_executable(test-memory-stream test.cpp)
target_link_libraries(test-memory-stream PRIVATE photon_shared)
add_test(NAME test-memory-stream COMMAND $<TARGET_FILE:test-memory-stream>)

