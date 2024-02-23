add_definitions(-w)

add_executable(test-fs test.cpp)
target_link_libraries(test-fs PRIVATE photon_shared)
add_test(NAME test-fs COMMAND $<TARGET_FILE:test-fs>)

# add_executable(test-exportfs test_exportfs.cpp)
# target_link_libraries(test-exportfs PRIVATE photon_static)
# add_test(NAME test-exportfs COMMAND $<TARGET_FILE:test-exportfs>)

add_executable(test-filecopy test_filecopy.cpp)
target_link_libraries(test-filecopy PRIVATE photon_shared)
add_test(NAME test-filecopy COMMAND $<TARGET_FILE:test-filecopy>)

add_executable(test-throttled test_throttledfile.cpp)
target_link_libraries(test-throttled PRIVATE photon_shared)
add_test(NAME test-throttled COMMAND $<TARGET_FILE:test-throttled>)