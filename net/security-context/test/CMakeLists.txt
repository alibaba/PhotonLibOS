add_definitions(-w)

add_executable(test-tls test.cpp)
target_link_libraries(test-tls PRIVATE photon_shared)
add_test(NAME test-tls COMMAND $<TARGET_FILE:test-tls>)

if (ENABLE_SASL AND (NOT (APPLE AND (${ARCH} STREQUAL arm64))))
    add_executable(test-sasl test-sasl.cpp)
    target_link_libraries(test-sasl PRIVATE photon_shared)
    add_test(NAME test-sasl COMMAND $<TARGET_FILE:test-sasl>)
endif ()

