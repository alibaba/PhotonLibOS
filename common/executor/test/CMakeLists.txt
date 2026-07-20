add_compile_options(-w)

photon_add_test(test-executor-async test_async.cpp LIBS ${GOOGLETEST_GTEST_MAIN_LIBRARIES})

# add_executable(test-executor-easy test_easy.cpp ../../../third_party/easy_weak/easy_weak.cpp)
# target_include_directories(ci-tools PRIVATE third_party/easy_weak)
# target_link_libraries(test-executor-easy PRIVATE photon_shared ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
# add_test(NAME test-executor-easy COMMAND $<TARGET_FILE:test-executor-easy>)

# add_executable(test-executor-easyexport test_easyexport.cpp ../../../third_party/easy_weak/easy_weak.cpp)
# target_include_directories(ci-tools PRIVATE third_party/easy_weak)
# target_link_libraries(test-executor-easyexport PRIVATE photon_shared ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
# add_test(NAME test-executor-easyexport COMMAND $<TARGET_FILE:test-executor-easyexport>)

photon_add_test(test-executor-export_as_executor test_export_as_executor.cpp LIBS ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
photon_add_test(test-executor-std test_std.cpp LIBS ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
photon_add_test(test-executor-burst-drain test_burst_drain.cpp LIBS ${GOOGLETEST_GTEST_MAIN_LIBRARIES})
