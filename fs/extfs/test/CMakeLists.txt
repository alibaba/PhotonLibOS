add_executable(test-extfs test.cpp)
target_link_libraries(test-extfs PRIVATE photon_shared)

add_test(NAME test-extfs COMMAND $<TARGET_FILE:test-extfs>)

add_executable(test-extfs-fsck test_fsck.cpp)
target_link_libraries(test-extfs-fsck PRIVATE photon_shared)

add_test(NAME test-extfs-fsck COMMAND $<TARGET_FILE:test-extfs-fsck>)
