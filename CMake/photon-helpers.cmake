# Declarative helpers that replace the repetitive option / source-URL / feature
# / dependency if-chains in the top-level CMakeLists.txt with one readable line
# each. Behavior is intentionally identical to the hand-written blocks they
# supersede; they only remove boilerplate.

# A user-facing cache knob, declared with one uniform form instead of mixing
# option() (booleans) with set(... CACHE STRING) (string values).
#   photon_option(<name> BOOL|STRING <default> <help>)
# option(X "help" OFF) is exactly set(X OFF CACHE BOOL "help"), so BOOL knobs
# behave identically to the option() they replace.
macro(photon_option name type default help)
    set(${name} ${default} CACHE ${type} "${help}")
endmacro ()

# A build-from-source download location, collapsing the repetitive
# set(PHOTON_..._SOURCE "url" CACHE STRING "") table into one line each.
# <key> carries the suffix (SOURCE or GIT) so the resulting cache variable name
# (PHOTON_<key>) matches the existing convention.
macro(photon_source key url)
    set(PHOTON_${key} "${url}" CACHE STRING "")
endmacro ()

# Append <name> to the `dependencies` list. Any trailing arguments form a
# condition evaluated by if(); with no condition the dependency is
# unconditional. Keeps each dependency on a single declarative line with its
# enabling condition inline, in place of a long if() / list(APPEND) chain.
macro(photon_dependency name)
    if (${ARGC} GREATER 1)
        if (${ARGN})
            list(APPEND dependencies ${name})
        endif ()
    else ()
        list(APPEND dependencies ${name})
    endif ()
endmacro ()

# target_compile_definitions(photon_features INTERFACE <defs>), guarded by a
# condition:
#   photon_feature(IF <cond...> DEFS <defs...>)
# <cond...> is any expression if() accepts; the definitions are added only when
# it holds. Mirrors the per-feature if() blocks with one line each.
function(photon_feature)
    cmake_parse_arguments(_pf "" "" "IF;DEFS" ${ARGN})
    if (${_pf_IF})
        target_compile_definitions(photon_features INTERFACE ${_pf_DEFS})
    endif ()
endfunction ()

# list(APPEND <list> <items...>), guarded by a condition:
#   photon_append_if(<list> IF <cond...> ITEMS <items...>)
# The items (possibly empty, e.g. an unset *_LIBRARIES) are appended only when
# <cond...> holds. Replaces the if() / list(APPEND) blocks that assemble the
# link-time dependency lists while preserving their append order.
function(photon_append_if listvar)
    cmake_parse_arguments(_pa "" "" "IF;ITEMS" ${ARGN})
    if (${_pa_IF})
        list(APPEND ${listvar} ${_pa_ITEMS})
        set(${listvar} "${${listvar}}" PARENT_SCOPE)
    endif ()
endfunction ()

# list(REMOVE_ITEM <list> <items...>), guarded by a condition -- the opt-out
# counterpart of photon_append_if:
#   photon_remove_if(<list> IF <cond...> ITEMS <items...>)
# Used where a file is globbed in by default and dropped when a feature is off.
function(photon_remove_if listvar)
    cmake_parse_arguments(_pr "" "" "IF;ITEMS" ${ARGN})
    if (${_pr_IF})
        list(REMOVE_ITEM ${listvar} ${_pr_ITEMS})
        set(${listvar} "${${listvar}}" PARENT_SCOPE)
    endif ()
endfunction ()
