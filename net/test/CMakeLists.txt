include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_definitions(-w)

add_executable(test-socket test.cpp)
target_link_libraries(test-socket PRIVATE photon ${testing_libs})
add_test(NAME test-socket COMMAND $<TARGET_FILE:test-socket>)
