set(TEST_WORKING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/)
add_compile_definitions(DATA_DIR=${TEST_WORKING_DIR})

if (CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64)
    enable_language(ASM_NASM)
    photon_add_test(test-checksum test_checksum.cpp crc32_iscsi_00.asm crc64_ecma_refl_by8.asm)
else()
    photon_add_test(test-checksum test_checksum.cpp crc32_iscsi_crc_ext.S crc64_ecma_refl_pmull.S)
endif()

photon_add_test(test-digest test_digest.cpp INCLUDES ${OPENSSL_INCLUDE_DIRS})
if (APPLE)
    target_link_libraries(test-digest PRIVATE ${OPENSSL_LIBRARIES})
endif ()

photon_add_test(test-xxhash test_xxhash.cpp)
