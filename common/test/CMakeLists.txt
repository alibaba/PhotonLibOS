set(TEST_WORKING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/)
add_compile_definitions(DATA_DIR=${TEST_WORKING_DIR})

photon_add_test(test-objcache test_objcache.cpp)

# Note: perf-objcache is intentionally registered to run the test-objcache binary.
photon_add_test(perf-objcache perf_objcache.cpp NO_REGISTER)
add_test(NAME perf-objcache COMMAND $<TARGET_FILE:test-objcache>)

photon_add_test(test-common test.cpp)
photon_add_test(test-scalepool test_scalepool.cpp)
photon_add_test(test-throttle test_throttle.cpp)
photon_add_test(test-constexprstr test_constexprstr.cpp)
photon_add_test(test-lockfree test_lockfree.cpp)
photon_add_test(test-alog test_alog.cpp x.cpp)
photon_add_test(perf-rcuptr perf_rcuptr.cpp NO_REGISTER)
photon_add_test(perf-alog perf_alog.cpp NO_REGISTER)
photon_add_test(test-delegates test_delegates.cpp)
photon_add_test(test-singleflight test_singleflight.cpp)
