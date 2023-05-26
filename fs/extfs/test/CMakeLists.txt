include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_definitions(-w)

add_executable(test-extfs test.cpp)
target_link_libraries(test-extfs PRIVATE photon_shared ${testing_libs})

add_test(NAME test-extfs COMMAND $<TARGET_FILE:test-extfs>)
