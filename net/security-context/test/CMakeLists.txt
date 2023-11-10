include_directories(${CMAKE_SOURCE_DIR}/include)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_definitions(-w)

if(PHOTON_BUILD_SHARED)
    set(photon_link_lib photon_shared)
else()
    set(photon_link_lib photon_static)
endif()

add_executable(test-tls test.cpp)
target_link_libraries(test-tls PRIVATE ${photon_link_lib} ${testing_libs})
add_test(NAME test-tls COMMAND $<TARGET_FILE:test-tls>)

if (ENABLE_SASL AND (NOT (APPLE AND (${ARCH} STREQUAL arm64))))
    add_executable(test-sasl test-sasl.cpp)
    target_link_libraries(test-sasl PRIVATE ${photon_link_lib} ${testing_libs})
    add_test(NAME test-sasl COMMAND $<TARGET_FILE:test-sasl>)
endif ()

