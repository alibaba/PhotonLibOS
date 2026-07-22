## Central registry that exposes each resolved dependency as an internal target
## Photon::<name>, so include dirs / definitions / link libraries propagate
## through the target graph instead of loose <DEP>_INCLUDE_DIRS / <DEP>_LIBRARIES
## variables.
##
## Both dependency acquisition paths -- find_package (the Find<pkg>.cmake modules)
## and build_from_src (ExternalProject) -- publish the same conventional
## variables. The top-level dependency loop feeds those into photon_declare_dep(),
## so a dependency looks identical to consumers regardless of where it came from.
##
## Note: targets carry compile usage requirements (include dirs, definitions) plus
## the dependency's link libraries. Photon's own libraries still assemble their
## final link line manually (whole-archive re-export cannot be expressed with
## targets under CMake 3.14), so the link libraries recorded here are informative
## for consumers rather than the source Photon itself links from.
include_guard(GLOBAL)

# photon_declare_dep(<name> [INCLUDES <dir>...] [LIBRARIES <lib>...] [DEFINITIONS <def>...])
#
#   Creates the INTERFACE library _photon_dep_<name> holding the usage
#   requirements and exposes it as the alias Photon::<name>. All keyword groups
#   are optional; empty groups are ignored, so it is safe to call with whatever
#   the acquisition path happened to publish.
function(photon_declare_dep name)
    cmake_parse_arguments(_dep "" "" "INCLUDES;LIBRARIES;DEFINITIONS" ${ARGN})
    set(_tgt _photon_dep_${name})
    if (NOT TARGET ${_tgt})
        add_library(${_tgt} INTERFACE)
        add_library(Photon::${name} ALIAS ${_tgt})
    endif ()
    if (_dep_INCLUDES)
        target_include_directories(${_tgt} INTERFACE ${_dep_INCLUDES})
    endif ()
    if (_dep_LIBRARIES)
        target_link_libraries(${_tgt} INTERFACE ${_dep_LIBRARIES})
    endif ()
    if (_dep_DEFINITIONS)
        target_compile_definitions(${_tgt} INTERFACE ${_dep_DEFINITIONS})
    endif ()
endfunction()
