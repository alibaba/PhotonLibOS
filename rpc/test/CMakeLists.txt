include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_definitions(-w)

if(PHOTON_BUILD_SHARED)
    set(photon_link_lib photon_shared)
else()
    set(photon_link_lib photon_static)
endif()

add_executable(test-rpc test.cpp)
target_link_libraries(test-rpc PRIVATE ${photon_link_lib} ${testing_libs})
add_test(NAME test-rpc COMMAND $<TARGET_FILE:test-rpc>)

add_executable(test-ooo test-ooo.cpp)
target_link_libraries(test-ooo PRIVATE ${photon_link_lib} ${testing_libs})
add_test(NAME test-ooo COMMAND $<TARGET_FILE:test-ooo>)

add_executable(test-rpc-message test-rpc-message.cpp)
target_link_libraries(test-rpc-message PRIVATE ${photon_link_lib} ${testing_libs})
add_test(NAME test-rpc-message COMMAND $<TARGET_FILE:test-rpc-message>)