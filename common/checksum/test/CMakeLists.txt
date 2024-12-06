SET(TEST_WORKING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/)
ADD_DEFINITIONS(-w -DDATA_DIR=${TEST_WORKING_DIR})

add_executable(test-checksum test_checksum.cpp)
target_link_libraries(test-checksum PRIVATE photon_shared)
add_test(NAME test-checksum COMMAND $<TARGET_FILE:test-checksum>)