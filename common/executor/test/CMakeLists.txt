add_definitions(-w)

add_executable(test-executor-async test_async.cpp)
target_link_libraries(test-executor-async PRIVATE photon_shared ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
add_test(NAME test-executor-async COMMAND $<TARGET_FILE:test-executor-async>)

# add_executable(test-executor-easy test_easy.cpp ../../../third_party/easy_weak/easy_weak.cpp)
# target_include_directories(ci-tools PRIVATE third_party/easy_weak)
# target_link_libraries(test-executor-easy PRIVATE photon_shared ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
# add_test(NAME test-executor-easy COMMAND $<TARGET_FILE:test-executor-easy>)

# add_executable(test-executor-easyexport test_easyexport.cpp ../../../third_party/easy_weak/easy_weak.cpp)
# target_include_directories(ci-tools PRIVATE third_party/easy_weak)
# target_link_libraries(test-executor-easyexport PRIVATE photon_shared ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
# add_test(NAME test-executor-easyexport COMMAND $<TARGET_FILE:test-executor-easyexport>)

add_executable(test-executor-export_as_executor test_export_as_executor.cpp)
target_link_libraries(test-executor-export_as_executor PRIVATE photon_shared ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
add_test(NAME test-executor-export_as_executor COMMAND $<TARGET_FILE:test-executor-export_as_executor>)

add_executable(test-executor-std test_std.cpp)
target_link_libraries(test-executor-std PRIVATE photon_shared ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
add_test(NAME test-executor-std COMMAND $<TARGET_FILE:test-executor-std>)

