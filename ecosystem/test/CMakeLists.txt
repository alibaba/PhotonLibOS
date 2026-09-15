<<<<<<< HEAD
add_executable(test-ecosystem test.cpp test_simple_dom.cpp test_redis.cpp test_oss_custom_headers.cpp)
target_link_libraries(test-ecosystem PRIVATE photon_shared)
add_test(NAME test-ecosystem COMMAND $<TARGET_FILE:test-ecosystem>)
=======
photon_add_test(test-ecosystem test.cpp test_simple_dom.cpp test_redis.cpp test_oss_custom_headers.cpp)
photon_add_test(test-oss-resolve-ip test_oss_resolve_ip.cpp)
>>>>>>> 0d56025 (feat(ecosystem): add OSS `ip_version` option backed by a client-owned resolver (#1640))

#add_executable(test-oss test_oss.cpp)
#target_link_libraries(test-oss PRIVATE photon_shared)
#add_test(NAME test-oss COMMAND $<TARGET_FILE:test-oss>)
