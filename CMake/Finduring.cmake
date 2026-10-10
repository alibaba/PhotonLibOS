include(photon-find-helpers)

# Locate the installed headers and library without pinning a shared-library
# filename. Compatible newer releases and static installs have different names.
photon_find_package(uring HEADERS liburing.h LIBRARIES uring)

# This file is reached only when liburing is NOT built from source -- the version
# photon downloads is pinned by PHOTON_URING_SOURCE in the top-level CMakeLists.
# What a SYSTEM liburing owes us is a capability, not a release number: uring_cmd
# and the 128-byte SQEs its 32-byte control command does not fit into otherwise.
#
# The requirement is photon's own, not one caller's: io/iouring-wrapper.cpp declares
# and defines iouring_uring_cmd with no #ifdef, so PHOTON_ENABLE_URING compiles it
# whether or not anything in-tree calls it. blk/ublk.cpp is the caller today -- both
# of its rings issue IORING_OP_URING_CMD -- but a liburing without the capability
# breaks the uring build with or without ublk.
#
# Probing rather than naming a release is also what lets the distro packages
# (Debian 12, Ubuntu 24.04, Fedora 40) configure, where an exact-version check
# rejected all of them. This is deliberately narrower than dropping the check
# outright: a liburing that cannot prep a uring_cmd on an SQE128 ring is refused
# here, at configure time, instead of failing to compile iouring-wrapper.cpp.
#
# The probe mirrors iouring_uring_cmd's own use of the API: prep a uring_cmd and
# copy a payload into sqe->cmd, on a ring asked for IORING_SETUP_SQE128.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${URING_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${URING_LIBRARIES})
check_cxx_source_compiles("
#include <liburing.h>
#include <cstring>
int main() {
    io_uring ring;
    io_uring_params params{};
    params.flags |= IORING_SETUP_SQE128;
    io_uring_queue_init_params(1, &ring, &params);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_uring_cmd(sqe, 0, -1);
    char payload[32] = {};
    std::memcpy(sqe->cmd, payload, sizeof(payload));
    io_uring_queue_exit(&ring);
    return 0;
}
" URING_SUPPORTS_URING_CMD)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

if (NOT URING_SUPPORTS_URING_CMD)
    # No release number here, deliberately: this file probes the capability instead,
    # so a version named in the failure would be a second, unenforced requirement that
    # drifts from the pin in the top-level CMakeLists -- the previous text recommended
    # "2.3 or newer" against a 2.15 pin.
    message(FATAL_ERROR "The liburing at ${URING_LIBRARIES} has no io_uring_prep_uring_cmd / IORING_SETUP_SQE128, which PHOTON_ENABLE_URING needs for io/iouring-wrapper.cpp's iouring_uring_cmd whatever callers are enabled. Install a liburing that provides both, or let photon build the pinned one with -D PHOTON_BUILD_DEPENDENCIES=ON (PHOTON_URING_SOURCE in the top-level CMakeLists names it).")
endif ()
