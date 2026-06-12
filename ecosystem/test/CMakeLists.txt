<<<<<<< HEAD
add_executable(test-ecosystem test.cpp test_simple_dom.cpp)
=======
add_executable(test-ecosystem test.cpp test_simple_dom.cpp test_redis.cpp test_oss_custom_headers.cpp)
>>>>>>> dab1c19 (feat(oss): support custom headers on all OSS SDK requests (#1312) (#1347))
target_link_libraries(test-ecosystem PRIVATE photon_shared)
add_test(NAME test-ecosystem COMMAND $<TARGET_FILE:test-ecosystem>)
