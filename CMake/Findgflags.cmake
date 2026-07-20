include(photon-find-helpers)

if (NOT CMAKE_SYSTEM_NAME STREQUAL CMAKE_HOST_SYSTEM_NAME)
    # Cross-compiling: don't find system libs, build from source instead
    set(GFLAGS_INCLUDE_DIRS "" CACHE STRING "")
    set(GFLAGS_LIBRARIES "" CACHE STRING "")
    return()
endif ()

photon_find_package(gflags HEADERS gflags/gflags.h LIBRARIES gflags)
