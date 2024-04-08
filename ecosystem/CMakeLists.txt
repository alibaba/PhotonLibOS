# Fetch depend code for ecosystem

include(FetchContent)

# Rapidjson
FetchContent_Declare(
    rapidjson
    URL ${PHOTON_RAPIDJSON_SOURCE}
    URL_HASH SHA256=8e00c38829d6785a2dfb951bb87c6974fa07dfe488aa5b25deec4b8bc0f6a3ab
    PATCH_COMMAND patch -p1 < ${CMAKE_CURRENT_SOURCE_DIR}/patches/rapidjson.patch
    UPDATE_DISCONNECTED 1)
set(RAPIDJSON_BUILD_DOC OFF)
set(RAPIDJSON_BUILD_EXAMPLES OFF)
set(RAPIDJSON_BUILD_TESTS OFF)
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

add_library(ecosystem_deps INTERFACE)
target_include_directories(
    ecosystem_deps
    INTERFACE ${rapidjson_SOURCE_DIR}/include ${rapidxml_SOURCE_DIR})
get_property(
    incs
    TARGET ecosystem_deps
    PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "ecosystem_deps incs: ${incs}")
