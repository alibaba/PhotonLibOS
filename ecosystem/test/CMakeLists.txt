add_executable(test-ecosystem test.cpp test_simple_dom.cpp)
target_link_libraries(test-ecosystem PRIVATE photon_shared)
add_test(NAME test-ecosystem COMMAND $<TARGET_FILE:test-ecosystem>)
