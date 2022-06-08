include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})

add_executable(simple-example simple.cpp)
target_link_libraries(simple-example PRIVATE photon)

add_executable(rpc-example-client rpc/client.cpp rpc/client_main.cpp)
target_link_libraries(rpc-example-client PRIVATE photon ${testing_libs})

add_executable(rpc-example-server rpc/server.cpp rpc/server_main.cpp)
target_link_libraries(rpc-example-server PRIVATE photon ${testing_libs})