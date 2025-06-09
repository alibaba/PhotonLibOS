# Fetch depend code for ecosystem
# Here for header-only parts.

include(FetchContent)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    # set DOWNLOAD_EXTRACT_TIMESTAMP ON cmake 3.24 or above
    cmake_policy(SET CMP0135 NEW)
endif()

# Rapidjson
FetchContent_Declare(
    rapidjson
    GIT_SUBMODULES ""
    GIT_REPOSITORY ${PHOTON_RAPIDJSON_GIT}
    GIT_TAG v1.1.0
    PATCH_COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/patches/rapidjson.patch
    UPDATE_DISCONNECTED 1)
# Pre-set rapidjson build option, prevent building docs, examples and tests
set(RAPIDJSON_BUILD_DOC OFF CACHE BOOL "Build rapidjson documentation.")
set(RAPIDJSON_BUILD_EXAMPLES OFF CACHE BOOL "Build rapidjson documentation.")
set(RAPIDJSON_BUILD_TESTS OFF CACHE BOOL "Build rapidjson perftests and unittests.")
FetchContent_MakeAvailable(rapidjson)
message(STATUS "Rapidjson source dir: ${rapidjson_SOURCE_DIR}")

# RapidXml
FetchContent_Declare(
    rapidxml
    URL ${PHOTON_RAPIDXML_SOURCE}
    URL_HASH
        SHA256=c3f0b886374981bb20fabcf323d755db4be6dba42064599481da64a85f5b3571
    UPDATE_DISCONNECTED 1)
FetchContent_MakeAvailable(rapidxml)
message(STATUS "Rapidxml source dir: ${rapidxml_SOURCE_DIR}")

# RapidYAML
# Single header file so should set DOWNLOAD_NO_EXTRACT
FetchContent_Declare(
    rapidyaml
    URL ${PHOTON_RAPIDYAML_SOURCE}
    URL_HASH
        SHA256=09a5be64f7add04e24a675704a16b3aea7ed80e9ff73d865a3b92301971d0815
    DOWNLOAD_NO_EXTRACT 1
    UPDATE_DISCONNECTED 1)
FetchContent_MakeAvailable(rapidyaml)
if (CMAKE_VERSION VERSION_LESS "3.18.0")
    # Before 3.18, CMake will not copy single header file to src directory
    file(COPY
        ${FETCHCONTENT_BASE_DIR}/rapidyaml-subbuild/rapidyaml-populate-prefix/src/rapidyaml-0.5.0.hpp
        DESTINATION ${rapidyaml_SOURCE_DIR})
endif()
message(STATUS "Rapidyaml source dir: ${rapidyaml_SOURCE_DIR}")

add_library(ecosystem_deps INTERFACE)
target_include_directories(
    ecosystem_deps
    INTERFACE ${rapidjson_SOURCE_DIR}/include ${rapidxml_SOURCE_DIR} ${rapidyaml_SOURCE_DIR}
    )
get_property(
    incs
    TARGET ecosystem_deps
    PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "ecosystem_deps incs: ${incs}")
