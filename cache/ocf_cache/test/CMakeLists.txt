include_directories($ENV{GFLAGS}/include)
link_directories($ENV{GFLAGS}/lib)

find_package(curl REQUIRED)

add_executable(ocf_perf_test ocf_perf_test.cpp)
target_include_directories(
        ocf_perf_test PUBLIC
        ${CURL_INCLUDE_DIRS}
        ${PHOTON_INCLUDE_DIR}
)

target_link_libraries(ocf_perf_test 
  PRIVATE 
  photon_shared
  cache_lib
)


add_test(
        NAME ocf_perf_test
        COMMAND ${EXECUTABLE_OUTPUT_PATH}/ocf_perf_test --ut_pass=true
)
