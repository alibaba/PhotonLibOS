## Shared helpers for Photon's Find<pkg>.cmake modules.
##
## Most dependencies are located by the same three-step boilerplate: probe for a
## header, probe for a single library, then validate and advertise the result
## via find_package_handle_standard_args. photon_find_package() captures that
## pattern and publishes the results under Photon's naming convention:
##     <PKG>_INCLUDE_DIRS   (located from HEADERS)
##     <PKG>_LIBRARIES      (located from LIBRARIES)
## where <PKG> is the upper-cased package name.
##
## It is deliberately a macro (not a function) so that find_path / find_library /
## find_package_handle_standard_args run in the including module's scope, exactly
## as the hand-written modules used to.
include_guard(GLOBAL)

include(FindPackageHandleStandardArgs)

# photon_find_package(<pkg> HEADERS <header>... LIBRARIES <name>...)
#
#   HEADERS    header name(s) passed to find_path (alternatives, first match wins)
#   LIBRARIES  library name(s) passed to find_library (alternatives, first match wins)
macro(photon_find_package _pkg)
    cmake_parse_arguments(_pfp "" "" "HEADERS;LIBRARIES" ${ARGN})
    string(TOUPPER ${_pkg} _pfp_upper)
    find_path(${_pfp_upper}_INCLUDE_DIRS NAMES ${_pfp_HEADERS})
    find_library(${_pfp_upper}_LIBRARIES NAMES ${_pfp_LIBRARIES})
    find_package_handle_standard_args(${_pkg} DEFAULT_MSG
            ${_pfp_upper}_LIBRARIES ${_pfp_upper}_INCLUDE_DIRS)
    mark_as_advanced(${_pfp_upper}_INCLUDE_DIRS ${_pfp_upper}_LIBRARIES)
endmacro()
