set(TEST_WORKING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/)
add_definitions(-DDATA_DIR=${TEST_WORKING_DIR})

add_executable(test-checksum test_checksum.cpp)
target_link_libraries(test-checksum PRIVATE photon_shared)
add_test(NAME test-checksum COMMAND $<TARGET_FILE:test-checksum>)