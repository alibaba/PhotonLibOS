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

################################################################################
# Build configuration
#
# One-shot toolchain setup, moved out of the top-level CMakeLists so that file
# reads as a project manifest (config / defs / structure / deps) rather than a
# wall of per-compiler / per-arch flag plumbing. Behavior is identical to the
# hand-written blocks these replace.

# add_compile_options() scoped with generator expressions so the flags reach C
# and C++ translation units only, never ASM (nasm / gas) -- the CRC kernels, for
# instance, must not receive -Werror or -msseX.
macro(photon_cc_options)
    add_compile_options(
            "$<$<COMPILE_LANGUAGE:C>:${ARGN}>"
            "$<$<COMPILE_LANGUAGE:CXX>:${ARGN}>"
    )
endmacro ()

# photon_cc_options() guarded by a condition, so the top-level CMakeLists can list
# its per-compiler / per-platform / per-arch flags as a readable table:
#   photon_cc_flags(IF <cond...> FLAGS <flags...>)
# <cond...> is any expression if() accepts; the flags reach C and C++ only (never
# ASM). Mirrors photon_feature / photon_append_if -- intent stays in the manifest,
# the C/CXX-only scoping stays here.
function(photon_cc_flags)
    cmake_parse_arguments(_cf "" "" "IF;FLAGS" ${ARGN})
    if (${_cf_IF})
        photon_cc_options(${_cf_FLAGS})
    endif ()
endfunction ()

# Validate the target CPU and register the project-wide toolchain: baseline
# warnings, sanitizer, C++ standard, the LINUX convenience flag, default build
# type, module path and output directories. A macro (not a function) because it
# sets directory-scoped variables (CMAKE_CXX_STANDARD, LINUX, output dirs, ...)
# later commands rely on. Per-configuration optimization levels and the
# per-platform flag table live in the top-level CMakeLists.
macro(photon_configure_build)
    if (NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64) AND NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64) AND NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL arm64))
        message(FATAL_ERROR "Unknown CPU architecture ${CMAKE_SYSTEM_PROCESSOR}")
    endif ()

    photon_cc_options(-Werror -Wall -Wno-error=pragmas)
    if (${CMAKE_CXX_COMPILER_ID} STREQUAL GNU AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
        # Hint: -faligned-new is enabled by default after -std=c++17 (C++ only).
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-faligned-new>")
    endif ()

    if (PHOTON_BUILD_WITH_ASAN)
        if ((NOT CMAKE_BUILD_TYPE STREQUAL "Debug") OR (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"))
            message(FATAL_ERROR "Wrong environment")
        endif ()
        photon_cc_options(-fsanitize=address -static-libasan)
        add_link_options(-fsanitize=address -static-libasan)
    endif ()

    set(CMAKE_CXX_STANDARD ${PHOTON_CXX_STANDARD})
    if (WIN32)
        set(CMAKE_CXX_STANDARD 17)
    endif ()
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    # CMake has no built-in LINUX flag (unlike WIN32 / APPLE)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(LINUX TRUE)
    endif ()

    # Default build type is Release
    if (NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif ()

    # CMake dirs
    list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/CMake)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/output)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/output)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/output)
endmacro ()

# Resolve the PHOTON_ENABLE_FUSE tri-state into the variables the fuse module
# consumes: _fuse_enabled (OFF disables), _fuse_pkg (fuse|fuse3) and
# _fuse_version (29|317). A macro so the results land in the caller's scope.
macro(photon_resolve_fuse)
    if (PHOTON_ENABLE_FUSE STREQUAL "OFF")
        set(_fuse_enabled OFF)
    else ()
        set(_fuse_enabled ON)
    endif ()
    if (PHOTON_ENABLE_FUSE STREQUAL "3")
        set(_fuse_pkg fuse3)
        set(_fuse_version 317)
    else ()
        set(_fuse_pkg fuse)
        set(_fuse_version 29)
    endif ()
endmacro ()

################################################################################
# Module registry
#
# Every buildable unit of libphoton is declared once with photon_module(),
# carrying its sources, internal module deps (REQUIRES), external deps (bucketed
# by how libphoton consumes them), optional gate (OPTION/ENABLE) and defines.
# Declaration only records metadata into GLOBAL properties and computes the
# enable state; the module's INTERFACE target is created later (after the
# Photon::<dep> targets exist) so its usage requirements can reference them.
#
#   photon_module(<name>
#       [OPTION <var>]            # existing cache option gating the module; omit => always-on
#       [ENABLE <cond...>]        # extra if() condition ANDed with OPTION (env / combined)
#       [REQUIRES <mod...>]       # internal module deps -- validated (FATAL_ERROR if disabled)
#       [SOURCES <file-or-glob...>]  # relative to PROJECT_SOURCE_DIR; globs expanded here
#       [STATIC <dep...>]         # external deps linked statically into libphoton
#       [SHARED <dep...>]         # external deps linked dynamically into libphoton
#       [FETCH <dep...>]          # external deps only acquired, not linked into libphoton
#       [ARCHIVE <dep...>]        # external deps additionally AR'd into the distributable libphoton.a
#       [DEFS <def...>]           # compile definitions on the module (reach photon_obj only)
#       [OBJ_LINK <tgt...>])      # extra INTERFACE targets (e.g. ecosystem_deps)
#
# Two orthogonal axes describe a dep's treatment:
#   * link kind -- STATIC / SHARED / FETCH (one per dep). STATIC and SHARED deps
#     are also compile-visible to the module's sources (exposed as Photon::<dep>):
#     anything libphoton links, its sources may include. FETCH deps are acquired
#     for consumers outside libphoton (e.g. gtest/gflags reach the test binaries
#     via ci-tools), so they are neither compile-visible to photon_obj nor linked
#     into libphoton.
#   * ARCHIVE -- an independent flag: the dep's static archive is merged into the
#     distributable libphoton.a. Orthogonal to STATIC (a dep is normally listed in
#     both STATIC and ARCHIVE, to be linked and bundled).
#
# Enabled modules append every bucket to `dependencies` in the caller scope, so
# the acquisition loop finds exactly what the enabled set needs.
function(photon_module name)
    cmake_parse_arguments(_m "" "OPTION"
            "ENABLE;REQUIRES;SOURCES;STATIC;SHARED;FETCH;ARCHIVE;DEFS;OBJ_LINK" ${ARGN})

    # Effective-enable = (OPTION var true, if given) AND (ENABLE cond true, if given).
    set(_enabled TRUE)
    if (_m_OPTION AND NOT ${${_m_OPTION}})
        set(_enabled FALSE)
    endif ()
    if (_enabled AND _m_ENABLE AND NOT (${_m_ENABLE}))
        set(_enabled FALSE)
    endif ()

    # Expand source globs / plain files to absolute paths now, so target_sources()
    # on the module INTERFACE library resolves regardless of the consumer's dir.
    set(_srcs "")
    if (_m_SOURCES)
        file(GLOB _srcs ${_m_SOURCES})
    endif ()

    set_property(GLOBAL APPEND PROPERTY PHOTON_MODULE_LIST ${name})
    set_property(GLOBAL PROPERTY PHOTON_MODULE_${name}_ENABLED  ${_enabled})
    set_property(GLOBAL PROPERTY PHOTON_MODULE_${name}_REQUIRES "${_m_REQUIRES}")
    set_property(GLOBAL PROPERTY PHOTON_MODULE_${name}_SOURCES  "${_srcs}")
    set_property(GLOBAL PROPERTY PHOTON_MODULE_${name}_STATIC   "${_m_STATIC}")
    set_property(GLOBAL PROPERTY PHOTON_MODULE_${name}_SHARED   "${_m_SHARED}")
    set_property(GLOBAL PROPERTY PHOTON_MODULE_${name}_FETCH    "${_m_FETCH}")
    set_property(GLOBAL PROPERTY PHOTON_MODULE_${name}_ARCHIVE  "${_m_ARCHIVE}")
    set_property(GLOBAL PROPERTY PHOTON_MODULE_${name}_DEFS     "${_m_DEFS}")
    set_property(GLOBAL PROPERTY PHOTON_MODULE_${name}_OBJ_LINK "${_m_OBJ_LINK}")

    if (_enabled)
        list(APPEND dependencies ${_m_STATIC} ${_m_SHARED} ${_m_FETCH} ${_m_ARCHIVE})
        set(dependencies "${dependencies}" PARENT_SCOPE)
    endif ()
endfunction ()

# Each enabled module's REQUIRES must also be enabled, else fail loudly.
function(photon_module_validate)
    get_property(_mods GLOBAL PROPERTY PHOTON_MODULE_LIST)
    foreach (m ${_mods})
        get_property(_en GLOBAL PROPERTY PHOTON_MODULE_${m}_ENABLED)
        if (NOT _en)
            continue ()
        endif ()
        get_property(_req GLOBAL PROPERTY PHOTON_MODULE_${m}_REQUIRES)
        foreach (r ${_req})
            get_property(_ren GLOBAL PROPERTY PHOTON_MODULE_${r}_ENABLED)
            if (NOT _ren)
                message(FATAL_ERROR "Photon module '${m}' requires '${r}', which is disabled")
            endif ()
        endforeach ()
    endforeach ()
endfunction ()

# Create the INTERFACE library for every enabled module. Sources compile into
# whatever links the module; REQUIRES becomes an INTERFACE link to the required
# module; STATIC/SHARED deps are exposed as Photon::<dep> compile deps; DEFS and
# OBJ_LINK ride along. Must run after the Photon::<dep> targets exist.
function(photon_module_targets)
    get_property(_mods GLOBAL PROPERTY PHOTON_MODULE_LIST)
    foreach (m ${_mods})
        get_property(_en GLOBAL PROPERTY PHOTON_MODULE_${m}_ENABLED)
        if (NOT _en)
            continue ()
        endif ()
        add_library(photon_mod_${m} INTERFACE)
        get_property(_srcs GLOBAL PROPERTY PHOTON_MODULE_${m}_SOURCES)
        if (_srcs)
            target_sources(photon_mod_${m} INTERFACE ${_srcs})
        endif ()
        get_property(_defs GLOBAL PROPERTY PHOTON_MODULE_${m}_DEFS)
        if (_defs)
            target_compile_definitions(photon_mod_${m} INTERFACE ${_defs})
        endif ()
        get_property(_req GLOBAL PROPERTY PHOTON_MODULE_${m}_REQUIRES)
        get_property(_static GLOBAL PROPERTY PHOTON_MODULE_${m}_STATIC)
        get_property(_shared GLOBAL PROPERTY PHOTON_MODULE_${m}_SHARED)
        get_property(_objlink GLOBAL PROPERTY PHOTON_MODULE_${m}_OBJ_LINK)
        set(_links "")
        foreach (r ${_req})
            list(APPEND _links photon_mod_${r})
        endforeach ()
        # Anything libphoton links (STATIC or SHARED) is also compile-visible to
        # the module's sources; FETCH deps stay invisible to photon_obj.
        foreach (d ${_static} ${_shared})
            list(APPEND _links Photon::${d})
        endforeach ()
        list(APPEND _links ${_objlink})
        if (_links)
            target_link_libraries(photon_mod_${m} INTERFACE ${_links})
        endif ()
    endforeach ()
endfunction ()

# Link every enabled module into <target> (e.g. photon_obj), pulling in their
# INTERFACE sources and usage requirements. CMake de-duplicates sources reached
# through more than one edge, so linking all modules directly is safe.
function(photon_module_link_into target)
    get_property(_mods GLOBAL PROPERTY PHOTON_MODULE_LIST)
    foreach (m ${_mods})
        get_property(_en GLOBAL PROPERTY PHOTON_MODULE_${m}_ENABLED)
        if (_en)
            target_link_libraries(${target} PRIVATE photon_mod_${m})
        endif ()
    endforeach ()
endfunction ()

# Route each enabled module's external-dep link libraries (${<DEP>_LIBRARIES})
# into <static_var> (STATIC) or <shared_var> (SHARED); FETCH deps are acquired but
# never linked. Not de-duplicated across modules, so dpdk shared by fstack_dpdk
# and spdk is appended once per module, matching the hand-written link list.
function(photon_module_collect_libs static_var shared_var)
    get_property(_mods GLOBAL PROPERTY PHOTON_MODULE_LIST)
    foreach (m ${_mods})
        get_property(_en GLOBAL PROPERTY PHOTON_MODULE_${m}_ENABLED)
        if (NOT _en)
            continue ()
        endif ()
        get_property(_static GLOBAL PROPERTY PHOTON_MODULE_${m}_STATIC)
        get_property(_shared GLOBAL PROPERTY PHOTON_MODULE_${m}_SHARED)
        foreach (d ${_static})
            string(TOUPPER ${d} D)
            list(APPEND ${static_var} ${${D}_LIBRARIES})
        endforeach ()
        foreach (d ${_shared})
            string(TOUPPER ${d} D)
            list(APPEND ${shared_var} ${${D}_LIBRARIES})
        endforeach ()
    endforeach ()
    set(${static_var} "${${static_var}}" PARENT_SCOPE)
    set(${shared_var} "${${shared_var}}" PARENT_SCOPE)
endfunction ()

# Collect each enabled module's ARCHIVE deps' static archives (${<DEP>_LIBRARIES})
# into <archive_var>, for merging into the distributable libphoton.a alongside the
# weak-symbol archives. Empty unless a module opts a dep into ARCHIVE; each entry
# must resolve to a static-archive path (non-.a link flags are not meaningful here).
function(photon_module_collect_archives archive_var)
    get_property(_mods GLOBAL PROPERTY PHOTON_MODULE_LIST)
    foreach (m ${_mods})
        get_property(_en GLOBAL PROPERTY PHOTON_MODULE_${m}_ENABLED)
        if (NOT _en)
            continue ()
        endif ()
        get_property(_arch GLOBAL PROPERTY PHOTON_MODULE_${m}_ARCHIVE)
        foreach (d ${_arch})
            string(TOUPPER ${d} D)
            list(APPEND ${archive_var} ${${D}_LIBRARIES})
        endforeach ()
    endforeach ()
    set(${archive_var} "${${archive_var}}" PARENT_SCOPE)
endfunction ()

# Acquire every dependency the enabled module set requires, then expose each as an
# internal Photon::<dep> target so include dirs / defs / link libraries propagate
# through the target graph. Each dep is either built from source (kcp always; the
# rest when PHOTON_BUILD_DEPENDENCIES is on and a PHOTON_<DEP>_SOURCE url exists)
# or located with find_package; both paths publish the same <DEP>_INCLUDE_DIRS /
# <DEP>_LIBRARIES, so one photon_declare_dep() call covers them uniformly. A macro
# (not a function) so build_from_src's PARENT_SCOPE results and find_package's
# cache entries land in the caller's scope, where the later link-line assembly
# (photon_module_collect_libs / _archives) reads them.
macro(photon_acquire_dependencies)
    foreach (dep ${dependencies})
        message(STATUS "Checking dependency ${dep}")
        string(TOUPPER ${dep} DEP)
        set(source_url "${PHOTON_${DEP}_SOURCE}")
        if (dep STREQUAL "kcp")
            message(STATUS "Will build ${dep} from source")
            message(STATUS "    URL: ${source_url}")
            build_from_src(${dep})
        elseif (PHOTON_BUILD_DEPENDENCIES AND (NOT source_url STREQUAL ""))
            message(STATUS "Will build ${dep} from source")
            message(STATUS "    URL: ${source_url}")
            build_from_src(${dep})
        else ()
            message(STATUS "Will find ${dep}")
            find_package(${dep} REQUIRED)
        endif ()
    endforeach ()

    include(photon-deps)
    foreach (dep ${dependencies})
        string(TOUPPER ${dep} DEP)
        photon_declare_dep(${dep} INCLUDES ${${DEP}_INCLUDE_DIRS} LIBRARIES ${${DEP}_LIBRARIES})
    endforeach ()
endmacro ()

################################################################################
# Library assembly
#
# The mechanical parts of turning photon_obj into the distributable libraries,
# kept out of the top-level CMakeLists so its final section reads as "a shared
# lib, a static lib, and a merged archive" rather than link incantations.

# Some acquired static_deps are actually shared libraries (a local find_package
# may hand back a .so/.dylib, since we can't tell static from shared for packages
# not built from source). Whole-archiving those would fail, so any entry whose
# path ends in a shared-library suffix is also appended to <shared_var> for a
# normal link -- for max compatibility. Reads <static_var>, extends <shared_var>.
function(photon_reclassify_shared_deps static_var shared_var)
    if (NOT APPLE)
        set(suffix "\.so$")
    else ()
        set(suffix "\.dylib$" "\.tbd$")
    endif ()
    foreach (dep ${${static_var}})
        foreach (suf ${suffix})
            if (dep MATCHES "${suf}")
                list(APPEND ${shared_var} ${dep})
                break()
            endif ()
        endforeach ()
    endforeach ()
    set(${shared_var} "${${shared_var}}" PARENT_SCOPE)
endfunction ()

# Bake the MinGW C/C++ runtime into libphoton.dll (WIN32) so the produced DLL has
# no external dependency on libgcc_s_seh-1.dll / libstdc++-6.dll /
# libwinpthread-1.dll; test / example executables can then run under wine without
# WINEPATH or extra DLL copies. libwinpthread needs its static archive referenced
# by absolute path wrapped in --whole-archive, else ld auto-links the import lib
# (libwinpthread.dll.a) and the DLL ends up depending on the DLL.
function(photon_bake_mingw_runtime target)
    execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libwinpthread.a
            OUTPUT_VARIABLE MINGW_WINPTHREAD_STATIC
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_link_options(${target} PRIVATE
            -static-libgcc -static-libstdc++)
    if (MINGW_WINPTHREAD_STATIC AND EXISTS "${MINGW_WINPTHREAD_STATIC}")
        target_link_options(${target} PRIVATE
                -Wl,--whole-archive,${MINGW_WINPTHREAD_STATIC},--no-whole-archive)
    endif ()
endfunction ()

# Merge photon_static (+ the easy_weak / fstack_weak archives and any ARCHIVE
# deps) into a single distributable libphoton.a via ar (Linux) or libtool
# (macOS/other). Do NOT link against the resulting target directly. Reads
# archive_deps from the caller scope.
function(photon_merge_static_archive)
    if (TARGET easy_weak)
        set(_easy_archive $<TARGET_FILE:easy_weak>)
    else ()
        set(_easy_archive "")
    endif ()
    if (TARGET fstack_weak)
        set(_extra_archives $<TARGET_FILE:fstack_weak>)
    else ()
        set(_extra_archives "")
    endif ()
    if (LINUX)
        add_custom_target(_photon_static_archive ALL
                COMMAND rm -rf libphoton.a
                COMMAND ${CMAKE_AR} -qcT libphoton.a $<TARGET_FILE:photon_static> ${_easy_archive} ${_extra_archives} ${archive_deps}
                COMMAND ${CMAKE_AR} -M < ${PROJECT_SOURCE_DIR}/tools/libphoton.mri
                DEPENDS photon_static
                WORKING_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
                VERBATIM
        )
    else ()
        add_custom_target(_photon_static_archive ALL
                COMMAND rm -rf libphoton.a
                COMMAND libtool -static -o libphoton.a $<TARGET_FILE:photon_static> ${_easy_archive} ${_extra_archives} ${archive_deps}
                DEPENDS photon_static
                WORKING_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
                VERBATIM
        )
    endif ()
endfunction ()
