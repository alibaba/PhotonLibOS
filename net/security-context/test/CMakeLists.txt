photon_add_test(test-tls test.cpp INCLUDES ${OPENSSL_INCLUDE_DIRS})

if (ENABLE_SASL AND NOT (APPLE AND (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64)))
    photon_add_test(test-sasl test-sasl.cpp)
endif ()
