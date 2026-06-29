add_executable(test-qat-lz4 test-qat-lz4.cpp)
target_link_libraries(test-qat-lz4 PRIVATE photon_shared)
target_include_directories(test-qat-lz4 PRIVATE
    ${CMAKE_SOURCE_DIR}/ecosystem/qat
    ${QAT_INCLUDE_DIRS}
)
add_test(NAME test-qat-lz4 COMMAND $<TARGET_FILE:test-qat-lz4>)
