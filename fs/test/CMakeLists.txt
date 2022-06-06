include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_definitions(-w)

add_executable(test-fs test.cpp)
target_link_libraries(test-fs PRIVATE photon ${testing_libs})
add_test(NAME test-fs COMMAND $<TARGET_FILE:test-fs>)
