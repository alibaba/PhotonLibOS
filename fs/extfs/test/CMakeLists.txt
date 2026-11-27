add_executable(test-extfs test.cpp)
target_link_libraries(test-extfs PRIVATE photon_shared)
add_test(NAME test-extfs COMMAND $<TARGET_FILE:test-extfs>)

add_executable(test-extfs-fsck test_fsck.cpp)
target_link_libraries(test-extfs-fsck PRIVATE photon_shared)
add_test(NAME test-extfs-fsck COMMAND $<TARGET_FILE:test-extfs-fsck>)

if(PHOTON_ENABLE_RESIZE)
    add_executable(test-extfs-resize test_resize.cpp)
    target_link_libraries(test-extfs-resize PRIVATE photon_shared)
    target_compile_definitions(test-extfs-resize PRIVATE PHOTON_ENABLE_RESIZE)
    add_test(NAME test-extfs-resize COMMAND $<TARGET_FILE:test-extfs-resize>)
endif()
