<<<<<<< HEAD
add_executable(test-rpc test.cpp)
target_link_libraries(test-rpc PRIVATE photon_shared)
add_test(NAME test-rpc COMMAND $<TARGET_FILE:test-rpc>)

add_executable(test-ooo test-ooo.cpp)
target_link_libraries(test-ooo PRIVATE photon_shared)
add_test(NAME test-ooo COMMAND $<TARGET_FILE:test-ooo>)

add_executable(test-rpc-message test-rpc-message.cpp)
target_link_libraries(test-rpc-message PRIVATE photon_shared)
add_test(NAME test-rpc-message COMMAND $<TARGET_FILE:test-rpc-message>)
=======
photon_add_test(test-rpc test.cpp)
photon_add_test(test-ooo test-ooo.cpp)
photon_add_test(test-ooo-timeout test-ooo-timeout.cpp)
photon_add_test(test-rpc-message test-rpc-message.cpp)
>>>>>>> af668b9 (rpc: fix OOO engine stack context UAF (#1291) (#1570))
