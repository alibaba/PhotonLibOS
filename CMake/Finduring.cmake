<<<<<<< HEAD
set(URING_VERSION 2.3)

find_path(URING_INCLUDE_DIRS liburing.h)

find_library(URING_LIBRARIES uring)

find_package_handle_standard_args(uring DEFAULT_MSG URING_LIBRARIES URING_INCLUDE_DIRS)

mark_as_advanced(URING_INCLUDE_DIRS URING_LIBRARIES)

get_filename_component(URING_LIB_BASE ${URING_LIBRARIES} DIRECTORY)
if (NOT EXISTS "${URING_LIB_BASE}/liburing.so.${URING_VERSION}")
    message(FATAL_ERROR "Requires liburing ${URING_VERSION}. Install it to system or try -D PHOTON_BUILD_DEPENDENCIES=ON")
endif ()
=======
include(photon-find-helpers)

# Locate the installed headers and library without pinning a shared-library
# filename. Compatible newer releases and static installs have different names.
photon_find_package(uring HEADERS liburing.h LIBRARIES uring)
>>>>>>> 92a7781 (fix(cmake): accept installed liburing without pinning its filename (#1672))
