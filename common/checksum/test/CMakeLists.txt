include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
SET(TEST_WORKING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/)
ADD_DEFINITIONS(-w -DDATA_DIR=${TEST_WORKING_DIR})

if(PHOTON_BUILD_SHARED)
    set(photon_link_lib photon_shared)
else()
    set(photon_link_lib photon_static)
endif()

add_executable(test-checksum test_checksum.cpp)
target_link_libraries(test-checksum PRIVATE ${photon_link_lib} ${testing_libs})
add_test(NAME test-checksum COMMAND $<TARGET_FILE:test-checksum>)