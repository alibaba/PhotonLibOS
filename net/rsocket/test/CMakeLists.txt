add_executable(test-rsocket test-rsocket.cpp)
target_link_libraries(test-rsocket PRIVATE photon_shared)
add_test(NAME test-rsocket COMMAND $<TARGET_FILE:test-rsocket>)

