# Helper functions that remove the repetitive add_executable / target_link_libraries /
# add_test boilerplate from the per-module test/ and the examples/ CMakeLists.
#
# Included once from the top-level CMakeLists when PHOTON_BUILD_TESTING is ON.

# photon_add_test(<name> <source>...
#                 [LIBS <lib>...] [DEFS <def>...] [INCLUDES <dir>...] [NO_REGISTER])
#
#   Build executable <name> from the given sources, link it against photon_shared
#   (plus any extra LIBS), and register it with CTest via
#   `add_test(NAME <name> COMMAND <name>)` unless NO_REGISTER is given.
function(photon_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NO_REGISTER" "" "LIBS;DEFS;INCLUDES")
    add_executable(${name} ${arg_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE photon_shared ${arg_LIBS})
    if (arg_INCLUDES)
        target_include_directories(${name} PRIVATE ${arg_INCLUDES})
    endif ()
    if (arg_DEFS)
        target_compile_definitions(${name} PRIVATE ${arg_DEFS})
    endif ()
    if (NOT arg_NO_REGISTER)
        add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
    endif ()
endfunction()

# photon_add_example(<name> <source>... [LIBS <lib>...] [DEFS <def>...])
#
#   Build example executable <name> and link it against photon_static. Extra LIBS
#   are placed before photon_static to preserve the historical static link order.
#   gflags is provided at directory scope by examples/CMakeLists.
function(photon_add_example name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "LIBS;DEFS")
    add_executable(${name} ${arg_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE ${arg_LIBS} photon_static)
    if (arg_DEFS)
        target_compile_definitions(${name} PRIVATE ${arg_DEFS})
    endif ()
endfunction()
