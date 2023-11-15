SET(TEST_WORKING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/)
ADD_DEFINITIONS(-w -DDATA_DIR=${TEST_WORKING_DIR})

add_executable(test-objcache test_objcache.cpp)
target_link_libraries(test-objcache PRIVATE photon_shared)
add_test(NAME test-objcache COMMAND $<TARGET_FILE:test-objcache>)

add_executable(test-common test.cpp)
target_link_libraries(test-common PRIVATE photon_shared)
add_test(NAME test-common COMMAND $<TARGET_FILE:test-common>)

add_executable(test-scalepool test_scalepool.cpp)
target_link_libraries(test-scalepool PRIVATE photon_shared)
add_test(NAME test-scalepool COMMAND $<TARGET_FILE:test-scalepool>)

add_executable(test-throttle test_throttle.cpp)
target_link_libraries(test-throttle PRIVATE photon_shared)
add_test(NAME test-throttle COMMAND $<TARGET_FILE:test-throttle>)

add_executable(test-constexprstr test_constexprstr.cpp)
target_link_libraries(test-constexprstr PRIVATE photon_shared)
add_test(NAME test-constexprstr COMMAND $<TARGET_FILE:test-constexprstr>)

#add_executable(test-lockfree test_lockfree.cpp)
#target_link_libraries(test-lockfree PRIVATE photon_shared)
#add_test(NAME test-lockfree COMMAND $<TARGET_FILE:test-lockfree>)

add_executable(test-alog test_alog.cpp x.cpp)
target_link_libraries(test-alog PRIVATE photon_shared)
add_test(NAME test-alog COMMAND $<TARGET_FILE:test-alog>)
