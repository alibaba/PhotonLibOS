include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_definitions(-w)

add_executable(client_perf client_perf.cpp)
target_link_libraries(client_perf PRIVATE photon_shared ${testing_libs})

add_executable(server_perf server_perf.cpp)
target_link_libraries(server_perf PRIVATE photon_shared ${testing_libs})

add_executable(pure_libcurl pure_libcurl.cpp)
target_link_libraries(pure_libcurl PRIVATE photon_shared ${testing_libs})

#add_executable(client_function_test client_function_test.cpp)
#target_link_libraries(client_function_test PRIVATE photon_shared ${testing_libs})
#add_test(NAME client_function_test COMMAND $<TARGET_FILE:client_function_test>)
#
add_executable(server_function_test server_function_test.cpp)
target_link_libraries(server_function_test PRIVATE photon_shared ${testing_libs})
add_test(NAME server_function_test COMMAND $<TARGET_FILE:server_function_test>)

add_executable(cookie_jar_test cookie_jar_test.cpp)
target_link_libraries(cookie_jar_test PRIVATE photon_shared ${testing_libs})
add_test(NAME cookie_jar_test COMMAND $<TARGET_FILE:cookie_jar_test>)

add_executable(headers_test headers_test.cpp)
target_link_libraries(headers_test PRIVATE photon_shared ${testing_libs})
add_test(NAME headers_test COMMAND $<TARGET_FILE:headers_test>)
