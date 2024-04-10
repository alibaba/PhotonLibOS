# Fetch depend code for ecosystem
# Here for header-only parts.

include(FetchContent)

# Rapidjson
FetchContent_Declare(
    rapidjson
    URL ${PHOTON_RAPIDJSON_SOURCE}
    URL_HASH SHA256=8e00c38829d6785a2dfb951bb87c6974fa07dfe488aa5b25deec4b8bc0f6a3ab
    PATCH_COMMAND patch -p1 < ${CMAKE_CURRENT_SOURCE_DIR}/patches/rapidjson.patch
    DOWNLOAD_EXTRACT_TIMESTAMP ON
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
    DOWNLOAD_EXTRACT_TIMESTAMP ON
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
message(STATUS "Rapidyaml source dir: ${rapidyaml_SOURCE_DIR}")

# cpp-redis
FetchContent_Declare(
    cpp-redis
    URL ${PHOTON_CPP_REDIS_SOURCE}
    URL_HASH
        SHA256=3859289d8254685fc775bda73de03dad27df923423b8ceb375b02d036c03b02f
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    UPDATE_DISCONNECTED 1)
# uses only a simple header, so do not add sub directory to avoid unnecessary build
# do not use FetchContent_MakeAvailable, just populate it.
FetchContent_GetProperties(cpp-redis)
if(NOT cpp-redis_POPULATED)
  FetchContent_Populate(cpp-redis)
endif()
message(STATUS "cpp-redis source dir: ${cpp-redis_SOURCE_DIR}")

add_library(ecosystem_deps INTERFACE)
target_include_directories(
    ecosystem_deps
    INTERFACE ${rapidjson_SOURCE_DIR}/include ${rapidxml_SOURCE_DIR} ${rapidyaml_SOURCE_DIR}
    ${cpp-redis_SOURCE_DIR}/includes)
get_property(
    incs
    TARGET ecosystem_deps
    PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "ecosystem_deps incs: ${incs}")
