add_definitions(-w)

add_executable(test-extfs test.cpp)
target_link_libraries(test-extfs PRIVATE photon_shared)

add_test(NAME test-extfs COMMAND $<TARGET_FILE:test-extfs>)
