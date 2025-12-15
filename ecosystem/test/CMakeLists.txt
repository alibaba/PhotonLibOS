add_executable(test-ecosystem test.cpp test_simple_dom.cpp test_redis.cpp)
target_link_libraries(test-ecosystem PRIVATE photon_shared)
add_test(NAME test-ecosystem COMMAND $<TARGET_FILE:test-ecosystem>)

#add_executable(test-oss test_oss.cpp)
#target_link_libraries(test-oss PRIVATE photon_shared)
#add_test(NAME test-oss COMMAND $<TARGET_FILE:test-oss>)
