add_definitions(-w)

add_executable(perf_usleepdefer_semaphore perf_usleepdefer_semaphore.cpp)
target_link_libraries(perf_usleepdefer_semaphore PRIVATE photon_shared)
add_test(NAME perf_usleepdefer_semaphore COMMAND $<TARGET_FILE:perf_usleepdefer_semaphore>)

add_executable(test-thread test.cpp x.cpp)
target_link_libraries(test-thread PRIVATE photon_static)
add_test(NAME test-thread COMMAND $<TARGET_FILE:test-thread>)

add_executable(test-std-compat test-std-compat.cpp)
target_link_libraries(test-std-compat PRIVATE photon_shared)
add_test(NAME test-std-compat COMMAND $<TARGET_FILE:test-std-compat>)

add_executable(test-specific-key test-specific-key.cpp)
target_link_libraries(test-specific-key PRIVATE photon_shared)
add_test(NAME test-specific-key COMMAND $<TARGET_FILE:test-specific-key>)

add_executable(test-thread-local test-thread-local.cpp)
target_link_libraries(test-thread-local PRIVATE photon_shared)
add_test(NAME test-thread-local COMMAND $<TARGET_FILE:test-thread-local>)

add_executable(test-tls-order-native test-tls-order-native.cpp)
target_link_libraries(test-tls-order-native PRIVATE photon_shared)
add_test(NAME test-tls-order-native COMMAND $<TARGET_FILE:test-tls-order-native>)

add_executable(test-tls-order-photon test-tls-order-photon.cpp)
target_link_libraries(test-tls-order-photon PRIVATE photon_shared)
add_test(NAME test-tls-order-photon COMMAND $<TARGET_FILE:test-tls-order-photon>)

add_executable(test-lib-data test-lib-data.cpp)
target_link_libraries(test-lib-data PRIVATE photon_shared)
add_test(NAME test-lib-data COMMAND $<TARGET_FILE:test-lib-data>)

add_executable(test-multi-vcpu-locking test-multi-vcpu-locking.cpp)
target_link_libraries(test-multi-vcpu-locking PRIVATE photon_static)
add_test(NAME test-multi-vcpu-locking COMMAND $<TARGET_FILE:test-multi-vcpu-locking>)