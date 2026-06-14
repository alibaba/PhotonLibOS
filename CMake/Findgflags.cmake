if (NOT CMAKE_SYSTEM_NAME STREQUAL CMAKE_HOST_SYSTEM_NAME)
    # Cross-compiling: don't find system libs, build from source instead
    set(GFLAGS_INCLUDE_DIRS "" CACHE STRING "")
    set(GFLAGS_LIBRARIES "" CACHE STRING "")
    return()
endif ()

find_path(GFLAGS_INCLUDE_DIRS gflags/gflags.h)

find_library(GFLAGS_LIBRARIES gflags)

find_package_handle_standard_args(gflags DEFAULT_MSG GFLAGS_LIBRARIES GFLAGS_INCLUDE_DIRS)

mark_as_advanced(GFLAGS_INCLUDE_DIRS GFLAGS_LIBRARIES)
