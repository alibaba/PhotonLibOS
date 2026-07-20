include(photon-find-helpers)

set(URING_VERSION 2.3)

photon_find_package(uring HEADERS liburing.h LIBRARIES uring)

get_filename_component(URING_LIB_BASE ${URING_LIBRARIES} DIRECTORY)
if (NOT EXISTS "${URING_LIB_BASE}/liburing.so.${URING_VERSION}")
    message(FATAL_ERROR "Requires liburing ${URING_VERSION}. Install it to system or try -D PHOTON_BUILD_DEPENDENCIES=ON")
endif ()
