set(TEST_WORKING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/)
add_definitions(-DDATA_DIR=${TEST_WORKING_DIR})

if (${ARCH} STREQUAL x86_64)
    enable_language(ASM_NASM)
    add_executable(test-checksum test_checksum.cpp crc32_iscsi_00.asm crc64_ecma_refl_by8.asm)
else()
    add_executable(test-checksum test_checksum.cpp crc32_iscsi_crc_ext.S crc64_ecma_refl_pmull.S)
endif()

target_link_libraries(test-checksum PRIVATE photon_shared)
add_test(NAME test-checksum COMMAND $<TARGET_FILE:test-checksum>)
