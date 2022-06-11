cmake_minimum_required(VERSION 3.12)

project(
        photon
        VERSION 0.1.0
        LANGUAGES C CXX ASM
)

# Compiler options
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED on)
set(CMAKE_CXX_EXTENSIONS off)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcrc32 -msse4.2 -fPIC")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# CMake dirs
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/output)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/CMake)

include(CPack)

include(FetchContent)
set(FETCHCONTENT_QUIET false)
if (${CMAKE_VERSION} VERSION_LESS 3.14)
    macro(FetchContent_MakeAvailable NAME)
        FetchContent_GetProperties(${NAME})
        if (NOT ${NAME}_POPULATED)
            FetchContent_Populate(${NAME})
            add_subdirectory(${${NAME}_SOURCE_DIR} ${${NAME}_BINARY_DIR})
        endif ()
    endmacro()
endif ()

# Find packages either from cmake-modules or external packages
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

find_package(aio REQUIRED)
find_package(fuse REQUIRED)
find_package(krb5 REQUIRED)
find_package(gsasl REQUIRED)

# Get CPU arch
execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT (${ARCH} STREQUAL x86_64) AND NOT (${ARCH} STREQUAL aarch64))
    message(FATAL_ERROR "Unknown CPU architecture ${ARCH}")
endif ()

# Fetch liburing
FetchContent_Declare(
        liburing
        GIT_REPOSITORY https://github.com/axboe/liburing.git
        GIT_TAG 469b5e80ce42be53ec748ddea8523a3c16ea5465
        UPDATE_COMMAND ./configure
)
FetchContent_MakeAvailable(liburing)
set(liburing_include_dir ${liburing_SOURCE_DIR}/src/include)
add_library(
        uring
        STATIC
        ${liburing_SOURCE_DIR}/src/setup.c
        ${liburing_SOURCE_DIR}/src/queue.c
        ${liburing_SOURCE_DIR}/src/register.c
        ${liburing_SOURCE_DIR}/src/syscall.c
)
target_compile_options(uring PRIVATE -Wno-implicit-function-declaration)
target_include_directories(uring PRIVATE ${liburing_include_dir})

# Fetch boost
FetchContent_Declare(
        boost
        URL https://boostorg.jfrog.io/artifactory/main/release/1.75.0/source/boost_1_75_0.tar.gz
        URL_MD5 38813f6feb40387dfe90160debd71251
)
FetchContent_MakeAvailable(boost)
set(boost_include_dir ${boost_SOURCE_DIR})

# Compile photon objects
file(
        GLOB PHOTON_SRC
        photon.cpp
        common/*.cpp
        common/checksum/*.cpp
        common/executor/*.cpp
        common/memory-stream/*.cpp
        common/stream-messenger/*.cpp
        fs/*.cpp
        fs/httpfs/*.cpp
        io/*.cpp
        net/*.cpp
        net/http/*.cpp
        net/security-context/*.cpp
        rpc/*.cpp
        thread/*.cpp
        thread/switch_context_${ARCH}.s
)
add_library(photon_obj OBJECT ${PHOTON_SRC})
target_include_directories(photon_obj PUBLIC include ${liburing_include_dir} ${boost_include_dir})
target_compile_definitions(photon_obj PRIVATE _FILE_OFFSET_BITS=64 FUSE_USE_VERSION=29)
target_compile_options(photon_obj PRIVATE -Wall)

# Make virtual interface for external dynamic libs
add_library(external_lib INTERFACE)
target_link_libraries(external_lib INTERFACE
        ZLIB::ZLIB
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
        ${AIO_LIBRARIES}
        ${FUSE_LIBRARIES}
        ${KRB5_LIBRARIES}
        ${GSASL_LIBRARIES}
        -lrt
        -lpthread
        )

# Link photon lib
add_library(photon SHARED $<TARGET_OBJECTS:photon_obj>)
target_link_libraries(
        photon
        -Wl,--whole-archive
        uring
        easy_weak
        -Wl,--no-whole-archive
        external_lib
)

add_subdirectory(third_party)

# Build test cases
if (BUILD_TESTING)
    include(CTest)

    find_package(GTest REQUIRED)
    find_package(gmock REQUIRED)
    find_package(gflags REQUIRED)

    set(testing_libs ${GTEST_BOTH_LIBRARIES} ${GMOCK_LIBRARIES} ${GFLAGS_LIBRARIES})

    add_subdirectory(examples)
    add_subdirectory(fs/test)
    add_subdirectory(io/test)
    add_subdirectory(net/test)
    add_subdirectory(net/http/test)
    add_subdirectory(rpc/test)
    add_subdirectory(thread/test)
endif ()
