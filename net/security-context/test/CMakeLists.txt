add_executable(test-tls test.cpp)
target_include_directories(test-tls PRIVATE ${OPENSSL_INCLUDE_DIRS})
target_link_libraries(test-tls PRIVATE photon_shared)
add_test(NAME test-tls COMMAND $<TARGET_FILE:test-tls>)

<<<<<<< HEAD
if (ENABLE_SASL AND (NOT (APPLE AND (${ARCH} STREQUAL arm64))))
    add_executable(test-sasl test-sasl.cpp)
    target_link_libraries(test-sasl PRIVATE photon_shared)
    add_test(NAME test-sasl COMMAND $<TARGET_FILE:test-sasl>)
=======
if (PHOTON_ENABLE_SASL AND NOT (APPLE AND (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64)))
    photon_add_test(test-sasl test-sasl.cpp)
>>>>>>> 1c1fe31 (fix(test): guard test-sasl with PHOTON_ENABLE_SASL (#1703))
endif ()

